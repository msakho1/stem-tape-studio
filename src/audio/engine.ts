/**
 * AudioEngine — the single authority over what is actually audible.
 *
 * Rules enforced here:
 *  - exactly ONE AudioContext for the whole app, created inside a user gesture;
 *  - persistent per-track graph; only the one-shot AudioBufferSourceNodes are
 *    recreated, tracked by generation so a stale `onended` can never mutate
 *    live state;
 *  - all four sources always receive the IDENTICAL scheduled `startAt` and the
 *    identical offset — never four sequential "play now" calls;
 *  - the playhead is DERIVED from ctx.currentTime through the INTEGRATED rate
 *    curve (see glide.ts), never ticked and never linearly approximated;
 *  - loop seams, chop jumps, reverse flips and loop-mode transitions all use a
 *    dual-source equal-power crossfade — the physical track gain is never
 *    automated for a seam;
 *  - dry ↔ filtered is a COMPLEMENTARY fade (correlated paths), never
 *    equal-power;
 *  - every command answers with an ack, and a rejected command must not be
 *    allowed to light an LED.
 */

import { ANTI_CLICK_MATRIX } from "./antiClick";
import type { Ack, AudioCommand } from "./commands";
import { equalPower, measuredDryWet, sampleCurve, FILTER_FADE_S, SEAM_FADE_S } from "./crossfade";
import { bufferBytes } from "./format";
import { GLIDE_TAU, glideCurve, glideDurationS } from "./glide";
import { allowed, defaultBudget, describeVerdict, formatMiB, judge, SSR_BUDGET, type MemoryBudget } from "./memory";
import { DEFAULT_WINDOW, resolveLoop, TapeTimeline, type LoopWindow } from "./tape";
import { estimateMigration } from "./workletBudget";
import { pairwiseDrift, sharedApplyFrame, type WorkletAck } from "./workletProtocol";
import { preflightWorklet, WorkletTrack, type MigrationStatus, type PreflightResult } from "./workletTrack";
import { FxRack, type FxRackSnapshot } from "./fx/rack";
import { FX_FAMILIES, type FxFamily } from "@/machine/stemPerformance";
import { RecordingController } from "./input/recorder";
import { PerformanceRecorder } from "./export/performanceRecorder";
import { emptyGrid, tapGrid, type GridState } from "./grid";

export type EnginePreference = "node" | "worklet";

export const LOOKAHEAD_S = 0.08;
export const RAMP_TAU = 0.008;
/** How far ahead the seam scheduler commits work, seconds. */
export const SEAM_LOOKAHEAD_S = 0.25;
const SCHEDULER_INTERVAL_MS = 25;

export type TrackId = 0 | 1 | 2 | 3;

interface LiveSource {
  node: AudioBufferSourceNode;
  /** Per-source fade gain: seams live here, never on the physical trackGain. */
  fade: GainNode;
  gen: number;
  startAt: number;
  /** Media position this source is playing at `startAt`. */
  startPos: number;
  /** Scheduled stop time, if any. */
  stopAt: number | null;
}

export interface TrackRuntime {
  buffer: AudioBuffer | null;
  /** Lazily built reverse copy; costs one extra buffer while resident. */
  reversed: AudioBuffer | null;
  /** Deleted buffers go here, recoverable until the project is compacted. */
  trash: AudioBuffer | null;
  input: GainNode;
  /** Post-filter unity tap feeding the permanent FX rack input. */
  preFx: GainNode;
  /** Phase 5C per-stem FX rack. Its input node is never re-parented. */
  fxRack: FxRack | null;
  /** Post-FX fader — it rides the FX tails, so mute never lives here. */
  gain: GainNode;
  /** Separate, smoothed solo gain. Solo never mutates saved mute state. */
  soloGain: GainNode;
  soloed: boolean;
  linked: boolean;
  fxVariation: Record<FxFamily, number>;
  fxActive: Record<FxFamily, boolean>;
  dry: GainNode;
  wet: GainNode;
  filter: BiquadFilterNode;
  analyser: AnalyserNode;
  sources: LiveSource[];
  generation: number;
  muted: boolean;
  level: number;
  loop: LoopWindow;
  /** Bumped on every rate/loop change: invalidates pending seams. */
  seamGeneration: number;
  /** Context time of the seam already committed to the graph, if any. */
  committedSeamAt: number | null;
  filterMode: "off" | "lp" | "hp";
  /** Exact context time this track's current source was scheduled to start. */
  scheduledStartAt: number | null;
  /** Where the bytes came from — user audio is never network-fetched. */
  provenance: "user-private" | "bundled-demo" | null;
  name: string | null;
  /** Decode instrumentation (Phase 4.1): must be exactly 1 per load. */
  decodeCount: number;
  decodeMs: number | null;
  bufferReused: boolean;
  seamCount: number;
  /** Source duration in seconds, retained after the node PCM is released. */
  sourceDurationS: number;
  /** Phase 5B: per-track worklet controller, null while on the node engine. */
  worklet: WorkletTrack | null;
  engineMode: "node" | "worklet";
  migrationStatus: MigrationStatus;
  fallbackReason: string | null;
  /** Node PCM released after a successful, phase-checked handoff. */
  nodeReleased: boolean;
}

export interface EngineStatus {
  contextState: AudioContextState | "none";
  sampleRate: number | null;
  currentTime: number;
  requestedPlaying: boolean;
  actuallyPlaying: boolean;
  position: number;
  duration: number;
  rate: number;
  targetRate: number;
  masterGain: number;
  startSpreadMs: number;
  tracks: {
    id: number;
    decoded: boolean;
    sourceLive: boolean;
    generation: number;
    gain: number;
    muted: boolean;
    scheduledStartAt: number | null;
    name: string | null;
    provenance: string | null;
    trashed: boolean;
    decodedBytes: number;
    decodeCount: number;
    decodeMs: number | null;
    bufferReused: boolean;
    loop: LoopWindow;
    loopStartS: number;
    loopLengthS: number;
    filterMode: string;
    seamCount: number;
    liveSources: number;
    nextSeamIn: number | null;
    engineMode: "node" | "worklet";
    migrationStatus: string;
    fallbackReason: string | null;
    driftFrames: number | null;
    workletPcmBytes: number;
    workletWraps: number;
    /** Proxy only — a render-frame gap, not a measured hardware underrun. */
    renderGapFrames: number | null;
    lastWorkletAck: string | null;
  }[];
  fx: (FxRackSnapshot | null)[];
  effectiveBpm: number[];
  bpmSource: "grid" | "manual" | "provisional";
  baseBpm: number;
  soloMask: string;
  linkMask: string;
  lastFxRejection: string | null;
  decodedBytes: number;
  reverseBytes: number;
  budget: MemoryBudget;
  highMemoryMode: boolean;
  memoryStatement: string;
  lastError: string | null;
  lastDecodeMs: number | null;
  enginePreference: EnginePreference;
  workletSupported: boolean;
  workletTrackCount: number;
  migrationWorstCaseBytes: number;
  migrationStatement: string;
  migrationAllowed: boolean;
  lastMigrationPeakBytes: number;
  migrationLog: string[];
  preflight: { name: string; ok: boolean; detail: string }[];
  /** Browsers expose no true underrun counter — this is labelled as a proxy. */
  underrunLabel: string;
}

type Listener = (ack: Ack) => void;

export class AudioEngine {
  ctx: AudioContext | null = null;
  private master: GainNode | null = null;
  private masterAnalyser: AnalyserNode | null = null;
  private tracks: TrackRuntime[] = [];
  private timeline = new TapeTimeline(1);
  private requestedPlaying = false;
  private masterLevel = 0.7;
  private listeners = new Set<Listener>();
  private schedulerTimer: ReturnType<typeof setInterval> | null = null;
  budget: MemoryBudget = SSR_BUDGET;
  /** Explicit opt-in; only meaningful above the standard threshold. */
  highMemoryMode = false;
  lastError: string | null = null;
  lastDecodeMs: number | null = null;
  /** Published for the diagnostics panel. */
  readonly antiClickMatrix = ANTI_CLICK_MATRIX;
  /** Phase 6 — frame-anchored tempo grid, learned by tap. */
  grid: GridState = emptyGrid(48000);
  lastGridTapFrame: number | null = null;
  quantisePunch = false;
  /** Phase 6 — created lazily, only after the context exists. */
  private recorder: RecordingController | null = null;
  private perfRecorder: PerformanceRecorder | null = null;

  /** Phase 6 capture/overdub runtime. Null until the context is unlocked. */
  recording(): RecordingController | null {
    if (!this.ctx) return null;
    if (!this.recorder) {
      const engine = this;
      this.recorder = new RecordingController({
        ctx: this.ctx,
        inputNodeFor: (i) => engine.tracks[i]?.input ?? null,
        faderNodeFor: (i) => engine.tracks[i]?.gain ?? null,
        tapeFrameFor: (i) => {
          const sr = engine.ctx?.sampleRate ?? 48000;
          void i;
          return Math.round(engine.position() * sr);
        },
        loopFramesFor: (i) => {
          const t = engine.tracks[i];
          if (!t?.buffer) return null;
          const b = resolveLoop(t.loop, t.buffer.duration);
          const sr = engine.ctx!.sampleRate;
          return { start: Math.round(b.start * sr), end: Math.round(b.end * sr) };
        },
        trackContent: (i) => (engine.tracks[i]?.buffer ? "loaded" : engine.tracks[i]?.trash ? "trashed" : "empty"),
        currentRate: () => engine.timeline.currentRate(engine.ctx?.currentTime ?? 0),
      });
    }
    return this.recorder;
  }

  /** Master-bus performance recorder — records exactly what is heard. */
  performance(): PerformanceRecorder | null {
    if (!this.ctx) return null;
    if (!this.perfRecorder) {
      this.perfRecorder = new PerformanceRecorder({
        ctx: this.ctx,
        masterTap: () => this.masterAnalyser,
      });
    }
    return this.perfRecorder;
  }


  /** Resolve the real platform budget after hydration (SSR must not guess). */
  resolveBudget(): MemoryBudget {
    this.budget = defaultBudget();
    return this.budget;
  }

  setHighMemoryMode(on: boolean) {
    this.highMemoryMode = on;
  }

  onAck(fn: Listener) {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  private ack(cmd: { id: number; type: AudioCommand["type"] }, status: Ack["status"], detail: string): Ack {
    const a: Ack = { id: cmd.id, type: cmd.type, status, detail, t: performance.now() };
    if (status === "rejected" || status === "failed") this.lastError = `${cmd.type}: ${detail}`;
    for (const l of this.listeners) l(a);
    return a;
  }

  /** Must be called from a user gesture. Idempotent. */
  async unlock(): Promise<{ ok: boolean; detail: string }> {
    try {
      if (!this.ctx) {
        const Ctor =
          window.AudioContext ??
          (window as unknown as { webkitAudioContext?: typeof AudioContext }).webkitAudioContext;
        if (!Ctor) return { ok: false, detail: "this browser has no Web Audio AudioContext" };
        this.ctx = new Ctor();
        this.buildGraph();
        this.startScheduler();
      }
      if (this.ctx.state !== "running") await this.ctx.resume();
      const ok = this.ctx.state === "running";
      return { ok, detail: ok ? `context running @ ${this.ctx.sampleRate} Hz` : `context is ${this.ctx.state}` };
    } catch (err) {
      const detail = err instanceof Error ? err.message : String(err);
      this.lastError = detail;
      return { ok: false, detail };
    }
  }

  /**
   * Per-track graph:
   *
   *   sources → [per-source fade] → input ─┬─ dry ───────────────┬→ gain → analyser → master
   *                                        └─ filter → wet ──────┘
   *
   * The dry path is a TRUE bypass: at filter "off" the biquad contributes
   * nothing at all, so its phase response cannot colour the signal.
   */
  private buildGraph() {
    const ctx = this.ctx!;
    this.master = ctx.createGain();
    this.master.gain.value = this.masterLevel;
    this.masterAnalyser = ctx.createAnalyser();
    this.masterAnalyser.fftSize = 1024;
    this.master.connect(this.masterAnalyser);
    this.masterAnalyser.connect(ctx.destination);

    this.tracks = [0, 1, 2, 3].map((i) => {
      const input = ctx.createGain();
      const dry = ctx.createGain();
      const wet = ctx.createGain();
      const filter = ctx.createBiquadFilter();
      const gain = ctx.createGain();
      const analyser = ctx.createAnalyser();
      analyser.fftSize = 256;

      input.gain.value = 1;
      dry.gain.value = 1;
      wet.gain.value = 0;
      filter.type = "lowpass";
      filter.frequency.value = 20000;

      const preFx = ctx.createGain();
      const soloGain = ctx.createGain();
      soloGain.gain.value = 1;

      input.connect(dry);
      input.connect(filter);
      filter.connect(wet);
      dry.connect(preFx);
      wet.connect(preFx);
      // Phase 5C: preFx → FxRack → fader → solo → analyser → master.
      const fxRack = new FxRack(ctx, gain);
      preFx.connect(fxRack.input);
      gain.connect(soloGain);
      soloGain.connect(analyser);
      analyser.connect(this.master!);
      gain.gain.value = [0.78, 0.72, 0.65, 0.7][i] ?? 0.7;

      return {
        buffer: null,
        reversed: null,
        trash: null,
        input,
        preFx,
        fxRack,
        gain,
        soloGain,
        soloed: false,
        linked: true,
        fxVariation: { filter: 1, echo: 1, reverb: 1, beatRepeat: 1 },
        fxActive: { filter: false, echo: false, reverb: false, beatRepeat: false },
        dry,
        wet,
        filter,
        analyser,
        sources: [],
        generation: 0,
        muted: false,
        level: gain.gain.value,
        loop: { ...DEFAULT_WINDOW },
        seamGeneration: 0,
        committedSeamAt: null,
        filterMode: "off",
        scheduledStartAt: null,
        provenance: null,
        name: null,
        decodeCount: 0,
        decodeMs: null,
        bufferReused: false,
        seamCount: 0,
        sourceDurationS: 0,
        worklet: null,
        engineMode: "node",
        migrationStatus: "node",
        fallbackReason: null,
        nodeReleased: false,
      } satisfies TrackRuntime;
    });
  }

  get ready() {
    return this.ctx != null && this.ctx.state === "running";
  }

  get decodedTotalBytes() {
    return this.tracks.reduce(
      (sum, t) =>
        sum +
        (t.buffer ? bufferBytes(t.buffer) : 0) +
        (t.reversed ? bufferBytes(t.reversed) : 0) +
        // After cutover the ONLY retained PCM for that track lives in the worklet.
        (t.nodeReleased ? (t.worklet?.pcmBytes ?? 0) : 0),
      0,
    );
  }

  get reverseTotalBytes() {
    return this.tracks.reduce((sum, t) => sum + (t.reversed ? bufferBytes(t.reversed) : 0), 0);
  }

  /** Read-only access to a decoded buffer (Memory Saver derives from it). */
  getBuffer(id: TrackId): AudioBuffer | null {
    return this.tracks[id]?.buffer ?? null;
  }

  /** Per-track forward PCM sizes — the worklet migration gate reads this. */
  trackByteList(): number[] {
    return this.tracks.map((t) => (t.buffer ? bufferBytes(t.buffer) : (t.nodeReleased ? (t.worklet?.pcmBytes ?? 0) : 0)));
  }

  trackBytes(id: TrackId): number {
    const b = this.tracks[id]?.buffer;
    return b ? bufferBytes(b) : 0;
  }

  /** Retained total if this track's buffer were replaced by `incoming` bytes. */
  projectedBytes(id: TrackId, incoming: number): number {
    return this.decodedTotalBytes - this.trackBytes(id) + incoming;
  }

  get duration() {
    return this.tracks.reduce((max, t) => Math.max(max, t.buffer?.duration ?? t.sourceDurationS), 0);
  }

  preDecodeGate(id: TrackId, estimateBytes: number): { ok: boolean; detail: string } {
    const projected = this.projectedBytes(id, estimateBytes);
    const verdict = judge(projected, this.budget, this.highMemoryMode);
    return { ok: allowed(verdict), detail: describeVerdict(projected, this.budget, this.highMemoryMode) };
  }

  adoptBuffer(
    id: TrackId,
    buffer: AudioBuffer,
    meta: { name: string; provenance: TrackRuntime["provenance"]; decodeMs?: number | undefined; reused?: boolean | undefined },
  ): { ok: boolean; detail: string; bytes: number } {
    const track = this.tracks[id];
    if (!track) return { ok: false, detail: `no track ${id}`, bytes: 0 };
    const incoming = bufferBytes(buffer);
    const projected = this.projectedBytes(id, incoming);
    const verdict = judge(projected, this.budget, this.highMemoryMode);
    if (!allowed(verdict)) {
      return { ok: false, detail: describeVerdict(projected, this.budget, this.highMemoryMode), bytes: incoming };
    }
    track.buffer = buffer;
    track.sourceDurationS = buffer.duration;
    track.reversed = null;
    track.name = meta.name;
    track.provenance = meta.provenance;
    track.decodeCount += 1;
    track.decodeMs = meta.decodeMs ?? null;
    track.bufferReused = meta.reused ?? true;
    this.lastDecodeMs = meta.decodeMs ?? this.lastDecodeMs;
    return {
      ok: true,
      detail: `adopted ${buffer.duration.toFixed(2)}s · ${buffer.numberOfChannels}ch @ ${buffer.sampleRate} Hz · ${(
        incoming /
        1024 /
        1024
      ).toFixed(1)} MiB${meta.decodeMs != null ? ` · decoded once in ${meta.decodeMs.toFixed(0)} ms` : ""}`,
      bytes: incoming,
    };
  }

  /**
   * FALLBACK ONLY (documented): decodes encoded bytes itself. The normal path
   * is probe → adoptBuffer, which decodes exactly once.
   */
  async loadTrack(
    id: TrackId,
    bytes: ArrayBuffer,
    meta: { name: string; provenance: TrackRuntime["provenance"] },
  ): Promise<{ ok: boolean; detail: string }> {
    const gate = await this.unlock();
    if (!this.ctx) return { ok: false, detail: gate.detail };
    const started = performance.now();
    try {
      const buffer = await this.ctx.decodeAudioData(bytes);
      const decodeMs = performance.now() - started;
      const adopted = this.adoptBuffer(id, buffer, { ...meta, decodeMs, reused: false });
      return { ok: adopted.ok, detail: adopted.detail };
    } catch (err) {
      const detail = err instanceof Error ? err.message : String(err);
      this.lastError = detail;
      return { ok: false, detail: `decode failed — ${detail}` };
    }
  }

  /** Explicit dereference: unloads a track's PCM, reverse copy and trash. */
  releaseTrack(id: TrackId): number {
    const t = this.tracks[id];
    if (!t) return 0;
    const freed =
      (t.buffer ? bufferBytes(t.buffer) : 0) +
      (t.trash ? bufferBytes(t.trash) : 0) +
      (t.reversed ? bufferBytes(t.reversed) : 0);
    t.buffer = null;
    t.trash = null;
    t.reversed = null;
    t.name = null;
    t.provenance = null;
    t.decodeCount = 0;
    t.decodeMs = null;
    t.bufferReused = false;
    return freed;
  }

  resetDecodeCounters() {
    for (const t of this.tracks) {
      t.decodeCount = 0;
      t.decodeMs = null;
      t.bufferReused = false;
    }
  }

  /** Derived playhead. Never incremented by a timer. */
  position(): number {
    if (!this.ctx) return this.timeline.positionAt(0);
    if (!this.requestedPlaying) return this.timeline.positionAt(this.timelineFrozenAt);
    return Math.min(this.duration, this.timeline.positionAt(this.ctx.currentTime));
  }

  private timelineFrozenAt = 0;

  // ---------------------------------------------------------------- sources

  private activeBuffer(t: TrackRuntime): AudioBuffer | null {
    if (!t.loop.reverse) return t.buffer;
    if (!t.reversed && t.buffer) t.reversed = reverseBuffer(this.ctx!, t.buffer);
    return t.reversed;
  }

  private loopBounds(t: TrackRuntime) {
    const buf = t.buffer;
    if (!buf) return null;
    return resolveLoop(t.loop, buf.duration);
  }

  private spawn(t: TrackRuntime, startAt: number, offset: number, fadeIn: boolean): LiveSource | null {
    const ctx = this.ctx!;
    const buf = this.activeBuffer(t);
    if (!buf) return null;
    const fade = ctx.createGain();
    fade.connect(t.input);
    const node = ctx.createBufferSource();
    node.buffer = buf;
    node.playbackRate.value = this.timeline.currentRate(ctx.currentTime);
    node.connect(fade);
    const gen = ++t.generation;
    const live: LiveSource = { node, fade, gen, startAt, startPos: offset, stopAt: null };
    node.onended = () => {
      t.sources = t.sources.filter((s) => s !== live);
      try {
        fade.disconnect();
      } catch {
        /* noop */
      }
    };
    if (fadeIn) {
      fade.gain.setValueAtTime(0, startAt);
      fade.gain.setValueCurveAtTime(sampleCurve(equalPower, "b"), startAt, SEAM_FADE_S);
    } else {
      fade.gain.setValueAtTime(1, startAt);
    }
    node.start(startAt, Math.min(Math.max(0, offset), Math.max(0, buf.duration - 1e-4)));
    t.sources.push(live);
    t.scheduledStartAt = startAt;
    return live;
  }

  private fadeOutAndStop(t: TrackRuntime, live: LiveSource, at: number) {
    live.fade.gain.cancelScheduledValues(at);
    live.fade.gain.setValueAtTime(1, at);
    live.fade.gain.setValueCurveAtTime(sampleCurve(equalPower, "a"), at, SEAM_FADE_S);
    try {
      live.node.stop(at + SEAM_FADE_S);
      live.stopAt = at + SEAM_FADE_S;
    } catch {
      /* already stopped */
    }
    void t;
  }

  private stopSources() {
    for (const t of this.tracks) {
      for (const s of t.sources) {
        try {
          s.node.onended = null;
          s.node.stop();
        } catch {
          /* already stopped */
        }
        s.node.disconnect();
        try {
          s.fade.disconnect();
        } catch {
          /* noop */
        }
      }
      t.sources = [];
      t.scheduledStartAt = null;
      t.committedSeamAt = null;
    }
  }

  /** All four stems start at ONE shared, scheduled context time. */
  private startAll(offset: number): { started: number; startAt: number } {
    const ctx = this.ctx!;
    const startAt = ctx.currentTime + LOOKAHEAD_S;
    let started = 0;
    for (const t of this.tracks) {
      if (!t.buffer) continue;
      const bounds = t.loop.enabled ? this.loopBounds(t) : null;
      const from = bounds ? Math.max(offset, bounds.start) : offset;
      if (this.spawn(t, startAt, from, false)) started++;
    }
    this.timeline.anchor(startAt, offset);
    return { started, startAt };
  }

  /** Largest difference between the scheduled start times of live sources. */
  startSpreadMs(): number {
    const times = this.tracks.map((t) => t.scheduledStartAt).filter((v): v is number => v != null);
    if (times.length < 2) return 0;
    return (Math.max(...times) - Math.min(...times)) * 1000;
  }

  // -------------------------------------------------------------- scheduler

  private startScheduler() {
    if (this.schedulerTimer) return;
    this.schedulerTimer = setInterval(() => this.tick(), SCHEDULER_INTERVAL_MS);
  }

  /**
   * Commits loop seams inside the lookahead window. Every seam time is derived
   * from the SAME integrated-rate timeline as the playhead, so a varispeed or
   * glide change (which bumps seamGeneration and clears committedSeamAt) makes
   * every pending seam be re-derived rather than fire at a stale time.
   */
  private tick() {
    const ctx = this.ctx;
    if (!ctx || !this.requestedPlaying) return;
    const now = ctx.currentTime;
    for (const t of this.tracks) {
      if (!t.buffer || !t.loop.enabled) continue;
      if (t.committedSeamAt != null && t.committedSeamAt > now) continue;
      const bounds = this.loopBounds(t);
      if (!bounds || bounds.length <= SEAM_FADE_S * 2) continue;
      const seamAt = this.timeline.timeAtPosition(now, bounds.end);
      if (seamAt == null) continue;
      const fadeStart = seamAt - SEAM_FADE_S;
      if (fadeStart > now + SEAM_LOOKAHEAD_S) continue;
      const at = Math.max(fadeStart, now + 0.005);
      const outgoing = t.sources[t.sources.length - 1];
      if (outgoing) this.fadeOutAndStop(t, outgoing, at);
      this.spawn(t, at, bounds.start, true);
      t.committedSeamAt = seamAt;
      t.seamCount++;
      // The wrap moves the playhead; re-anchor at the seam instant so the
      // derived position keeps agreeing with what is audible.
      this.timeline.anchor(at, bounds.start);
    }
  }

  /** Any rate or loop-shape change invalidates every pending seam. */
  private invalidateSeams() {
    for (const t of this.tracks) {
      t.seamGeneration++;
      t.committedSeamAt = null;
    }
  }

  /**
   * Dual-source crossfade to a new read position — used by loop-mode
   * transitions, chop jumps and reverse flips (anti-click matrix).
   */
  private relocate(t: TrackRuntime, toPosition: number): boolean {
    const ctx = this.ctx;
    if (!ctx || !this.requestedPlaying) return false;
    const at = ctx.currentTime + 0.01;
    const outgoing = t.sources[t.sources.length - 1];
    if (outgoing) this.fadeOutAndStop(t, outgoing, at);
    const ok = this.spawn(t, at, toPosition, true) != null;
    if (ok) {
      t.committedSeamAt = null;
      this.timeline.anchor(at, toPosition);
    }
    return ok;
  }

  // ------------------------------------------------------------------ gains

  private setGain(param: AudioParam, value: number) {
    const ctx = this.ctx!;
    param.cancelScheduledValues(ctx.currentTime);
    param.setTargetAtTime(value, ctx.currentTime, RAMP_TAU);
  }

  applyTrackGain(id: TrackId, level: number) {
    const t = this.tracks[id];
    if (!t) return;
    t.level = level;
    if (!this.ctx) return;
    // Post-FX fader: it never gates, so it can be swept while tails ring out.
    this.setGain(t.gain.gain, level);
  }

  applyMasterGain(level: number) {
    this.masterLevel = level;
    if (!this.ctx || !this.master) return;
    this.setGain(this.master.gain, level);
  }

  /**
   * Dry ↔ filtered: the two paths carry the SAME signal, so their amplitudes
   * add. Complementary gains hold the sum at unity; equal-power would put a
   * +3 dB bump in the middle of the transition.
   */
  private applyFilter(t: TrackRuntime, mode: "off" | "lp" | "hp", cutoff: number, correlation = 1) {
    const ctx = this.ctx!;
    const now = ctx.currentTime;
    if (mode !== "off") {
      t.filter.type = mode === "lp" ? "lowpass" : "highpass";
      t.filter.frequency.setTargetAtTime(cutoff, now, 0.02);
    }
    const target = mode === "off" ? 0 : 1;
    const curve = (x: number) => measuredDryWet(target === 1 ? x : 1 - x, correlation);
    if (t.fxRack) {
      // Correction 4: the rack is the SINGLE automation owner for cutoff/type/Q.
      // The legacy 5A biquad stays in the graph but is held at true dry.
      t.dry.gain.cancelScheduledValues(now);
      t.wet.gain.cancelScheduledValues(now);
      t.dry.gain.setValueAtTime(1, now);
      t.wet.gain.setValueAtTime(0, now);
      t.fxRack.setTapeFilter(mode, cutoff, correlation);
    } else {
      t.dry.gain.cancelScheduledValues(now);
      t.wet.gain.cancelScheduledValues(now);
      t.dry.gain.setValueCurveAtTime(sampleCurve(curve, "a"), now, FILTER_FADE_S);
      t.wet.gain.setValueCurveAtTime(sampleCurve(curve, "b"), now, FILTER_FADE_S);
    }
    t.filterMode = mode;
  }

  // ------------------------------------------------------- Phase 5B worklet

  /** Session-local selector. Default stays "node"; never flipped implicitly. */
  enginePreference: EnginePreference = "node";
  lastPreflight: PreflightResult | null = null;
  migrationLog: string[] = [];
  lastMigrationPeakBytes = 0;
  /** Re-decode hook supplied by the ingest layer for failure recovery. */
  redecode: ((id: TrackId) => Promise<AudioBuffer | null>) | null = null;

  private note(line: string) {
    this.migrationLog = [`${new Date().toISOString().slice(11, 23)}  ${line}`, ...this.migrationLog].slice(0, 60);
  }

  get workletTracks(): TrackId[] {
    return this.tracks.flatMap((t, i) => (t.engineMode === "worklet" ? [i as TrackId] : []));
  }

  /** Conservative gate: the whole project's PCM duplicated, never per-track. */
  migrationGate() {
    return estimateMigration(this.trackByteList(), this.budget, this.highMemoryMode);
  }

  async preflight(): Promise<PreflightResult> {
    const base = await preflightWorklet(this.ctx);
    const gate = this.migrationGate();
    base.checks.push({ name: "memory-gate", ok: gate.allowed, detail: gate.statement });
    const result = { ok: base.ok && gate.allowed, checks: base.checks };
    this.lastPreflight = result;
    return result;
  }

  /**
   * Migrate ONE track. Order: preflight → create → adopt (ownership transfer)
   * → readiness handshake → derive source position from the shared transport
   * timeline → schedule at a future shared context frame → crossfade →
   * measure drift → release the 5A representation only on success.
   */
  async migrateTrack(id: TrackId, applyFrame?: number): Promise<{ ok: boolean; detail: string }> {
    const t = this.tracks[id];
    if (!t) return { ok: false, detail: `no track ${id}` };
    if (!this.ctx) return { ok: false, detail: "audio not unlocked" };
    if (t.engineMode === "worklet") return { ok: true, detail: `track ${id + 1} already on the worklet engine` };
    if (!t.buffer) return { ok: false, detail: `track ${id + 1} has no decoded audio` };

    t.migrationStatus = "checking";
    const pre = await this.preflight();
    if (!pre.ok) {
      t.migrationStatus = "refused";
      t.fallbackReason = pre.checks.filter((c) => !c.ok).map((c) => c.detail).join(" · ");
      this.note(`T${id + 1} migration refused — ${t.fallbackReason}`);
      return { ok: false, detail: t.fallbackReason };
    }

    const wt = new WorkletTrack(id, this.ctx, t.input, (w, detail) => this.handleProcessorError(w, detail));
    const created = wt.create();
    if (!created.ok) {
      t.migrationStatus = "failed";
      t.fallbackReason = created.detail;
      this.note(`T${id + 1} node creation failed — ${created.detail}`);
      return { ok: false, detail: created.detail };
    }
    t.worklet = wt;

    const adopted = await wt.adopt(t.buffer);
    this.lastMigrationPeakBytes = Math.max(this.lastMigrationPeakBytes, this.migrationGate().worstCasePeakBytes);
    if (adopted.status !== "ready") {
      t.migrationStatus = "failed";
      t.fallbackReason = adopted.detail;
      await wt.dispose();
      t.worklet = null;
      this.note(`T${id + 1} adopt failed — ${adopted.detail}`);
      return { ok: false, detail: adopted.detail };
    }
    this.note(`T${id + 1} adopt ready — ${adopted.detail}`);

    // Structural state must match the node engine before the switch.
    const at = applyFrame ?? sharedApplyFrame(this.ctx);
    await this.pushStructure(t, at);

    const sr = t.buffer.sampleRate;
    const posAtSwitch = this.requestedPlaying
      ? this.timeline.positionAt(at / this.ctx.sampleRate)
      : this.position();
    const sourceFrame = Math.round(posAtSwitch * sr);

    const outgoing = t.sources[t.sources.length - 1];
    const result = await wt.handoff(
      sourceFrame,
      (fadeAt) => {
        if (outgoing) this.fadeOutAndStop(t, outgoing, fadeAt);
      },
      at,
    );
    if (!result.ok) {
      t.migrationStatus = "failed";
      t.fallbackReason = result.detail;
      await wt.dispose();
      t.worklet = null;
      this.note(`T${id + 1} handoff refused — ${result.detail}`);
      return { ok: false, detail: result.detail };
    }

    if (!this.requestedPlaying) await wt.post({ type: "stop", applyAtContextFrame: at });

    // Only now is the Phase 5A representation released.
    t.buffer = null;
    t.reversed = null;
    t.nodeReleased = true;
    t.engineMode = "worklet";
    t.migrationStatus = "worklet";
    t.fallbackReason = null;
    this.note(`T${id + 1} on worklet — ${result.detail}`);
    return { ok: true, detail: result.detail };
  }

  /** Sequential four-track migration at ONE shared context frame. */
  async migrateAll(): Promise<{ ok: boolean; detail: string }> {
    if (!this.ctx) return { ok: false, detail: "audio not unlocked" };
    const gate = this.migrationGate();
    if (!gate.allowed) {
      this.note(`four-track migration refused — ${gate.statement}`);
      return { ok: false, detail: gate.statement };
    }
    const ids = this.tracks.flatMap((t, i) => (t.buffer && t.engineMode === "node" ? [i as TrackId] : []));
    if (ids.length === 0) return { ok: false, detail: "nothing to migrate" };
    // Enough lookahead for every sequential adopt to complete before the switch.
    const at = sharedApplyFrame(this.ctx, 0.12 + 0.25 * ids.length);
    const results: string[] = [];
    let ok = true;
    for (const id of ids) {
      const r = await this.migrateTrack(id, at);
      if (!r.ok) ok = false;
      results.push(`T${id + 1}: ${r.ok ? "ok" : "FAILED"} — ${r.detail}`);
      await new Promise((res) => setTimeout(res, 0)); // yield between tracks
    }
    return { ok, detail: `shared context frame ${at}\n${results.join("\n")}\n${gate.statement}` };
  }

  /** Push current window/chop/direction/rate state into a processor. */
  private async pushStructure(t: TrackRuntime, at: number) {
    const wt = t.worklet;
    if (!wt) return;
    const frames = Math.round(t.sourceDurationS * (t.buffer?.sampleRate ?? this.ctx!.sampleRate));
    const a = Math.min(t.loop.start, t.loop.end);
    const b = Math.max(t.loop.start, t.loop.end);
    await wt.post({
      type: "setWindow",
      start: Math.round(a * frames),
      end: Math.round(b * frames),
      enabled: t.loop.enabled,
      applyAtContextFrame: at,
    });
    await wt.post({ type: "setChop", division: t.loop.chopDiv, index: t.loop.chopIndex, applyAtContextFrame: at });
    await wt.post({ type: "setDirection", direction: t.loop.reverse ? -1 : 1, applyAtContextFrame: at });
    await wt.post({
      type: "setRate",
      rate: this.timeline.targetRate(),
      rampFrames: 0,
      applyAtContextFrame: at,
    });
  }

  /** Revert every worklet track to the Phase 5A node engine. */
  async revertToNode(): Promise<{ ok: boolean; detail: string }> {
    const ids = this.workletTracks;
    if (ids.length === 0) return { ok: true, detail: "already on the node engine" };
    const restored: string[] = [];
    for (const id of ids) {
      const t = this.tracks[id]!;
      const wt = t.worklet;
      const buf = this.redecode ? await this.redecode(id) : null;
      await wt?.dispose();
      t.worklet = null;
      t.engineMode = "node";
      t.migrationStatus = "node";
      t.nodeReleased = false;
      if (buf) {
        t.buffer = buf;
        t.sourceDurationS = buf.duration;
        restored.push(`T${id + 1} re-decoded from its stored blob`);
      } else {
        t.fallbackReason = "no stored blob available to re-decode — track is silent until reloaded";
        restored.push(`T${id + 1} could NOT be re-decoded: ${t.fallbackReason}`);
      }
    }
    if (this.requestedPlaying && this.ctx) {
      const pos = this.position();
      this.stopSources();
      this.startAll(pos);
    }
    this.enginePreference = "node";
    this.note(`reverted to node engine — ${restored.join("; ")}`);
    return { ok: true, detail: restored.join("; ") };
  }

  /**
   * processorerror recovery. The failed node is never reused: it is recreated
   * once, and if that fails the track falls back to Phase 5A (or stays silent
   * with a reported reason). The other tracks are never touched.
   */
  private handleProcessorError(wt: WorkletTrack, detail: string) {
    const t = this.tracks[wt.trackId];
    if (!t) return;
    t.migrationStatus = "recovering";
    t.fallbackReason = detail;
    this.note(`T${wt.trackId + 1} processorerror — ${detail}; recovering, other tracks untouched`);
    void this.recoverTrack(wt.trackId as TrackId);
  }

  async recoverTrack(id: TrackId): Promise<{ ok: boolean; detail: string }> {
    const t = this.tracks[id];
    if (!t || !this.ctx) return { ok: false, detail: "no track/context" };
    const old = t.worklet;
    await old?.dispose();
    t.worklet = null;
    const buf = this.redecode ? await this.redecode(id) : null;
    if (!buf) {
      t.engineMode = "node";
      t.migrationStatus = "failed";
      t.nodeReleased = false;
      t.fallbackReason = `${t.fallbackReason ?? "processor failed"} · no stored blob to re-decode — track ${id + 1} stays silent, project intact`;
      return { ok: false, detail: t.fallbackReason };
    }
    t.buffer = buf;
    t.sourceDurationS = buf.duration;
    t.nodeReleased = false;
    t.engineMode = "node";

    if ((old?.recreateAttempts ?? 0) < 1 && this.enginePreference === "worklet") {
      const r = await this.migrateTrack(id);
      if (r.ok) {
        this.note(`T${id + 1} recreated on the worklet engine and rejoined at a shared frame`);
        return r;
      }
    }
    // Rejoin on 5A at a future shared frame with a crossfade.
    t.migrationStatus = "node";
    if (this.requestedPlaying) {
      const at = this.ctx.currentTime + LOOKAHEAD_S;
      const pos = this.timeline.positionAt(at);
      this.spawn(t, at, pos, true);
    }
    this.note(`T${id + 1} re-decoded and rejoined the node engine — other tracks stayed audible`);
    return { ok: true, detail: `track ${id + 1} recovered on the node engine` };
  }

  /** Test hook: force one processor to throw so recovery can be proven. */
  async forceProcessorFailure(id: TrackId): Promise<string> {
    const t = this.tracks[id];
    if (!t?.worklet?.node) return `track ${id + 1} is not on the worklet engine`;
    const ack = await t.worklet.forceError(0);
    return `track ${id + 1}: ${ack.detail}`;
  }

  /** Ask every worklet processor where it is; report pairwise drift. */
  async measureDrift(): Promise<{ frames: (number | null)[]; pairs: string[]; maxDrift: number; acks: WorkletAck[] }> {
    const acks: WorkletAck[] = [];
    const frames: (number | null)[] = [];
    for (const t of this.tracks) {
      if (t.worklet?.node) {
        const a = await t.worklet.poll();
        acks.push(a);
        frames.push(a.resultingSourceFrame ?? null);
      } else frames.push(null);
    }
    const { pairs, maxDrift } = pairwiseDrift(frames);
    return { frames, pairs, maxDrift, acks };
  }

  /** Fan a structural/transport change out to every worklet track at ONE frame. */
  private fanout(build: (t: TrackRuntime, at: number) => Parameters<WorkletTrack["post"]>[0] | null) {
    if (!this.ctx) return;
    const at = sharedApplyFrame(this.ctx);
    for (const t of this.tracks) {
      if (t.engineMode !== "worklet" || !t.worklet) continue;
      const msg = build(t, at);
      if (msg) void t.worklet.post(msg);
    }
  }

  // ------------------------------------------------- Phase 5C stem + FX

  /** BPM source hierarchy: grid → manual → provisional 120. */
  baseBpm = 120;
  bpmSource: "grid" | "manual" | "provisional" = "provisional";
  /** Diagnostics: last rejected FX activation. */
  lastFxRejection: string | null = null;

  setBaseBpm(bpm: number, source: "grid" | "manual") {
    if (Number.isFinite(bpm) && bpm > 0) {
      this.baseBpm = bpm;
      this.bpmSource = source;
    }
  }

  /** Per-stem effective BPM (correction 5): baseBpm × |rate of that stem|. */
  effectiveBpm(id: TrackId): number {
    const now = this.ctx?.currentTime ?? 0;
    const rate = Math.abs(this.timeline.currentRate(now)) || 1;
    return this.baseBpm * rate;
  }

  private anySolo(): boolean {
    return this.tracks.some((t) => t.soloed);
  }

  /**
   * Correction 3. Solo never mutates saved mute state:
   *   audibleBySolo = anySolo ? track.soloed : true
   *   inputOpen     = audibleBySolo && (!track.muted || track.soloed)
   */
  applyAudibility(id: TrackId) {
    const t = this.tracks[id];
    if (!t || !this.ctx) return;
    const audibleBySolo = this.anySolo() ? t.soloed : true;
    const open = audibleBySolo && (!t.muted || t.soloed);
    t.fxRack?.setInputOpen(open);
    this.setGain(t.soloGain.gain, audibleBySolo ? 1 : 0);
  }

  private applyAudibilityAll() {
    for (let i = 0; i < this.tracks.length; i++) this.applyAudibility(i as TrackId);
  }

  /** Re-apply one FX family from the current slot flags. */
  private async applyFx(id: TrackId, family: FxFamily, latched: boolean): Promise<{ ok: boolean; detail: string }> {
    const t = this.tracks[id];
    if (!t?.fxRack) return { ok: false, detail: "audio not unlocked" };
    const active = t.fxActive[family];
    const vi = Math.max(0, Math.min(3, t.fxVariation[family] - 1));
    switch (family) {
      case "filter":
        t.fxRack.setFxFilter(active ? vi : null, latched);
        if (active && latched) t.fxRack.commitFilterLatch();
        return { ok: true, detail: `filter ${active ? `variation ${vi + 1}` : "released to the underlying tape filter"}` };
      case "echo":
        t.fxRack.setEcho(active, vi, this.effectiveBpm(id));
        return {
          ok: true,
          detail: `echo ${active ? "on" : "released (tail decaying)"} · ${t.fxRack.echoDelayFor(vi, this.effectiveBpm(id)).toFixed(4)}s @ ${this.effectiveBpm(id).toFixed(2)} BPM`,
        };
      case "reverb":
        t.fxRack.setReverb(active, vi);
        return { ok: true, detail: `reverb ${active ? "on" : "released (tail decaying)"} · variation ${vi + 1}` };
      case "beatRepeat": {
        const res = await t.fxRack.setBeatRepeat(active, vi, this.effectiveBpm(id));
        if (!res.ok) this.lastFxRejection = res.detail;
        return res;
      }
    }
  }

  /** Momentary / latched activation for one FX family on one stem. */
  setFxActive(id: TrackId, family: FxFamily, active: boolean, latched = false): Promise<{ ok: boolean; detail: string }> {
    const t = this.tracks[id];
    if (!t) return Promise.resolve({ ok: false, detail: `no track ${id}` });
    t.fxActive[family] = active;
    return this.applyFx(id, family, latched);
  }

  setFxVariation(id: TrackId, family: FxFamily, variation: number, latched = false): Promise<{ ok: boolean; detail: string }> {
    const t = this.tracks[id];
    if (!t) return Promise.resolve({ ok: false, detail: `no track ${id}` });
    t.fxVariation[family] = Math.max(1, Math.min(4, variation));
    return this.applyFx(id, family, latched);
  }

  setSolo(id: TrackId, soloed: boolean) {
    const t = this.tracks[id];
    if (!t) return;
    t.soloed = soloed;
    this.applyAudibilityAll();
  }

  /** Unlink/relink is phase-continuous: no source is stopped or restarted. */
  setLinked(id: TrackId, linked: boolean) {
    const t = this.tracks[id];
    if (t) t.linked = linked;
  }

  /** Song switch / stem replacement: clear momentary DSP history everywhere. */
  flushAllFx() {
    for (const t of this.tracks) {
      for (const f of FX_FAMILIES) t.fxActive[f] = false;
      t.fxRack?.flushTails();
    }
  }

  fxSnapshots(): (FxRackSnapshot | null)[] {
    return this.tracks.map((t) => t.fxRack?.snapshot() ?? null);
  }

  // ------------------------------------------------- Phase 6 heads / PRINT

  heads: HeadsState = emptyHeads();
  /** Node-engine head voices. Empty while heads are served by the worklet. */
  private headVoices: ({ node: AudioBufferSourceNode; gain: GainNode } | null)[] = [null, null, null, null];
  private headsBus: GainNode | null = null;
  /** Mute mask of the non-source tracks, restored verbatim on heads exit. */
  private preHeadsMutes: boolean[] | null = null;
  /** PRINT commit hook, installed by the ingest layer (persist + adopt). */
  commitPrint:
    | ((target: TrackId, buffer: AudioBuffer, detail: string) => Promise<{ ok: boolean; detail: string }>)
    | null = null;

  private headCandidates(): SourceCandidate[] {
    return this.tracks.map((t, i) => ({
      index: i,
      loaded: t.buffer != null || (t.engineMode === "worklet" && (t.worklet?.pcmBytes ?? 0) > 0),
      playing: t.sources.length > 0 || (t.engineMode === "worklet" && this.requestedPlaying),
      muted: t.muted,
    }));
  }

  /** Cycle geometry of one track in SOURCE frames (window/chop resolved). */
  private cycleOf(id: TrackId): { startFrame: number; frames: number; startS: number; lengthS: number; sr: number } | null {
    const t = this.tracks[id];
    if (!t) return null;
    const duration = t.buffer?.duration ?? t.sourceDurationS;
    if (!(duration > 0)) return null;
    const sr = t.buffer?.sampleRate ?? this.ctx?.sampleRate ?? 48000;
    const b = resolveLoop(t.loop, duration);
    return { startFrame: Math.round(b.start * sr), frames: Math.round(b.length * sr), startS: b.start, lengthS: b.length, sr };
  }

  private headBufferAndOffset(t: TrackRuntime, h: HeadState, cycleStartS: number, cycleLenS: number) {
    const buf = t.buffer!;
    const posS = cycleStartS + h.offset * cycleLenS;
    if (!h.reverse) return { buffer: buf, offset: posS, loopStart: cycleStartS, loopEnd: cycleStartS + cycleLenS };
    if (!t.reversed) t.reversed = reverseBuffer(this.ctx!, buf);
    const d = buf.duration;
    return {
      buffer: t.reversed,
      offset: d - posS,
      loopStart: d - (cycleStartS + cycleLenS),
      loopEnd: d - cycleStartS,
    };
  }

  private spawnHeadVoice(t: TrackRuntime, i: number, at: number, fadeIn: boolean) {
    const ctx = this.ctx!;
    const cyc = this.cycleOf(this.heads.source as TrackId);
    if (!cyc || !t.buffer || !this.headsBus) return;
    const h = this.heads.heads[i]!;
    const { buffer, offset, loopStart, loopEnd } = this.headBufferAndOffset(t, h, cyc.startS, cyc.lengthS);
    const gain = ctx.createGain();
    gain.connect(this.headsBus);
    const node = ctx.createBufferSource();
    node.buffer = buffer;
    node.loop = true;
    node.loopStart = Math.max(0, loopStart);
    node.loopEnd = Math.min(buffer.duration, Math.max(loopStart + 1e-3, loopEnd));
    node.playbackRate.value = Math.abs(this.timeline.currentRate(ctx.currentTime)) || 1;
    node.connect(gain);
    const level = h.muted ? 0 : h.level;
    if (fadeIn) {
      gain.gain.setValueAtTime(0, at);
      gain.gain.setValueCurveAtTime(sampleCurve(equalPower, "b").map((v) => v * level) as unknown as Float32Array, at, SEAM_FADE_S);
      gain.gain.setValueAtTime(level, at + SEAM_FADE_S);
    } else {
      gain.gain.setValueAtTime(level, at);
    }
    node.start(at, Math.min(Math.max(0, offset), Math.max(0, buffer.duration - 1e-4)));
    this.headVoices[i] = { node, gain };
  }

  private killHeadVoice(i: number, at: number, fadeOut: boolean) {
    const v = this.headVoices[i];
    if (!v) return;
    this.headVoices[i] = null;
    try {
      if (fadeOut) {
        v.gain.gain.cancelScheduledValues(at);
        v.gain.gain.setValueCurveAtTime(sampleCurve(equalPower, "a").map((x) => x * v.gain.gain.value) as unknown as Float32Array, at, SEAM_FADE_S);
        v.node.stop(at + SEAM_FADE_S);
      } else v.node.stop(at);
    } catch {
      /* already stopped */
    }
    setTimeout(() => {
      try {
        v.node.disconnect();
        v.gain.disconnect();
      } catch {
        /* noop */
      }
    }, 200);
  }

  private teardownHeadVoices() {
    const at = this.ctx?.currentTime ?? 0;
    for (let i = 0; i < 4; i++) this.killHeadVoice(i, at, false);
    if (this.headsBus) {
      try {
        this.headsBus.disconnect();
      } catch {
        /* noop */
      }
      this.headsBus = null;
    }
  }

  /** Push the current head table to whichever engine is serving heads. */
  private pushHeads() {
    const src = this.heads.source;
    if (src == null) return;
    const t = this.tracks[src];
    if (!t) return;
    if (this.heads.engine === "worklet" && t.worklet) {
      void t.worklet.setHeads(
        this.heads.heads.map((h) => ({ offset: h.offset, level: h.muted ? 0 : h.level, muted: h.muted, reverse: h.reverse })),
        this.heads.cycleFrames > 0 ? this.heads.cycleStartFrame : 0,
        Math.max(1, this.heads.cycleFrames),
      );
      return;
    }
    // Node engine: levels/mutes ride the per-head gain, geometry changes respawn.
    const now = this.ctx?.currentTime ?? 0;
    for (let i = 0; i < 4; i++) {
      const v = this.headVoices[i];
      const h = this.heads.heads[i]!;
      if (v) this.setGain(v.gain.gain, h.muted ? 0 : h.level);
      void now;
    }
  }

  /** Respawn one node head (reverse flip or absolute scrub) through a seam. */
  private restartHeadVoice(i: number) {
    if (this.heads.engine !== "node" || this.heads.source == null || !this.ctx) return;
    const t = this.tracks[this.heads.source]!;
    const at = this.ctx.currentTime + 0.01;
    this.killHeadVoice(i, at, true);
    this.spawnHeadVoice(t, i, at, true);
  }

  enterHeadsMode(): { ok: boolean; detail: string } {
    if (!this.ctx) return { ok: false, detail: "audio not unlocked" };
    if (this.heads.active) return { ok: true, detail: "heads already active" };
    const source = chooseSource(this.headCandidates());
    const cyc = source != null ? this.cycleOf(source as TrackId) : null;
    const t = source != null ? this.tracks[source] : null;
    const engine: "worklet" | "node" = t?.engineMode === "worklet" && t.worklet ? "worklet" : "node";
    const res = enterHeads(this.heads, {
      source,
      cycleFrames: cyc?.frames ?? 0,
      cycleStartFrame: cyc?.startFrame ?? 0,
      engine,
      fallback: engine === "node" ? "served by the node engine — four AudioBufferSourceNode voices over one shared PCM" : null,
      frame: Math.round(this.ctx.currentTime * this.ctx.sampleRate),
    });
    if (!res.ok) return { ok: false, detail: res.detail };
    this.heads = res.state;

    // Everything except the source is silenced for the duration; the saved mute
    // states are restored verbatim on exit.
    this.preHeadsMutes = this.tracks.map((tr) => tr.muted);
    this.tracks.forEach((tr, i) => {
      if (i !== source) tr.muted = true;
      this.applyAudibility(i as TrackId);
    });

    if (engine === "worklet") {
      this.pushHeads();
    } else {
      const at = this.ctx.currentTime + LOOKAHEAD_S;
      const bus = this.ctx.createGain();
      bus.gain.value = 1;
      bus.connect(t!.input);
      this.headsBus = bus;
      for (const s of t!.sources) {
        try {
          s.node.onended = null;
          s.node.stop(at);
        } catch {
          /* noop */
        }
      }
      t!.sources = [];
      t!.committedSeamAt = null;
      for (let i = 0; i < 4; i++) this.spawnHeadVoice(t!, i, at, false);
    }
    return { ok: true, detail: `${res.detail}${this.heads.fallback ? ` · ${this.heads.fallback}` : ""}` };
  }

  exitHeadsMode(): { ok: boolean; detail: string } {
    if (!this.heads.active) return { ok: true, detail: "heads already off" };
    const src = this.heads.source;
    const engine = this.heads.engine;
    const t = src != null ? this.tracks[src] : null;
    if (engine === "worklet" && t?.worklet) {
      // The underlying pointer never moved, so exit is phase-correct by design.
      void t.worklet.setHeads(null, 0, 1);
    } else {
      this.teardownHeadVoices();
      if (t && this.ctx && this.requestedPlaying) {
        const pos = this.position();
        const bounds = t.loop.enabled ? this.loopBounds(t) : null;
        this.spawn(t, this.ctx.currentTime + 0.01, bounds ? Math.max(pos, bounds.start) : pos, true);
        t.committedSeamAt = null;
      }
    }
    if (this.preHeadsMutes) {
      const mask = this.preHeadsMutes;
      this.tracks.forEach((tr, i) => {
        tr.muted = mask[i] ?? false;
        this.applyAudibility(i as TrackId);
      });
      this.preHeadsMutes = null;
    }
    this.heads = exitHeads(this.heads);
    return { ok: true, detail: "heads off — original four tracks, faders and mutes restored, transport untouched" };
  }

  setHeadsSource(id: TrackId): { ok: boolean; detail: string } {
    if (!this.heads.active) return { ok: false, detail: "heads mode is not active" };
    const t = this.tracks[id];
    if (!t) return { ok: false, detail: `no track ${id}` };
    const cyc = this.cycleOf(id);
    if (!cyc || cyc.frames <= 1) return { ok: false, detail: `track ${id + 1} has no audible cycle to read` };
    // Relink = leave heads mode geometry, re-enter against the new source.
    const keep = this.heads.heads;
    this.exitHeadsMode();
    // Force the chosen track to be the candidate winner.
    const saved = this.tracks.map((tr) => tr.muted);
    this.tracks.forEach((tr, i) => {
      tr.muted = i === id ? false : true;
    });
    const res = this.enterHeadsMode();
    this.preHeadsMutes = saved;
    if (!res.ok) {
      this.tracks.forEach((tr, i) => {
        tr.muted = saved[i] ?? false;
        this.applyAudibility(i as TrackId);
      });
      return res;
    }
    this.heads = relinkSource({ ...this.heads, heads: keep }, id, cyc.frames, cyc.startFrame);
    this.pushHeads();
    if (this.heads.engine === "node") for (let i = 0; i < 4; i++) this.restartHeadVoice(i);
    return { ok: true, detail: `heads source → track ${id + 1} · cycle ${cyc.frames} frames · ${headsSummary(this.heads)}` };
  }

  /** Source PCM for one whole audible cycle, from whichever engine owns it. */
  private async readCyclePcm(): Promise<{ ok: boolean; channels: Float32Array[]; detail: string }> {
    const src = this.heads.source;
    if (src == null) return { ok: false, channels: [], detail: "no heads source" };
    const t = this.tracks[src]!;
    const start = this.heads.cycleStartFrame;
    const frames = this.heads.cycleFrames;
    if (t.buffer) {
      const chans: Float32Array[] = [];
      for (let c = 0; c < t.buffer.numberOfChannels; c++) {
        const arr = new Float32Array(frames);
        const src32 = t.buffer.getChannelData(c);
        for (let i = 0; i < frames; i++) arr[i] = start + i < src32.length ? src32[start + i]! : 0;
        chans.push(arr);
      }
      return { ok: true, channels: chans, detail: `read ${frames} frames from the node buffer` };
    }
    if (t.worklet) return t.worklet.readRange(start, frames);
    return { ok: false, channels: [], detail: `track ${src + 1} holds no readable PCM` };
  }

  /**
   * PRINT (§4): bake exactly one audible cycle of the four-head performance
   * into an EMPTY track. Positions, directions, levels and mutes are baked;
   * rate, FX and master are not, so the printed loop is never resampled twice.
   */
  async printHeads(target: TrackId): Promise<{ ok: boolean; detail: string }> {
    if (!this.ctx) return { ok: false, detail: "audio not unlocked" };
    if (!this.heads.active || this.heads.source == null) return { ok: false, detail: "PRINT requires heads mode to be active" };
    if (target === this.heads.source) return { ok: false, detail: "PRINT target cannot be the heads source" };
    const tt = this.tracks[target];
    if (!tt) return { ok: false, detail: `no track ${target}` };
    if (tt.buffer || tt.engineMode === "worklet")
      return { ok: false, detail: `track ${target + 1} is not empty — PRINT only ever writes into an empty track` };

    const frames = this.heads.cycleFrames;
    this.heads = { ...this.heads, print: { target, phase: "rendering", detail: "reading one cycle", cycleFrames: frames } };
    const read = await this.readCyclePcm();
    if (!read.ok || read.channels.length === 0) {
      this.heads = { ...this.heads, print: { target, phase: "failed", detail: read.detail, cycleFrames: frames } };
      return { ok: false, detail: `PRINT failed — ${read.detail}. Target left empty, heads still audible.` };
    }
    const rendered = renderHeadsCycle(read.channels, 0, frames, this.heads.heads);
    const peak = peakOf(rendered);
    const sr = this.tracks[this.heads.source]!.buffer?.sampleRate ?? this.ctx.sampleRate;
    const buffer = this.ctx.createBuffer(rendered.length, frames, sr);
    for (let c = 0; c < rendered.length; c++) buffer.copyToChannel(rendered[c]!, c);

    this.heads = { ...this.heads, print: { target, phase: "finalising", detail: "committing", cycleFrames: frames } };
    const detail = `PRINT ${frames} frames (${(frames / sr).toFixed(3)}s) @ ${sr} Hz · ${rendered.length}ch · peak ${peak.toFixed(3)} · ${headsSummary(this.heads)}`;
    if (!this.commitPrint) {
      // No persistence layer installed: adopt in memory rather than lose the bake.
      const adopted = this.adoptBuffer(target, buffer, { name: `print ${target + 1}`, provenance: "user-private", decodeMs: 0, reused: true });
      this.heads = { ...this.heads, print: { target, phase: adopted.ok ? "done" : "failed", detail: adopted.detail, cycleFrames: frames } };
      return { ok: adopted.ok, detail: `${detail} · ${adopted.detail} · not persisted (no storage layer installed)` };
    }
    const commit = await this.commitPrint(target, buffer, detail);
    this.heads = { ...this.heads, print: { target, phase: commit.ok ? "done" : "failed", detail: commit.detail, cycleFrames: frames } };
    return { ok: commit.ok, detail: commit.ok ? `${detail} · ${commit.detail}` : `PRINT failed — ${commit.detail}. Target left empty, heads still audible.` };
  }


  // --------------------------------------------------------------- commands


  execute(cmd: AudioCommand): Ack {
    const p = cmd.payload;
    if (!this.ctx && (cmd.type.startsWith("track.") || cmd.type.startsWith("loop.") || cmd.type.startsWith("tape."))) {
      return this.ack(cmd, "rejected", "audio not unlocked — enable audio, then repeat the gesture");
    }
    try {
      switch (cmd.type) {
        case "transport.play": {
          if (!this.ctx) return this.ack(cmd, "rejected", "audio not unlocked — press Play again after enabling audio");
          if (this.ctx.state !== "running") return this.ack(cmd, "rejected", `AudioContext is ${this.ctx.state}`);
          if (this.duration === 0) return this.ack(cmd, "rejected", "no stems decoded");
          const resume = this.position() >= this.duration ? 0 : this.position();
          this.stopSources();
          this.requestedPlaying = true;
          const { started, startAt } = this.startAll(resume);
          this.fanout((t, at) => ({
            type: "start",
            applyAtContextFrame: at,
            sourceFrame: Math.round(this.timeline.positionAt(at / this.ctx!.sampleRate) * this.ctx!.sampleRate),
          }));
          if (started === 0 && this.workletTracks.length === 0) {
            this.requestedPlaying = false;
            return this.ack(cmd, "failed", "no source could be created");
          }
          return this.ack(cmd, "completed", `${started} stems scheduled at t=${startAt.toFixed(4)}s (spread 0.000 ms)`);
        }
        case "transport.stop": {
          if (!this.ctx) return this.ack(cmd, "rejected", "audio not unlocked");
          const pos = this.position();
          this.requestedPlaying = false;
          this.timelineFrozenAt = this.ctx.currentTime;
          this.timeline.anchor(this.ctx.currentTime, pos);
          this.stopSources();
          this.fanout((_t, at) => ({ type: "stop", applyAtContextFrame: at }));
          return this.ack(cmd, "completed", `stopped at ${pos.toFixed(3)}s`);
        }
        case "transport.restart": {
          if (!this.ctx) return this.ack(cmd, "rejected", "audio not unlocked");
          if (this.duration === 0) return this.ack(cmd, "rejected", "no stems decoded");
          this.stopSources();
          this.requestedPlaying = true;
          const { started, startAt } = this.startAll(0);
          this.fanout((_t, at) => ({ type: "restart", applyAtContextFrame: at }));
          return started > 0 || this.workletTracks.length > 0
            ? this.ack(cmd, "completed", `restarted from the top — ${started} stems at t=${startAt.toFixed(4)}s`)
            : this.ack(cmd, "failed", "restart produced no sources");
        }
        case "track.mute":
        case "track.unmute": {
          const id = Number(p["track"]) as TrackId;
          const t = this.tracks[id];
          if (!t) return this.ack(cmd, "rejected", `no track ${id}`);
          if (!t.buffer) return this.ack(cmd, "rejected", `track ${id + 1} has no audio loaded`);
          t.muted = cmd.type === "track.mute";
          // Correction 3: mute closes the FX rack inputs only. The fader stays
          // where the user left it and the echo/reverb tails decay naturally.
          if (this.ctx) this.applyAudibility(id);
          return this.ack(cmd, "completed", `track ${id + 1} ${t.muted ? "muted" : "unmuted"} (buffer retained)`);
        }
        case "track.gain": {
          const id = Number(p["track"]) as TrackId;
          const level = Number(p["level"]);
          if (!this.tracks[id]) return this.ack(cmd, "rejected", `no track ${id}`);
          this.applyTrackGain(id, level);
          return this.ack(cmd, "completed", `track ${id + 1} gain → ${level.toFixed(3)}`);
        }
        case "master.gain": {
          const level = Number(p["level"]);
          this.applyMasterGain(level);
          return this.ack(cmd, "completed", `master → ${level.toFixed(3)}`);
        }
        case "track.delete": {
          const id = Number(p["track"]) as TrackId;
          const t = this.tracks[id];
          if (!t) return this.ack(cmd, "rejected", `no track ${id}`);
          if (!t.buffer) return this.ack(cmd, "rejected", `track ${id + 1} already empty`);
          for (const s of t.sources) {
            try {
              s.node.onended = null;
              s.node.stop();
            } catch {
              /* noop */
            }
            s.node.disconnect();
          }
          t.sources = [];
          t.scheduledStartAt = null;
          t.trash = t.buffer;
          t.buffer = null;
          t.reversed = null;
          return this.ack(cmd, "completed", `track ${id + 1} unloaded → recoverable trash (blob kept)`);
        }
        case "track.restore": {
          const id = Number(p["track"]) as TrackId;
          const t = this.tracks[id];
          if (!t?.trash) return this.ack(cmd, "rejected", `nothing in track ${id + 1} trash`);
          t.buffer = t.trash;
          t.trash = null;
          if (this.requestedPlaying) {
            const pos = this.position();
            this.stopSources();
            this.startAll(pos);
          }
          return this.ack(cmd, "completed", `track ${id + 1} restored from trash`);
        }
        case "rate.set": {
          const rate = Number(p["rate"]);
          const glide = p["glide"] === false ? 0 : Number(p["tau"] ?? GLIDE_TAU);
          if (!Number.isFinite(rate) || rate <= 0) return this.ack(cmd, "rejected", `invalid rate ${String(p["rate"])}`);
          if (this.ctx) {
            const now = this.ctx.currentTime;
            const from = this.timeline.currentRate(now);
            if (glide > 0) {
              this.timeline.glideTo(now, rate, glide);
              const curve = glideCurve(from, rate, glide);
              const dur = glideDurationS(glide);
              for (const t of this.tracks)
                for (const s of t.sources) {
                  s.node.playbackRate.cancelScheduledValues(now);
                  // Finite scheduled curve: it TERMINATES on the target, so the
                  // audio and the integrated timeline agree exactly afterwards.
                  s.node.playbackRate.setValueCurveAtTime(curve, now, dur);
                }
            } else {
              this.timeline.setRate(now, rate);
              for (const t of this.tracks)
                for (const s of t.sources) s.node.playbackRate.setValueAtTime(rate, now);
            }
            // Worklet tracks apply the same ramp, inside the processor, at one
            // shared context frame — no rAF, no accumulating timer drift.
            this.fanout((_t, at) => ({
              type: "setRate",
              rate,
              rampFrames: glide > 0 ? Math.round(glideDurationS(glide) * this.ctx!.sampleRate) : 0,
              applyAtContextFrame: at,
            }));
            // Every future seam must be re-derived on the new curve.
            this.invalidateSeams();
          } else {
            this.timeline.setRate(0, rate);
          }
          return this.ack(
            cmd,
            "completed",
            `playback rate → ${rate.toFixed(4)}×${glide > 0 ? ` over ${(glideDurationS(glide) * 1000).toFixed(0)} ms (integrated)` : ""}; seams re-derived`,
          );
        }
        case "loop.set": {
          const id = Number(p["track"]) as TrackId;
          const t = this.tracks[id];
          if (!t) return this.ack(cmd, "rejected", `no track ${id}`);
          const enabled = p["enabled"] == null ? t.loop.enabled : Boolean(p["enabled"]);
          const start = p["start"] == null ? t.loop.start : Math.min(1, Math.max(0, Number(p["start"])));
          const end = p["end"] == null ? t.loop.end : Math.min(1, Math.max(0, Number(p["end"])));
          t.loop = { ...t.loop, enabled, start, end };
          this.invalidateSeams();
          if (t.engineMode === "worklet" && t.worklet && this.ctx) {
            const frames = Math.round(t.sourceDurationS * this.ctx.sampleRate);
            const at = sharedApplyFrame(this.ctx);
            void t.worklet.post({
              type: "setWindow",
              start: Math.round(Math.min(start, end) * frames),
              end: Math.round(Math.max(start, end) * frames),
              enabled,
              applyAtContextFrame: at,
            });
          }
          const bounds = this.loopBounds(t);
          // Loop-mode transitions are NOT click-free by themselves: entering or
          // leaving the window relocates the read pointer.
          const relocated = bounds && enabled ? this.relocate(t, bounds.start) : false;
          return this.ack(
            cmd,
            "completed",
            `track ${id + 1} loop ${enabled ? "on" : "off"} · window ${start.toFixed(3)}–${end.toFixed(3)} (normalized)` +
              `${bounds ? ` = ${bounds.length.toFixed(3)}s` : ""}${relocated ? " · dual-source crossfade" : ""}`,
          );
        }
        case "loop.chop": {
          const id = Number(p["track"]) as TrackId;
          const t = this.tracks[id];
          if (!t) return this.ack(cmd, "rejected", `no track ${id}`);
          const div = Math.max(1, Math.floor(Number(p["div"] ?? t.loop.chopDiv)));
          const index = Math.floor(Number(p["index"] ?? t.loop.chopIndex));
          t.loop = { ...t.loop, chopDiv: div, chopIndex: index, enabled: true };
          this.invalidateSeams();
          if (t.engineMode === "worklet" && t.worklet && this.ctx) {
            void t.worklet.post({
              type: "setChop",
              division: div,
              index: index,
              applyAtContextFrame: sharedApplyFrame(this.ctx),
            });
          }
          const bounds = this.loopBounds(t);
          const relocated = bounds ? this.relocate(t, bounds.start) : false;
          return this.ack(
            cmd,
            "completed",
            `track ${id + 1} chop 1/${div} slice ${((index % div) + div) % div}` +
              `${bounds ? ` · loopLength = windowWidth / ${div} = ${bounds.length.toFixed(3)}s` : ""}` +
              `${relocated ? " · dual-source crossfade" : ""}`,
          );
        }
        case "tape.reverse": {
          const id = Number(p["track"]) as TrackId;
          const t = this.tracks[id];
          if (!t) return this.ack(cmd, "rejected", `no track ${id}`);
          if (t.engineMode === "worklet" && t.worklet && this.ctx) {
            // Zero-copy: the processor negates its pointer step. No reversed
            // PCM is allocated, so reverseBytes stays exactly 0.
            const flip = p["on"] == null ? !t.loop.reverse : Boolean(p["on"]);
            t.loop = { ...t.loop, reverse: flip };
            void t.worklet.post({
              type: "setDirection",
              direction: flip ? -1 : 1,
              applyAtContextFrame: sharedApplyFrame(this.ctx),
            });
            return this.ack(
              cmd,
              "completed",
              `track ${id + 1} reverse ${flip ? "on" : "off"} — worklet pointer negated, 0 bytes of additional PCM`,
            );
          }
          if (!t.buffer) return this.ack(cmd, "rejected", `track ${id + 1} has no audio loaded`);
          const on = p["on"] == null ? !t.loop.reverse : Boolean(p["on"]);
          const projected = this.decodedTotalBytes + (on && !t.reversed ? bufferBytes(t.buffer) : 0);
          if (on && !allowed(judge(projected, this.budget, this.highMemoryMode))) {
            return this.ack(cmd, "rejected", describeVerdict(projected, this.budget, this.highMemoryMode));
          }
          const pos = this.position();
          t.loop = { ...t.loop, reverse: on };
          if (!on) t.reversed = null;
          const mirrored = Math.max(0, t.buffer.duration - pos);
          const relocated = this.relocate(t, on ? mirrored : Math.max(0, t.buffer.duration - pos));
          this.invalidateSeams();
          return this.ack(
            cmd,
            "completed",
            `track ${id + 1} reverse ${on ? "on" : "off"}${relocated ? " · dual-source crossfade" : ""}`,
          );
        }
        case "filter.set": {
          const id = Number(p["track"]) as TrackId;
          const t = this.tracks[id];
          if (!t) return this.ack(cmd, "rejected", `no track ${id}`);
          const mode = String(p["mode"] ?? "off") as "off" | "lp" | "hp";
          const cutoff = Number(p["cutoff"] ?? 1000);
          if (!this.ctx) return this.ack(cmd, "rejected", "audio not unlocked");
          this.applyFilter(t, mode, cutoff, Number(p["correlation"] ?? 1));
          return this.ack(
            cmd,
            "completed",
            mode === "off"
              ? `track ${id + 1} filter off — true dry bypass (complementary fade, no level bump)`
              : `track ${id + 1} ${mode.toUpperCase()} @ ${cutoff.toFixed(0)} Hz (complementary dry↔wet)`,
          );
        }
        case "song.load": {
          if (this.ctx) {
            // Lifecycle (correction 8): momentary state cleared, DSP history
            // faded out, Beat Repeat rings dropped. Stored FX config survives.
            this.flushAllFx();
            this.stopSources();
            this.requestedPlaying = false;
            this.timeline.setRate(this.ctx.currentTime, this.timeline.targetRate());
            this.timeline.anchor(this.ctx.currentTime, 0);
            this.timelineFrozenAt = this.ctx.currentTime;
          }
          return this.ack(cmd, "completed", `song ${Number(p["song"]) + 1} armed — transport stopped, waiting for Play`);
        }
        case "rollback": {
          const rate = Number(p["rate"]);
          if (Number.isFinite(rate) && rate > 0 && this.ctx) {
            this.timeline.setRate(this.ctx.currentTime, rate);
            for (const t of this.tracks)
              for (const s of t.sources) s.node.playbackRate.setValueAtTime(rate, this.ctx.currentTime);
            this.invalidateSeams();
          }
          const mask = String(p["mutes"] ?? "");
          if (mask.length === this.tracks.length) {
            this.tracks.forEach((t, i) => {
              t.muted = mask[i] === "1";
              if (this.ctx) this.applyAudibility(i as TrackId);
            });
          }
          return this.ack(
            cmd,
            "completed",
            `rolled back ${String(p["control"])} ×${String(p["toCount"])}${mask ? ` · mutes=${mask}` : ""}`,
          );
        }
        case "stem.solo": {
          const id = Number(p["track"]) as TrackId;
          if (!this.tracks[id]) return this.ack(cmd, "rejected", `no track ${id}`);
          this.setSolo(id, Boolean(p["on"]));
          return this.ack(
            cmd,
            "completed",
            `stem ${id + 1} solo ${p["on"] ? "on" : "off"} — mute state untouched (${this.tracks[id]!.muted ? "still muted" : "unmuted"})`,
          );
        }
        case "stem.link": {
          const id = Number(p["track"]) as TrackId;
          if (!this.tracks[id]) return this.ack(cmd, "rejected", `no track ${id}`);
          this.setLinked(id, Boolean(p["on"]));
          return this.ack(cmd, "completed", `stem ${id + 1} ${p["on"] ? "linked" : "unlinked"} — phase-continuous, no restart`);
        }
        case "stem.select":
          return this.ack(cmd, "completed", `active stem → ${Number(p["stem"]) + 1}`);
        case "fx.overlay":
          return this.ack(cmd, "completed", `FX overlay ${p["on"] ? "open" : "closed"} — tape audio unaffected`);
        case "fx.momentary.start":
        case "fx.momentary.end":
        case "fx.latch":
        case "fx.variation":
        case "fx.clearLatches": {
          const id = Number(p["track"]) as TrackId;
          const t = this.tracks[id];
          if (!t) return this.ack(cmd, "rejected", `no track ${id}`);
          if (!this.ctx) return this.ack(cmd, "rejected", "audio not unlocked");
          if (cmd.type === "fx.clearLatches") {
            for (const f of FX_FAMILIES) void this.setFxActive(id, f, false);
            return this.ack(cmd, "completed", `stem ${id + 1} latches cleared`);
          }
          const family = String(p["family"]) as FxFamily;
          if (!FX_FAMILIES.includes(family)) return this.ack(cmd, "rejected", `unknown FX family ${String(p["family"])}`);
          const latched = Boolean(p["latched"]);
          if (cmd.type === "fx.variation") {
            void this.setFxVariation(id, family, Number(p["variation"]), latched);
            return this.ack(cmd, "completed", `stem ${id + 1} ${family} variation → ${Number(p["variation"])}`);
          }
          const active = cmd.type === "fx.latch" ? Boolean(p["on"]) : cmd.type === "fx.momentary.start";
          void this.setFxActive(id, family, active, latched);
          return this.ack(
            cmd,
            "completed",
            `stem ${id + 1} ${family} ${active ? "engaged" : "released"}${latched ? " (latched)" : ""}`,
          );
        }
        // ---- Phase 6: recording, grid, heads/PRINT -------------------------
        case "rec.arm":
        case "rec.tap":
        case "rec.undoPass": {
          const id = Number(p["track"]) as TrackId;
          const rec = this.recording();
          if (!rec) return this.ack(cmd, "rejected", "audio not unlocked");
          if (!rec.model.inputEnabled)
            return this.ack(cmd, "rejected", "input is not enabled — open the input drawer and allow the microphone first");
          if (cmd.type === "rec.arm") rec.arm(id);
          else if (cmd.type === "rec.tap") rec.tap(id);
          else rec.doubleTap(id);
          return this.ack(cmd, "completed", rec.model.lastAck);
        }
        case "grid.tap": {
          const frame = this.ctx ? Math.round(this.ctx.currentTime * this.ctx.sampleRate) : 0;
          const sr = this.ctx?.sampleRate ?? 48000;
          if (this.grid.sampleRate !== sr) this.grid = { ...this.grid, sampleRate: sr };
          // 1500 ms of inactivity resets the learning window (v2.6 behaviour).
          const stale = this.lastGridTapFrame != null && frame - this.lastGridTapFrame > 1.5 * sr;
          if (stale) this.grid = emptyGrid(sr);
          this.grid = tapGrid(this.grid, frame, stale ? null : this.lastGridTapFrame);
          this.lastGridTapFrame = frame;
          return this.ack(
            cmd,
            "completed",
            this.grid.rejected
              ? "tap far outside 40–220 BPM — grid rejected, nothing moved"
              : this.grid.bpm == null
                ? `tap ${this.grid.intervals.length + 1} — keep tapping, three intervals lock the tempo`
                : `grid ${this.grid.bpm.toFixed(2)} BPM (${this.grid.source}, median of ${this.grid.intervals.length})`,
          );
        }
        case "grid.quantise": {
          this.quantisePunch = !this.quantisePunch;
          return this.ack(cmd, "completed", `punch quantise ${this.quantisePunch ? "on — punches snap to the grid" : "off — punches land where you press"}`);
        }
        case "heads.print":
          return this.ack(cmd, "rejected", "PRINT is reserved in this build — the gesture, LED slot and take shape exist, the render is not enabled");
        default:
          return this.ack(cmd, "rejected", "unknown command type");
      }
    } catch (err) {
      return this.ack(cmd, "failed", err instanceof Error ? err.message : String(err));
    }
  }

  status(): EngineStatus {
    const now = this.ctx?.currentTime ?? 0;
    const gate = this.migrationGate();
    return {
      contextState: this.ctx?.state ?? "none",
      sampleRate: this.ctx?.sampleRate ?? null,
      currentTime: now,
      requestedPlaying: this.requestedPlaying,
      actuallyPlaying:
        this.requestedPlaying && this.ctx?.state === "running" && this.tracks.some((t) => t.sources.length > 0),
      position: this.position(),
      duration: this.duration,
      rate: this.timeline.currentRate(now),
      targetRate: this.timeline.targetRate(),
      masterGain: this.masterLevel,
      startSpreadMs: this.startSpreadMs(),
      tracks: this.tracks.map((t, i) => {
        const bounds = t.buffer ? resolveLoop(t.loop, t.buffer.duration) : null;
        return {
          id: i,
          decoded: t.buffer != null,
          sourceLive: t.sources.length > 0,
          generation: t.generation,
          gain: t.muted ? 0 : t.level,
          muted: t.muted,
          scheduledStartAt: t.scheduledStartAt,
          name: t.name,
          provenance: t.provenance,
          trashed: t.trash != null,
          decodedBytes: (t.buffer ? bufferBytes(t.buffer) : 0) + (t.reversed ? bufferBytes(t.reversed) : 0),
          decodeCount: t.decodeCount,
          decodeMs: t.decodeMs,
          bufferReused: t.bufferReused,
          loop: t.loop,
          loopStartS: bounds?.start ?? 0,
          loopLengthS: bounds?.length ?? 0,
          filterMode: t.filterMode,
          seamCount: t.seamCount,
          liveSources: t.sources.length,
          nextSeamIn: t.committedSeamAt != null ? t.committedSeamAt - now : null,
          engineMode: t.engineMode,
          migrationStatus: t.migrationStatus,
          fallbackReason: t.fallbackReason,
          driftFrames: t.worklet?.driftFrames ?? null,
          workletPcmBytes: t.worklet?.pcmBytes ?? 0,
          workletWraps: t.worklet?.wrapCount ?? 0,
          renderGapFrames: t.worklet ? t.worklet.renderGapFrames : null,
          lastWorkletAck: t.worklet?.lastAck
            ? `seq ${t.worklet.lastAck.seq} ${t.worklet.lastAck.status}: ${t.worklet.lastAck.detail}`
            : null,
        };
      }),
      fx: this.fxSnapshots(),
      effectiveBpm: this.tracks.map((_t, i) => this.effectiveBpm(i as TrackId)),
      bpmSource: this.bpmSource,
      baseBpm: this.baseBpm,
      soloMask: this.tracks.map((t) => (t.soloed ? "1" : "0")).join(""),
      linkMask: this.tracks.map((t) => (t.linked ? "1" : "0")).join(""),
      lastFxRejection: this.lastFxRejection,
      decodedBytes: this.decodedTotalBytes,
      reverseBytes: this.reverseTotalBytes,
      budget: this.budget,
      highMemoryMode: this.highMemoryMode,
      memoryStatement: describeVerdict(this.decodedTotalBytes, this.budget, this.highMemoryMode),
      lastError: this.lastError,
      lastDecodeMs: this.lastDecodeMs,
      enginePreference: this.enginePreference,
      workletSupported:
        typeof AudioWorkletNode !== "undefined" && this.ctx != null && typeof this.ctx.audioWorklet !== "undefined",
      workletTrackCount: this.workletTracks.length,
      migrationWorstCaseBytes: gate.worstCasePeakBytes,
      migrationStatement: gate.statement,
      migrationAllowed: gate.allowed,
      lastMigrationPeakBytes: this.lastMigrationPeakBytes,
      migrationLog: this.migrationLog,
      preflight: this.lastPreflight?.checks ?? [],
      underrunLabel:
        this.workletTracks.length === 0
          ? "underrun unavailable — no worklet track"
          : `render-frame gap proxy: ${this.tracks.reduce((n, t) => n + (t.worklet?.renderGapFrames ?? 0), 0)} frames (proxy, not a hardware underrun count)`,
    };
  }

  /** Suspension / route change / backgrounding: never claim to be playing. */
  async reconcileLifecycle(): Promise<void> {
    if (!this.ctx) return;
    if (this.ctx.state !== "running" && this.requestedPlaying) {
      const pos = this.position();
      this.requestedPlaying = false;
      this.timelineFrozenAt = this.ctx.currentTime;
      this.timeline.anchor(this.ctx.currentTime, pos);
      this.stopSources();
      this.lastError = `AudioContext became ${this.ctx.state} — transport stopped, position held at ${pos.toFixed(2)}s`;
    }
  }

  dispose() {
    for (const t of this.tracks) {
      t.fxRack?.dispose();
      t.fxRack = null;
      void t.worklet?.dispose();
      t.worklet = null;
      t.engineMode = "node";
    }
    if (this.schedulerTimer) clearInterval(this.schedulerTimer);
    this.schedulerTimer = null;
    this.stopSources();
    void this.ctx?.close();
    this.ctx = null;
  }
}

/** Lazily built reverse copy — one extra buffer, gated by the memory budget. */
export function reverseBuffer(ctx: BaseAudioContext, src: AudioBuffer): AudioBuffer {
  const out = ctx.createBuffer(src.numberOfChannels, src.length, src.sampleRate);
  for (let c = 0; c < src.numberOfChannels; c++) {
    const from = src.getChannelData(c);
    const to = out.getChannelData(c);
    for (let i = 0, n = from.length; i < n; i++) to[i] = from[n - 1 - i]!;
  }
  return out;
}

let singleton: AudioEngine | null = null;

/** One context per app, never one per track. */
export function getAudioEngine(): AudioEngine {
  if (!singleton) singleton = new AudioEngine();
  return singleton;
}
