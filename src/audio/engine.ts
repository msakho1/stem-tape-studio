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
import {
  DEFAULT_INERTIA_PRESET,
  INERTIA_MIN_RATE,
  INERTIA_PRESETS,
  INERTIA_STOP_FADE_S,
  inertiaCurve,
  makeInertiaSegment,
  type InertiaPresetName,
  type TransportPhase,
  CUE_FADE_S,
} from "./inertia";
import { estimateMigration } from "./workletBudget";
import { pairwiseDrift, sharedApplyFrame, type WorkletAck } from "./workletProtocol";
import { preflightWorklet, WorkletTrack, type MigrationStatus, type PreflightResult } from "./workletTrack";
import { FxRack, type FxRackSnapshot } from "./fx/rack";
import { BankRack, type BankStageSnapshot } from "./fx/banks";
import type { AlgorithmIndex, BankIndex } from "@/machine/fx12";
import { algorithmDef } from "@/machine/fx12";
import { FX_FAMILIES, type FxFamily } from "@/machine/stemPerformance";
import { RecordingController } from "./input/recorder";
import { TapeTimelineBus, type TapeTimelineEvent } from "./timelineEvents";
import { PerformanceRecorder } from "./export/performanceRecorder";
import { emptyGrid, tapGrid, type GridState } from "./grid";
import {
  chooseSource,
  emptyHeads,
  enterHeads,
  exitHeads,
  headsSummary,
  peakOf,
  relinkSource,
  renderHeadsCycle,
  setHeadLevel,
  scrubHead,
  toggleHeadMute,
  toggleHeadReverse,
  type HeadState,
  type HeadsState,
  type SourceCandidate,
  headReadPosition,
} from "./heads";
import {
  MAX_SCRUB_RATE,
  SCRUB_SILENCE_RATE,
  ScrubLog,
  ScrubTracker,
  type ScrubEvent,
} from "./scrub";
import type { ScrubTelemetryHead } from "./workletProtocol";



export type EnginePreference = "node" | "worklet";

export const LOOKAHEAD_S = 0.08;
export const RAMP_TAU = 0.008;
/** How far ahead the seam scheduler commits work, seconds. */
export const SEAM_LOOKAHEAD_S = 0.25;
const SCHEDULER_INTERVAL_MS = 25;
/** Held-shuttle transport speed, in source seconds per real second. */
const GLOBAL_SCRUB_RATE = 1.6;
/** Grain scheduler period for the shuttle (ms). */
const GLOBAL_SCRUB_INTERVAL_MS = 45;
/** Grain level; the shuttle is deliberately quieter than normal playback. */
const GLOBAL_SCRUB_LEVEL = 0.85;

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
  /** Workstream 3: four serial bank stages, permanently inserted. */
  bankRack: BankRack | null;
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
  /** Instantaneous inertia rate — what you hear right now. */
  rate: number;
  /** Musical target rate — preserved across wind-up/wind-down. */
  targetRate: number;
  transportPhase: TransportPhase;
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
  /** Phase 6 heads/PRINT truth, straight from the engine. */
  heads: HeadsState;
  headsSummary: string;
  /** Live scrub evidence: open gestures, kernel telemetry, emitted events. */
  scrub: {
    open: ({ head: number; pointerId: number; previews: number; lastVelocity: number } | null)[];
    telemetry: { contextFrame: number; rms: number; heads: (ScrubTelemetryHead | null)[] } | null;
    events: ScrubEvent[];
  };



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
  /** Correction 6 — the one authoritative timeline event stream. */
  readonly timelineBus = new TapeTimelineBus();
  private requestedPlaying = false;
  /** Workstream 2: tape inertia. Classic (300 ms / 450 ms) is the default. */
  private inertiaPreset: InertiaPresetName = DEFAULT_INERTIA_PRESET;
  private inertiaEnabled = true;
  private transportPhase: TransportPhase = "stopped";
  private windTimer: ReturnType<typeof setTimeout> | null = null;
  /** Dedicated transport envelope — never the fader, never the mute gain. */
  private transportGain: GainNode | null = null;
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
  /** §2.2 — track held for recording before input exists; armed once granted. */
  pendingInputTrack: TrackId | null = null;

  /** Called by the input UI after permission lands: honours the pending hold. */
  resolvePendingInput(): { ok: boolean; detail: string } {
    const id = this.pendingInputTrack;
    if (id == null) return { ok: false, detail: "no pending input request" };
    const rec = this.recording();
    if (!rec?.model.inputEnabled) return { ok: false, detail: "input still not enabled" };
    rec.arm(id);
    this.pendingInputTrack = null;
    return { ok: true, detail: `pending hold honoured — track ${id + 1} armed: ${rec.model.lastAck}` };
  }

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
      // SOS follows the timeline through ONE subscription. No engine branch has
      // to remember to call followRate() ever again.
      const rec = this.recorder;
      this.timelineBus.subscribe((ev: TapeTimelineEvent) => {
        if (ev.type === "RateChange") {
          for (let i = 0; i < engine.tracks.length; i++) rec.followRate(i, ev.rate, ev.rampFrames);
        } else if (ev.type === "DirectionChange") {
          // Reverse stays REJECTED for recorded layers until reverse recording
          // is approved — the take mixer keeps its forward rate.
          engine.lastError = "reverse is not applied to recorded take layers (reverse recording unapproved)";
        }
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
    this.transportGain = ctx.createGain();
    this.transportGain.gain.value = 1;
    this.master.connect(this.transportGain);
    this.transportGain.connect(this.masterAnalyser);
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
      // Phase 5C / Workstream 3:
      //   preFx → FxRack (tape filter + Beat Repeat worklet)
      //         → BankRack (TONE → RHYTHM → MOTION → SPACE)
      //         → fader → solo → analyser → master.
      const bankRack = new BankRack(ctx);
      const fxRack = new FxRack(ctx, bankRack.input);
      bankRack.output.connect(gain);
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
        bankRack,
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
    // Root cause 3: during a held shuttle `requestedPlaying` is false, so the
    // frozen branch pinned the reported playhead even while the tape moved.
    // The shuttle owns the position while it is open.
    if (this.globalScrub) return Math.min(this.duration, Math.max(0, this.globalScrub.pos));
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
    // Node-engine heads follow varispeed like any other voice; no seams apply
    // because each head loops on its own loopStart/loopEnd.
    if (this.heads.active && this.heads.engine === "node") {
      const rate = Math.abs(this.timeline.currentRate(now)) || 1;
      for (const v of this.headVoices) if (v) v.node.playbackRate.setTargetAtTime(rate, now, RAMP_TAU);
    }
    for (const t of this.tracks) {
      if (this.heads.active && this.heads.engine === "node" && this.heads.source === this.tracks.indexOf(t)) continue;
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
    // Engine swaps are refused while heads are audible or a PRINT is baking:
    // both read the source PCM and a handoff would move it under them.
    if (this.heads.active)
      return { ok: false, detail: `refused — heads mode is active (source track ${(this.heads.source ?? 0) + 1}); exit heads before switching engines` };
    if (this.heads.print && (this.heads.print.phase === "rendering" || this.heads.print.phase === "finalising"))
      return { ok: false, detail: "refused — a PRINT render is in flight" };


    t.migrationStatus = "checking";
    const pre = await this.preflight();
    if (!pre.ok) {
      t.migrationStatus = "refused";
      t.fallbackReason = pre.checks.filter((c) => !c.ok).map((c) => c.detail).join(" · ");
      this.note(`T${id + 1} migration refused — ${t.fallbackReason}`);
      return { ok: false, detail: t.fallbackReason };
    }

    const wt = new WorkletTrack(id, this.ctx, t.input, (w, detail) => this.handleProcessorError(w, detail));
    wt.onTelemetry = (_w, ack) => {
      if (ack.detail !== "scrub" || !ack.scrubHeads) return;
      this.scrubTelemetry = { contextFrame: ack.contextFrame ?? 0, rms: ack.rms ?? 0, heads: ack.scrubHeads };
    };
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

  /**
   * Per-stem effective BPM: baseBpm × |MUSICAL rate|.
   *
   * Correction 8: tempo-derived effects (Pump, Echo, Gate, Beat Repeat) must
   * follow the musical rate, never the instantaneous inertia rate — otherwise a
   * wind-down collapses the LFO to ~0 Hz and Pump goes silent.
   */
  effectiveBpm(id: TrackId): number {
    const rate = Math.abs(this.timeline.musicalRate()) || 1;
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

  // ------------------------------------------------- Workstream 3: 12 FX ---

  /**
   * Beat Repeat is the one algorithm that stays on its sample-accurate
   * worklet inside the legacy rack; its bank stage is a passthrough.
   */
  private isWorkletAlgorithm(bank: BankIndex, algorithm: AlgorithmIndex): boolean {
    return algorithmDef(bank, algorithm).id === "beatRepeat";
  }

  /** Zero hold latency: the wet ramp starts at the current context time. */
  async setBankActive(
    id: TrackId,
    bank: BankIndex,
    algorithm: AlgorithmIndex,
    active: boolean,
    latched = false,
  ): Promise<{ ok: boolean; detail: string }> {
    const t = this.tracks[id];
    if (!t?.bankRack || !this.ctx) return { ok: false, detail: "audio not unlocked" };
    const def = algorithmDef(bank, algorithm);
    if (this.isWorkletAlgorithm(bank, algorithm)) {
      return this.setFxActive(id, "beatRepeat", active, latched);
    }
    const stage = t.bankRack.stage(bank);
    stage.setTempo(this.effectiveBpm(id), this.ctx.currentTime);
    stage.select(algorithm, this.ctx.currentTime);
    stage.setActive(active, this.ctx.currentTime);
    const rejected = stage.rejectionFor(algorithm);
    if (rejected) {
      this.lastFxRejection = `${def.label}: ${rejected}`;
      // One algorithm is refused; the bank itself stays usable.
      return { ok: false, detail: `${def.label} rejected — ${rejected}; bank ${bank + 1} still usable` };
    }
    return {
      ok: true,
      detail: `stem ${id + 1} ${def.label} ${active ? "engaged" : "released"}${latched ? " (latched)" : ""} · wet ${stage.snapshot().wet.toFixed(3)}`,
    };
  }

  /** Cycling never activates an inactive bank. */
  selectBankAlgorithm(id: TrackId, bank: BankIndex, algorithm: AlgorithmIndex): { ok: boolean; detail: string } {
    const t = this.tracks[id];
    if (!t?.bankRack || !this.ctx) return { ok: false, detail: "audio not unlocked" };
    const stage = t.bankRack.stage(bank);
    const wasWorklet = this.isWorkletAlgorithm(bank, stage.selected);
    const active = stage.isActive || (wasWorklet && t.fxActive.beatRepeat);
    // Leaving Beat Repeat must silence its worklet, not leave it running.
    if (wasWorklet && !this.isWorkletAlgorithm(bank, algorithm) && t.fxActive.beatRepeat) {
      void this.setFxActive(id, "beatRepeat", false);
    }
    stage.select(algorithm, this.ctx.currentTime);
    if (active && this.isWorkletAlgorithm(bank, algorithm)) {
      stage.setActive(false, this.ctx.currentTime);
      void this.setFxActive(id, "beatRepeat", true);
    }
    return { ok: true, detail: `stem ${id + 1} bank ${bank + 1} → ${algorithmDef(bank, algorithm).label}` };
  }

  /** Macro values are PER ALGORITHM, never shared across a bank. */
  setBankMacro(id: TrackId, bank: BankIndex, algorithm: AlgorithmIndex, value: number): { ok: boolean; detail: string } {
    const t = this.tracks[id];
    if (!t?.bankRack || !this.ctx) return { ok: false, detail: "audio not unlocked" };
    const v = Math.min(1, Math.max(0, value));
    t.bankRack.stage(bank).setMacro(algorithm, v, this.ctx.currentTime);
    if (this.isWorkletAlgorithm(bank, algorithm)) {
      // The legacy Beat Repeat exposes four presets; the macro selects one.
      void this.setFxVariation(id, "beatRepeat", Math.min(4, Math.max(1, Math.round(v * 3) + 1)));
    }
    return { ok: true, detail: `stem ${id + 1} ${algorithmDef(bank, algorithm).label} macro → ${v.toFixed(2)}` };
  }

  clearBanks(id: TrackId): { ok: boolean; detail: string } {
    const t = this.tracks[id];
    if (!t?.bankRack || !this.ctx) return { ok: false, detail: "audio not unlocked" };
    for (const stage of t.bankRack.stages) stage.setActive(false, this.ctx.currentTime);
    void this.setFxActive(id, "beatRepeat", false);
    return { ok: true, detail: `stem ${id + 1} all four banks released` };
  }

  bankSnapshots(): (BankStageSnapshot[] | null)[] {
    return this.tracks.map((t) => t.bankRack?.snapshot() ?? null);
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
    // Any open scrub must be closed before the heads layer disappears.
    this.cancelAllScrubs();
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

  // ------------------------------------------------- audible head scrubbing

  /** Emitted-event trail; the diagnostic panel reads it verbatim. */
  readonly scrubLog = new ScrubLog();
  private scrubTrackers: (ScrubTracker | null)[] = [null, null, null, null];
  /** Node-fallback grain state: absolute read frame + last grain time. */
  private nodeScrub: ({ pos: number; level: number; lastGrainAt: number } | null)[] = [null, null, null, null];
  /** Last telemetry frame received from the worklet kernel. */
  scrubTelemetry: { contextFrame: number; rms: number; heads: (ScrubTelemetryHead | null)[] } | null = null;

  /** Head read position (absolute source frames) right now. */
  private headFrameNow(i: number): number {
    const h = this.heads.heads[i]!;
    const cf = Math.max(1, this.heads.cycleFrames);
    const sr = this.tracks[this.heads.source ?? 0]?.buffer?.sampleRate ?? this.ctx?.sampleRate ?? 48000;
    const posFrames = this.ctx ? this.timeline.positionAt(this.ctx.currentTime) * sr : this.heads.cycleStartFrame;
    return headReadPosition(h, posFrames, this.heads.cycleStartFrame, cf);
  }

  private logScrub(type: ScrubEvent["type"], headId: number, pointerId: number, normalized: number, target: number) {
    this.scrubLog.push({
      type,
      headId,
      pointerId,
      normalizedPosition: normalized,
      targetSourceFrame: target,
      contextFrame: this.ctx ? Math.round(this.ctx.currentTime * this.ctx.sampleRate) : 0,
      inputTimestamp: typeof performance !== "undefined" ? performance.now() : 0,
    });
  }

  private scrubWorklet(head: number, phase: "start" | "preview" | "end" | "cancel", pointerId: number, normalized: number, deltaFrames: number) {
    const src = this.heads.source;
    const t = src != null ? this.tracks[src] : null;
    if (this.heads.engine !== "worklet" || !t?.worklet) return false;
    void t.worklet.headScrub({ head, phase, pointerId, normalizedPosition: normalized, deltaFrames });
    return true;
  }

  /**
   * Node fallback: the head's looping voice is silenced and the movement is
   * rendered as short overlapping grains whose playbackRate IS the scrub speed.
   * It is a documented degradation, not a different musical behaviour.
   */
  private scrubNodeGrain(head: number, deltaFrames: number, dtS: number) {
    const ctx = this.ctx;
    const src = this.heads.source;
    const t = src != null ? this.tracks[src] : null;
    const st = this.nodeScrub[head];
    if (!ctx || !t?.buffer || !this.headsBus || !st) return;
    const sr = t.buffer.sampleRate;
    const cf = Math.max(1, this.heads.cycleFrames);
    const cs = this.heads.cycleStartFrame;
    const rate = Math.min(MAX_SCRUB_RATE, Math.abs(deltaFrames) / Math.max(1e-4, dtS) / sr);
    st.pos = cs + (((st.pos + deltaFrames - cs) % cf) + cf) % cf;
    if (rate < SCRUB_SILENCE_RATE) return;
    const now = ctx.currentTime;
    const at = Math.max(now, st.lastGrainAt);
    const dur = Math.min(0.12, Math.max(0.02, dtS * 1.6));
    const backwards = deltaFrames < 0;
    if (backwards && !t.reversed) t.reversed = reverseBuffer(ctx, t.buffer);
    const buffer = backwards ? t.reversed! : t.buffer;
    const offsetS = backwards ? buffer.duration - st.pos / sr : st.pos / sr;
    const g = ctx.createGain();
    g.connect(this.headsBus);
    const node = ctx.createBufferSource();
    node.buffer = buffer;
    node.playbackRate.value = Math.max(0.02, rate);
    node.connect(g);
    const level = st.level * Math.min(1, rate / SCRUB_SILENCE_RATE);
    const fade = Math.min(0.006, dur / 3);
    g.gain.setValueAtTime(0, at);
    g.gain.linearRampToValueAtTime(level, at + fade);
    g.gain.setValueAtTime(level, at + dur - fade);
    g.gain.linearRampToValueAtTime(0, at + dur);
    node.start(at, Math.min(Math.max(0, offsetS), Math.max(0, buffer.duration - 1e-3)), dur * node.playbackRate.value);
    st.lastGrainAt = at + dur * 0.75;
    node.onended = () => {
      try {
        node.disconnect();
        g.disconnect();
      } catch {
        /* noop */
      }
    };
  }

  // ------------------------------------------------- global four-stem shuttle
  /**
   * Hotfix: FUNCTION + rocker (and F+Q / F+A on the keyboard) is a HELD tape
   * shuttle, not a discrete jump. One shared playhead moves for all four stems
   * and the movement is rendered as overlapping grains through each track's own
   * input node, so faders, mutes, solo and FX all stay in the signal path.
   */
  private globalScrub: {
    dir: 1 | -1;
    pos: number;
    startPos: number;
    /** Context time at which `pos` was last integrated — the integral anchor. */
    posCtxTime: number;
    wasPlaying: boolean;
    musicalRate: number;
    timer: ReturnType<typeof setInterval> | null;
    last: number;
    lastGrainAt: number[];
    grains: number;
    startedAt: number;
    /** Scheduler generation. Bumped on release; stale grains are never emitted. */
    gen: number;
    /** Every grain currently scheduled or sounding, per track. */
    live: { node: AudioBufferSourceNode; gain: GainNode; at: number; endAt: number; gen: number }[][];
    /** Per-track evidence — the shuttle is proven per stem, never by master RMS. */
    perTrack: {
      mode: "node" | "worklet";
      startPos: number;
      pos: number;
      grains: number;
      peak: number;
      rms: number;
    }[];
  } | null = null;

  /** Release handoff evidence — one record per stem, per release. */
  lastScrubHandoff: {
    keyupContextFrame: number;
    handoffContextFrame: number;
    fadeMs: number;
    stems: {
      id: number;
      mode: "node" | "worklet";
      landingFrame: number;
      restartFrame: number;
      landingErrorFrames: number;
      queuedGrainsBefore: number;
      queuedGrainsAfter: number;
      activeScrubSources: number;
      activeNormalSources: number;
      livePlaybackPaths: number;
    }[];
  } | null = null;


  /**
   * Per-track scrub-path analysers. The shuttle grains are tapped BEFORE the
   * track input so the measurement can never be satisfied by the ordinary stem
   * signal — a silent scrub processor reads zero here even while music plays.
   */
  private scrubTaps: (AnalyserNode | null)[] = [null, null, null, null];
  private scrubTapBuf: Float32Array | null = null;

  private scrubTap(id: number): AnalyserNode | null {
    const ctx = this.ctx;
    if (!ctx) return null;
    let a = this.scrubTaps[id] ?? null;
    if (!a) {
      a = ctx.createAnalyser();
      a.fftSize = 2048;
      this.scrubTaps[id] = a;
    }
    return a;
  }

  /** Sample every scrub tap; called on each shuttle tick. */
  private sampleScrubTaps() {
    const gs = this.globalScrub;
    if (!gs) return;
    for (let i = 0; i < gs.perTrack.length; i++) {
      const a = this.scrubTaps[i];
      if (!a) continue;
      if (!this.scrubTapBuf || this.scrubTapBuf.length !== a.fftSize) {
        this.scrubTapBuf = new Float32Array(new ArrayBuffer(a.fftSize * 4));
      }
      const buf = this.scrubTapBuf;
      a.getFloatTimeDomainData(buf as Float32Array<ArrayBuffer>);
      let sum = 0;
      let peak = 0;
      for (let k = 0; k < buf.length; k++) {
        const v = buf[k]!;
        sum += v * v;
        const av = v < 0 ? -v : v;
        if (av > peak) peak = av;
      }
      const rms = Math.sqrt(sum / buf.length);
      const pt = gs.perTrack[i]!;
      if (rms > pt.rms) pt.rms = rms;
      if (peak > pt.peak) pt.peak = peak;
    }
  }

  globalScrubState() {
    const gs = this.globalScrub;
    return {
      active: gs != null,
      direction: gs?.dir ?? 0,
      position: gs?.pos ?? this.position(),
      grains: gs?.grains ?? 0,
      wasPlaying: gs?.wasPlaying ?? false,
      musicalRate: gs?.musicalRate ?? this.timeline.musicalRate(),
      /** Read pointer + scrub-path output per stem — the four-pointer proof. */
      tracks: (gs?.perTrack ?? this.tracks.map(() => null)).map((pt, i) => ({
        id: i,
        scrubActive: gs != null && pt != null,
        mode: pt?.mode ?? this.tracks[i]?.engineMode ?? "node",
        readPosition: pt?.pos ?? this.position(),
        displacement: pt ? pt.pos - pt.startPos : 0,
        grains: pt?.grains ?? 0,
        scrubRms: pt?.rms ?? 0,
        scrubPeak: pt?.peak ?? 0,
      })),
      lastRejection: this.lastScrubRejection,
    };
  }

  /** Why the most recent shuttle attempt was refused, for the diagnostic record. */
  lastScrubRejection: string | null = null;

  private scrubGrainAll(dtS: number) {
    const gs = this.globalScrub;
    const ctx = this.ctx;
    if (!gs || !ctx) return;
    const before = gs.pos;
    gs.pos = Math.min(this.duration, Math.max(0, gs.pos + gs.dir * GLOBAL_SCRUB_RATE * dtS));
    gs.posCtxTime = ctx.currentTime;
    // ONE shared playhead: the timeline is re-anchored, every stem reads it.
    this.timeline.anchor(ctx.currentTime, gs.pos);
    this.timelineFrozenAt = ctx.currentTime;
    for (const pt of gs.perTrack) pt.pos = gs.pos;
    if (gs.pos === before) return; // parked at an end — no grain, no click
    const now = ctx.currentTime;
    const dur = Math.min(0.12, Math.max(0.03, dtS * 1.6));
    const backwards = gs.dir < 0;
    const gen = gs.gen;
    let emitted = 0;
    for (let i = 0; i < this.tracks.length; i++) {
      const t = this.tracks[i]!;
      const pt = gs.perTrack[i];
      if (!pt) continue;
      if (pt.mode === "worklet") {
        // The worklet kernel renders its own shuttle: it is already running at
        // GLOBAL_SCRUB_RATE in `gs.dir`. Only re-align its read pointer so all
        // four stems stay locked to the single shared playhead.
        pt.grains++;
        emitted++;
        continue;
      }
      const src = t.buffer ?? this.scrubPcm(t);
      if (!src) continue;
      if (backwards && !t.reversed) t.reversed = reverseBuffer(ctx, src);
      const buffer = backwards ? t.reversed : src;
      if (!buffer) continue;
      const at = Math.max(now, gs.lastGrainAt[i] ?? now);
      const offsetS = backwards ? buffer.duration - gs.pos : gs.pos;
      const g = ctx.createGain();
      g.connect(t.input);
      const tap = this.scrubTap(i);
      if (tap) g.connect(tap);
      const node = ctx.createBufferSource();
      node.buffer = buffer;
      node.playbackRate.value = GLOBAL_SCRUB_RATE;
      node.connect(g);
      const fade = Math.min(0.006, dur / 3);
      g.gain.setValueAtTime(0, at);
      g.gain.linearRampToValueAtTime(GLOBAL_SCRUB_LEVEL, at + fade);
      g.gain.setValueAtTime(GLOBAL_SCRUB_LEVEL, at + dur - fade);
      g.gain.linearRampToValueAtTime(0, at + dur);
      node.start(at, Math.min(Math.max(0, offsetS), Math.max(0, buffer.duration - 1e-3)), dur * GLOBAL_SCRUB_RATE);
      gs.lastGrainAt[i] = at + dur * 0.8;
      pt.grains++;
      emitted++;
      const rec = { node, gain: g, at, endAt: at + dur, gen };
      (gs.live[i] ??= []).push(rec);
      node.onended = () => {
        const cur = this.globalScrub;
        if (cur) cur.live[i] = (cur.live[i] ?? []).filter((r) => r !== rec);
        try {
          node.disconnect();
          g.disconnect();
        } catch {
          /* noop */
        }
      };
    }
    if (emitted > 0) gs.grains++;
    this.sampleScrubTaps();
  }


  /**
   * PCM for a track whose node buffer was released after worklet handoff.
   * Falls back to the reversed copy (re-reversed lazily) so a migrated track is
   * never silently skipped by the shuttle.
   */
  private scrubPcm(t: TrackRuntime): AudioBuffer | null {
    if (t.buffer) return t.buffer;
    if (t.reversed && this.ctx) {
      t.buffer = reverseBuffer(this.ctx, t.reversed);
      return t.buffer;
    }
    return null;
  }

  private globalScrubTick() {
    const gs = this.globalScrub;
    if (!gs) return;
    const nowMs = typeof performance !== "undefined" ? performance.now() : Date.now();
    const dt = Math.min(0.2, Math.max(0, (nowMs - gs.last) / 1000));
    gs.last = nowMs;
    if (dt > 0) this.scrubGrainAll(dt);
  }

  /** Point every worklet track at the shuttle rate/direction from `pos`. */
  private worbletShuttle(dir: 1 | -1, pos: number, engage: boolean) {
    const ctx = this.ctx;
    if (!ctx) return;
    const sr = ctx.sampleRate;
    this.fanout((_t, at) => ({ type: "setDirection", applyAtContextFrame: at, direction: dir }));
    this.fanout((_t, at) => ({
      type: "setRate",
      applyAtContextFrame: at,
      rate: engage ? GLOBAL_SCRUB_RATE : this.timeline.musicalRate(),
      rampFrames: Math.round(0.008 * sr),
    }));
    if (engage) {
      this.fanout((_t, at) => ({ type: "start", applyAtContextFrame: at, sourceFrame: Math.round(pos * sr) }));
    }
  }

  beginGlobalScrub(dir: 1 | -1): { ok: boolean; detail: string } {
    // Root cause 1: the shuttle was refused outright on a suspended/never-run
    // context and the refusal never reached the surface. Resume first, then
    // report a precise reason if it is still impossible.
    const ctx = this.ctx;
    if (!ctx) {
      this.lastScrubRejection = "audio not unlocked — the shuttle needs the AudioContext created by a user gesture";
      return { ok: false, detail: this.lastScrubRejection };
    }
    if (ctx.state !== "running") void ctx.resume();
    if (this.duration === 0) {
      this.lastScrubRejection = "no stems decoded — load or demo four stems before shuttling";
      return { ok: false, detail: this.lastScrubRejection };
    }
    if (this.globalScrub) return this.setGlobalScrubDirection(dir);
    this.lastScrubRejection = null;
    const now = ctx.currentTime;
    const pos = this.position();
    const wasPlaying = this.requestedPlaying;
    const musicalRate = this.timeline.musicalRate();
    this.cancelWind();
    this.stopSources();
    this.requestedPlaying = false;
    this.timeline.anchor(now, pos);
    this.timelineFrozenAt = now;
    this.globalScrub = {
      dir,
      pos,
      startPos: pos,
      posCtxTime: now,
      wasPlaying,
      musicalRate,
      gen: 1,
      live: this.tracks.map(() => []),
      timer: null,

      last: typeof performance !== "undefined" ? performance.now() : Date.now(),
      lastGrainAt: this.tracks.map(() => now),
      grains: 0,
      startedAt: now,
      perTrack: this.tracks.map((t) => ({
        mode: t.engineMode === "worklet" && t.worklet ? ("worklet" as const) : ("node" as const),
        startPos: pos,
        pos,
        grains: 0,
        peak: 0,
        rms: 0,
      })),
    };
    // Worklet tracks shuttle inside the kernel; node tracks shuttle as grains.
    this.worbletShuttle(dir, pos, true);
    this.globalScrub.timer = setInterval(() => this.globalScrubTick(), GLOBAL_SCRUB_INTERVAL_MS);
    const wk = this.globalScrub.perTrack.filter((p) => p.mode === "worklet").length;
    return {
      ok: true,
      detail: `global shuttle ${dir > 0 ? "forward" : "backward"} at ${GLOBAL_SCRUB_RATE}× from ${pos.toFixed(3)}s — four stems on one playhead (${wk} worklet / ${4 - wk} node)`,
    };
  }

  /** Advance the shuttle integral to `atCtx` without emitting a grain. */
  private integrateScrubTo(atCtx: number): number {
    const gs = this.globalScrub;
    if (!gs) return this.position();
    const dt = Math.max(0, atCtx - gs.posCtxTime);
    const p = Math.min(this.duration, Math.max(0, gs.pos + gs.dir * GLOBAL_SCRUB_RATE * dt));
    return p;
  }

  setGlobalScrubDirection(dir: 1 | -1): { ok: boolean; detail: string } {
    const gs = this.globalScrub;
    if (!gs) return this.beginGlobalScrub(dir);
    if (gs.dir !== dir) {
      const ctx = this.ctx;
      if (ctx) {
        gs.pos = this.integrateScrubTo(ctx.currentTime);
        gs.posCtxTime = ctx.currentTime;
      }
      gs.dir = dir;
      this.worbletShuttle(dir, gs.pos, true);
    }
    return { ok: true, detail: `shuttle direction → ${dir > 0 ? "forward" : "backward"} at ${gs.pos.toFixed(3)}s` };
  }

  /**
   * Scrub → playback handoff.
   *
   * The release is a single SHARED context frame on the next render quantum —
   * NOT the 80 ms transport lookahead, which overshoots audibly after key
   * release. At that frame the scrub integral is evaluated once; the resulting
   * landing position is authoritative for every stem, node and worklet alike.
   * The scheduler generation is invalidated in the same call, so no further
   * grain can be emitted, every queued future grain is stopped outright, and
   * anything already sounding is taken to zero over a 4 ms COMPLEMENTARY fade
   * (correlated material — an equal-power fade would bump +3 dB). Exactly one
   * playback path per stem survives.
   */
  endGlobalScrub(): { ok: boolean; detail: string } {
    const gs = this.globalScrub;
    const ctx = this.ctx;
    if (!gs || !ctx) return { ok: false, detail: "no global scrub active" };
    if (gs.timer) clearInterval(gs.timer);
    gs.timer = null;
    const sr = ctx.sampleRate;
    const keyup = ctx.currentTime;
    // One shared handoff on the next render quantum (two quanta of margin so
    // the scheduling itself cannot land in the past).
    const quantum = 128 / sr;
    const handoff = keyup + Math.max(2 * quantum, SCRUB_HANDOFF_MIN_S);
    const landing = this.integrateScrubTo(handoff);
    const landingFrame = Math.round(landing * sr);
    const handoffFrame = Math.round(handoff * sr);

    // 1. Invalidate the scheduler generation — no further grains, ever.
    gs.gen++;
    const queuedBefore = gs.live.map((l) => l.length);

    // 2. Cancel queued grains, fade sounding ones over the short handoff.
    const stillSounding: number[] = this.tracks.map(() => 0);
    for (let i = 0; i < gs.live.length; i++) {
      for (const rec of gs.live[i] ?? []) {
        try {
          rec.gain.gain.cancelScheduledValues(handoff);
          if (rec.at >= handoff) {
            // Never started: kill it outright, it cannot be allowed to appear.
            rec.gain.gain.setValueAtTime(0, handoff);
            rec.node.stop(handoff);
          } else {
            const g = complementary(0).a * GLOBAL_SCRUB_LEVEL;
            rec.gain.gain.setValueAtTime(g, handoff);
            rec.gain.gain.linearRampToValueAtTime(0, handoff + SCRUB_HANDOFF_FADE_S);
            rec.node.stop(handoff + SCRUB_HANDOFF_FADE_S);
            stillSounding[i] = (stillSounding[i] ?? 0) + 1;
          }
        } catch {
          /* already stopped */
        }
      }
      gs.live[i] = [];
    }

    const evidence = gs.perTrack
      .map((p, i) => `T${i + 1} ${p.mode} Δ${(p.pos - p.startPos).toFixed(3)}s g${p.grains} rms${p.rms.toFixed(4)}`)
      .join(", ");
    const perTrack = gs.perTrack;
    const moved = landing - gs.startPos;
    this.globalScrub = null;

    // 3. Re-anchor every timeline and the visible playhead to the handoff.
    this.timeline.anchor(handoff, landing);
    this.timelineFrozenAt = handoff;

    // 4. Start (or park) normal playback at exactly the same shared frame.
    let started = 0;
    if (gs.wasPlaying) {
      this.requestedPlaying = true;
      started = this.startAllAt(landing, handoff).started;
      this.fanoutAt(handoffFrame, () => ({ type: "setDirection", applyAtContextFrame: handoffFrame, direction: 1 }));
      this.fanoutAt(handoffFrame, () => ({
        type: "setRate",
        applyAtContextFrame: handoffFrame,
        rate: gs.musicalRate,
        rampFrames: 0,
      }));
      this.fanoutAt(handoffFrame, () => ({ type: "start", applyAtContextFrame: handoffFrame, sourceFrame: landingFrame }));
      this.invalidateSeams();
      this.timeline.endInertia(handoff, gs.musicalRate);
      this.transportPhase = "playing";
    } else {
      this.fanoutAt(handoffFrame, () => ({ type: "setDirection", applyAtContextFrame: handoffFrame, direction: 1 }));
      this.fanoutAt(handoffFrame, () => ({
        type: "setRate",
        applyAtContextFrame: handoffFrame,
        rate: gs.musicalRate,
        rampFrames: 0,
      }));
      this.fanoutAt(handoffFrame, () => ({ type: "stop", applyAtContextFrame: handoffFrame }));
    }

    // 5. Evidence, per stem.
    this.lastScrubHandoff = {
      keyupContextFrame: Math.round(keyup * sr),
      handoffContextFrame: handoffFrame,
      fadeMs: SCRUB_HANDOFF_FADE_S * 1000,
      stems: this.tracks.map((t, i) => {
        const restart = t.sources.length > 0 ? Math.round((t.sources[t.sources.length - 1]!.startPos ?? landing) * sr) : landingFrame;
        const normal = t.buffer ? (gs.wasPlaying ? t.sources.length : 0) : 0;
        return {
          id: i,
          mode: perTrack[i]?.mode ?? "node",
          landingFrame,
          restartFrame: restart,
          landingErrorFrames: Math.abs(restart - landingFrame),
          queuedGrainsBefore: queuedBefore[i] ?? 0,
          queuedGrainsAfter: 0,
          activeScrubSources: 0,
          activeNormalSources: normal,
          livePlaybackPaths: normal,
        };
      }),
    };

    return {
      ok: true,
      detail: gs.wasPlaying
        ? `shuttle released → handoff frame ${handoffFrame}, landing ${landing.toFixed(3)}s (${moved >= 0 ? "+" : ""}${moved.toFixed(3)}s), ${started} stems resume at ${gs.musicalRate.toFixed(3)}×, ${stillSounding.reduce((a, b) => a + b, 0)} grains faded over ${(SCRUB_HANDOFF_FADE_S * 1000).toFixed(1)} ms [${evidence}]`
        : `shuttle released → parked at ${landing.toFixed(3)}s (handoff frame ${handoffFrame}, ${moved >= 0 ? "+" : ""}${moved.toFixed(3)}s, transport stopped) [${evidence}]`,
    };
  }


  beginHeadScrub(head: number, pointerId: number, normalized: number, timestamp: number): { ok: boolean; detail: string } {
    if (!this.heads.active) return { ok: false, detail: "scrub ignored — heads mode is not active" };
    const cf = Math.max(1, this.heads.cycleFrames);
    const at = this.headFrameNow(head);
    this.scrubTrackers[head] = new ScrubTracker(head, pointerId, this.heads.cycleStartFrame, cf, at, normalized, timestamp);
    this.logScrub("head.scrub.start", head, pointerId, normalized, at);
    if (!this.scrubWorklet(head, "start", pointerId, normalized, 0)) {
      // Node path: silence the looping head voice for the duration of the drag.
      const h = this.heads.heads[head]!;
      this.nodeScrub[head] = { pos: at, level: h.muted ? 0 : h.level, lastGrainAt: this.ctx?.currentTime ?? 0 };
      const v = this.headVoices[head];
      if (v && this.ctx) this.setGain(v.gain.gain, 0);
    }
    return { ok: true, detail: `scrub armed on head ${head + 1} at source frame ${at.toFixed(1)}` };
  }

  previewHeadScrub(head: number, normalized: number, timestamp: number): { ok: boolean; detail: string } {
    const tr = this.scrubTrackers[head];
    if (!tr) return { ok: false, detail: `no scrub gesture open on head ${head + 1}` };
    const prevT = tr.lastTimestamp;
    const p = tr.preview(normalized, timestamp);
    this.logScrub("head.scrub.preview", head, tr.pointerId, normalized, p.targetSourceFrame);
    if (!this.scrubWorklet(head, "preview", tr.pointerId, normalized, p.deltaFrames)) {
      this.scrubNodeGrain(head, p.deltaFrames, Math.max(1e-4, (timestamp - prevT) / 1000));
    }
    return {
      ok: true,
      detail: `head ${head + 1} scrub ${p.direction >= 0 ? "+" : "−"}${Math.abs(p.deltaFrames).toFixed(0)} frames @ ${(p.velocityFramesPerSecond / 1000).toFixed(1)} kframes/s`,
    };
  }

  endHeadScrub(head: number, normalized: number): { ok: boolean; detail: string } {
    const tr = this.scrubTrackers[head];
    if (!tr) return { ok: false, detail: `no scrub gesture open on head ${head + 1}` };
    this.scrubTrackers[head] = null;
    const final = tr.finalFrame(normalized);
    this.logScrub("head.scrub.end", head, tr.pointerId, normalized, final);
    this.heads = scrubHead(this.heads, head, normalized);
    if (!this.scrubWorklet(head, "end", tr.pointerId, normalized, 0)) {
      this.nodeScrub[head] = null;
      // Resume normal playback from exactly where the scrub landed.
      this.restartHeadVoice(head);
    }
    return { ok: true, detail: `head ${head + 1} landed on source frame ${final.toFixed(3)} after ${tr.previewCount} previews` };
  }

  cancelHeadScrub(head: number): { ok: boolean; detail: string } {
    const tr = this.scrubTrackers[head];
    if (!tr) return { ok: false, detail: `no scrub gesture open on head ${head + 1}` };
    this.scrubTrackers[head] = null;
    this.logScrub("head.scrub.cancel", head, tr.pointerId, tr.lastNormalized, tr.startSourceFrame);
    if (!this.scrubWorklet(head, "cancel", tr.pointerId, tr.lastNormalized, 0)) {
      this.nodeScrub[head] = null;
      this.restartHeadVoice(head);
    }
    return { ok: true, detail: `head ${head + 1} scrub cancelled — pre-gesture position restored` };
  }

  /** Every open scrub is closed safely (heads exit, source relink, teardown). */
  cancelAllScrubs() {
    for (let i = 0; i < 4; i++) if (this.scrubTrackers[i]) this.cancelHeadScrub(i);
  }

  /** Live head level from the continuous bus (HEADS without FUNCTION). */
  applyHeadLevel(head: number, level: number): void {
    if (!this.heads.active) return;
    this.heads = setHeadLevel(this.heads, head, level);
    const st = this.nodeScrub[head];
    if (st) st.level = level;
    this.pushHeads();
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
    const srcIdx = this.heads.source ?? 0;
    const sr = this.tracks[srcIdx]?.buffer?.sampleRate ?? this.ctx.sampleRate;
    const buffer = this.ctx.createBuffer(rendered.length, frames, sr);
    for (let c = 0; c < rendered.length; c++) buffer.copyToChannel(new Float32Array(rendered[c]!), c);

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


  /** Abort any in-flight wind ramp so a reversal rebases from the real rate. */
  private cancelWind() {
    if (this.windTimer !== null) {
      clearTimeout(this.windTimer);
      this.windTimer = null;
    }
  }

  /**
   * Workstream 2: schedule one finite tape-inertia ramp across the node
   * sources, the worklet kernels and the integrated timeline, so the audible
   * rate and the reported playhead never disagree.
   */
  private beginWind(kind: "windUp" | "windDown", startAt: number) {
    if (!this.ctx || !this.inertiaEnabled) return null;
    // Correction 1: the MUSICAL target is never read back off the transport.
    // A wind-down parks the instantaneous rate at ~0; the musical rate is still
    // 1.00×, and that is what a wind-up must aim at.
    const musical = this.timeline.musicalRate();
    const stoppedish = this.transportPhase === "stopped" || this.transportPhase === "cued";
    const current = stoppedish
      ? INERTIA_MIN_RATE
      : Math.max(INERTIA_MIN_RATE, this.timeline.currentRate(startAt));
    const seg = makeInertiaSegment({
      startAt,
      currentRate: kind === "windUp" ? current : Math.max(INERTIA_MIN_RATE, this.timeline.currentRate(startAt)),
      targetRate: kind === "windUp" ? musical : INERTIA_MIN_RATE,
      preset: INERTIA_PRESETS[this.inertiaPreset],
      kind,
    });
    if (seg.durationS <= 0.011) return null;
    this.transportPhase = kind === "windUp" ? "windingUp" : "windingDown";
    this.timeline.startInertia(startAt, seg);
    const curve = inertiaCurve(seg);
    for (const t of this.tracks)
      for (const s of t.sources) {
        s.node.playbackRate.cancelScheduledValues(startAt);
        s.node.playbackRate.setValueCurveAtTime(curve, startAt, seg.durationS);
      }
    this.fanout((_t, at) => ({
      type: "inertia",
      from: seg.from,
      to: seg.to,
      k: seg.k,
      durationFrames: Math.round(seg.durationS * this.ctx!.sampleRate),
      applyAtContextFrame: at,
    }));
    this.timelineBus.emit({
      type: "RateChange",
      rate: seg.to,
      musicalRate: this.timeline.musicalRate(),
      rampFrames: Math.round(seg.durationS * this.ctx.sampleRate),
      cause: kind,
    });
    // Seams derived on the old curve are invalid the moment the rate moves.
    this.invalidateSeams();
    return seg;
  }

  /**
   * Correction 2 — reverse an in-flight wind WITHOUT completing it.
   *
   * The completion path stops sources and would then have to create new ones;
   * doing that mid-gesture is the source-recreation bug. This instead freezes
   * the ramp at `now`, re-anchors the timeline on the instantaneous rate, and
   * leaves every live source running so the opposite curve can be scheduled on
   * top of the very same nodes.
   */
  private interruptAndRebaseWind(now: number): number {
    if (this.windTimer) {
      clearTimeout(this.windTimer);
      this.windTimer = null;
    }
    const instant = Math.max(INERTIA_MIN_RATE, this.timeline.interruptInertia(now));
    if (this.transportGain) {
      this.transportGain.gain.cancelScheduledValues(now);
      this.transportGain.gain.setValueAtTime(1, now);
    }
    for (const t of this.tracks)
      for (const src of t.sources) {
        src.node.playbackRate.cancelScheduledValues(now);
        src.node.playbackRate.setValueAtTime(instant, now);
      }
    this.fanout((_t, at) => ({
      type: "inertia",
      from: instant,
      to: instant,
      k: 1,
      durationFrames: 0,
      applyAtContextFrame: at,
    }));
    this.invalidateSeams();
    this.timelineBus.emit({
      type: "RateChange",
      rate: instant,
      musicalRate: this.timeline.musicalRate(),
      rampFrames: 0,
      cause: "windReversal",
    });
    this.windReversals += 1;
    return instant;
  }

  /**
   * Force-complete any transition. Used ONLY for teardown (project change,
   * dispose) — never for a musical reversal.
   */
  private forceCompleteWind(): void {
    if (this.windTimer) {
      clearTimeout(this.windTimer);
      this.windTimer = null;
    }
    if (this.ctx) this.timeline.endInertia(this.ctx.currentTime, this.timeline.musicalRate());
  }

  /** Evidence counters for the transport proofs. */
  windReversals = 0;
  sourceCreations = 0;

  /** Tape-inertia preset. Rhythmic operations never receive inertia. */
  setInertiaPreset(name: InertiaPresetName | "off"): { ok: boolean; detail: string } {
    if (name === "off") {
      this.inertiaEnabled = false;
      return { ok: true, detail: "tape inertia disabled — instant start/stop" };
    }
    this.inertiaEnabled = true;
    this.inertiaPreset = name;
    const p = INERTIA_PRESETS[name];
    return { ok: true, detail: `inertia ${p.label} — up ${p.startS * 1000} ms / down ${p.stopS * 1000} ms` };
  }

  /** Post-FX, post-transport-envelope RMS — what the speakers actually get. */
  masterRms(): number {
    const a = this.masterAnalyser;
    if (!a) return 0;
    const buf = new Float32Array(a.fftSize);
    a.getFloatTimeDomainData(buf);
    let sum = 0;
    for (let i = 0; i < buf.length; i++) sum += buf[i]! * buf[i]!;
    return Math.sqrt(sum / buf.length);
  }

  transportPhaseNow(): TransportPhase {
    return this.transportPhase;
  }

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
          const now = this.ctx.currentTime;
          if (this.transportPhase === "windingDown" && this.requestedPlaying) {
            // Continuous reversal: the tape never restarts from zero and no
            // source is recreated.
            const instant = this.interruptAndRebaseWind(now);
            const up = this.beginWind("windUp", now);
            if (!up) {
              this.transportPhase = "playing";
              this.timeline.endInertia(now, this.timeline.musicalRate());
              return this.ack(cmd, "completed", `wind-down reversed instantly at ${instant.toFixed(3)}×`);
            }
            this.ack(cmd, "accepted", `wind-down reversed from ${instant.toFixed(3)}× — no source recreated`);
            this.windTimer = setTimeout(() => {
              this.windTimer = null;
              this.transportPhase = "playing";
              if (this.ctx) this.timeline.endInertia(this.ctx.currentTime, this.timeline.musicalRate());
            }, up.durationS * 1000);
            return this.ack(
              cmd,
              "completed",
              `continuous reversal ${up.from.toFixed(3)}× → ${up.to.toFixed(3)}× over ${(up.durationS * 1000).toFixed(0)} ms (sources preserved)`,
            );
          }
          this.cancelWind();
          const cueLaunch = this.transportPhase === "cued";
          const resume = cueLaunch ? 0 : this.position() >= this.duration ? 0 : this.position();
          this.stopSources();
          this.requestedPlaying = true;
          // The transport envelope always opens instantly on Play; the tape
          // sound comes from the rate ramp, not from a volume fade-in.
          this.transportGain?.gain.cancelScheduledValues(now);
          this.transportGain?.gain.setValueAtTime(1, now);
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
          // Correction/Decision: cue launches EXACT — predictable live sync.
          const wind = cueLaunch ? null : this.beginWind("windUp", startAt);
          if (!wind) {
            if (cueLaunch) {
              this.timeline.endInertia(startAt, this.timeline.musicalRate());
              this.transportGain?.gain.setValueAtTime(0, startAt);
              this.transportGain?.gain.linearRampToValueAtTime(1, startAt + CUE_FADE_S);
              this.transportPhase = "playing";
              return this.ack(
                cmd,
                "completed",
                `EXACT cue launch — ${started} stems from frame 0 at t=${startAt.toFixed(4)}s, ${(CUE_FADE_S * 1000).toFixed(0)} ms anti-click open`,
              );
            }
            this.transportPhase = "playing";
            return this.ack(cmd, "completed", `${started} stems scheduled at t=${startAt.toFixed(4)}s (spread 0.000 ms)`);
          }
          this.ack(cmd, "accepted", `Play accepted — winding up over ${(wind.durationS * 1000).toFixed(0)} ms`);
          this.windTimer = setTimeout(() => {
            this.windTimer = null;
            this.transportPhase = "playing";
            if (this.ctx) this.timeline.endInertia(this.ctx.currentTime, this.timeline.musicalRate());
          }, wind.durationS * 1000);
          return this.ack(
            cmd,
            "completed",
            `${started} stems scheduled at t=${startAt.toFixed(4)}s · wind-up ${wind.from.toFixed(3)}× → ${wind.to.toFixed(3)}× over ${(wind.durationS * 1000).toFixed(0)} ms`,
          );
        }
        case "transport.stop": {
          if (!this.ctx) return this.ack(cmd, "rejected", "audio not unlocked");
          const now = this.ctx.currentTime;
          this.cancelWind();
          const wind = this.requestedPlaying ? this.beginWind("windDown", now) : null;
          if (!wind) {
            const pos = this.position();
            this.requestedPlaying = false;
            this.transportPhase = "stopped";
            this.timelineFrozenAt = now;
            this.timeline.anchor(now, pos);
            this.stopSources();
            this.fanout((_t, at) => ({ type: "stop", applyAtContextFrame: at }));
            return this.ack(cmd, "completed", `stopped at ${pos.toFixed(3)}s`);
          }
          const endAt = now + wind.durationS;
          // Click-free: the dedicated transport envelope closes only after the
          // tape has already slowed to a standstill.
          this.transportGain?.gain.cancelScheduledValues(now);
          this.transportGain?.gain.setValueAtTime(1, endAt);
          this.transportGain?.gain.linearRampToValueAtTime(0, endAt + INERTIA_STOP_FADE_S);
          this.ack(cmd, "accepted", `Stop accepted — winding down over ${(wind.durationS * 1000).toFixed(0)} ms`);
          this.windTimer = setTimeout(
            () => {
              this.windTimer = null;
              if (!this.ctx) return;
              const t = this.ctx.currentTime;
              const pos = this.position();
              this.requestedPlaying = false;
              this.transportPhase = "stopped";
              this.timeline.endInertia(t, wind.to);
              this.timelineFrozenAt = t;
              this.timeline.anchor(t, pos);
              this.stopSources();
              this.fanout((_t2, at) => ({ type: "stop", applyAtContextFrame: at }));
              this.transportGain?.gain.setValueAtTime(1, t + 0.001);
            },
            (wind.durationS + INERTIA_STOP_FADE_S) * 1000,
          );
          return this.ack(
            cmd,
            "completed",
            `wind-down ${wind.from.toFixed(3)}× → 0× over ${(wind.durationS * 1000).toFixed(0)} ms, then transport envelope closed in ${(INERTIA_STOP_FADE_S * 1000).toFixed(0)} ms`,
          );
        }

        case "transport.cue": {
          if (!this.ctx) return this.ack(cmd, "rejected", "audio not unlocked");
          const now = this.ctx.currentTime;
          // Correction 3: cue is a UTILITY action — it must not sit through a
          // 450 ms wind-down. Interrupt inertia, close an 8 ms anti-click fade,
          // stop the sources and park every stem on frame zero.
          if (this.transportPhase === "windingUp" || this.transportPhase === "windingDown") {
            this.interruptAndRebaseWind(now);
          }
          this.cancelWind();
          if (this.transportGain) {
            this.transportGain.gain.cancelScheduledValues(now);
            this.transportGain.gain.setValueAtTime(this.transportGain.gain.value, now);
            this.transportGain.gain.linearRampToValueAtTime(0, now + CUE_FADE_S);
          }
          const fadeEnd = now + CUE_FADE_S;
          this.requestedPlaying = false;
          this.transportPhase = "cued";
          // v1 feature freeze: the cue point is frame zero, runtime only.
          this.timeline.endInertia(now, this.timeline.musicalRate());
          this.timeline.anchor(fadeEnd, 0);
          this.timelineFrozenAt = fadeEnd;
          this.stopSources();
          this.fanout((_t, at) => ({ type: "stop", applyAtContextFrame: at }));
          this.transportGain?.gain.setValueAtTime(1, fadeEnd + 0.001);
          return this.ack(
            cmd,
            "completed",
            `cued at frame 0 after a ${(CUE_FADE_S * 1000).toFixed(0)} ms transport fade — musical rate held at ${this.timeline.musicalRate().toFixed(3)}×`,
          );
        }
        case "transport.scrub.start": {
          const dir: 1 | -1 = Number(p["direction"] ?? 1) < 0 ? -1 : 1;
          const r = this.beginGlobalScrub(dir);
          return this.ack(cmd, r.ok ? "accepted" : "rejected", r.detail);
        }
        case "transport.scrub.end": {
          const r = this.endGlobalScrub();
          return this.ack(cmd, r.ok ? "completed" : "rejected", r.detail);
        }
        case "transport.scrub": {
          if (!this.ctx) return this.ack(cmd, "rejected", "audio not unlocked");
          if (this.duration === 0) return this.ack(cmd, "rejected", "no stems decoded");
          const now = this.ctx.currentTime;
          const delta = Number(p["seconds"] ?? 0);
          const target = Math.min(this.duration, Math.max(0, this.position() + delta));
          const wasPlaying = this.requestedPlaying;
          this.stopSources();
          this.timeline.anchor(now, target);
          if (wasPlaying) {
            const { started } = this.startAll(target);
            this.invalidateSeams();
            return this.ack(
              cmd,
              "completed",
              `global scrub ${delta >= 0 ? "+" : ""}${delta.toFixed(3)}s → ${target.toFixed(3)}s across ${started} stems (one shared playhead)`,
            );
          }
          this.timelineFrozenAt = now;
          return this.ack(cmd, "completed", `global scrub → ${target.toFixed(3)}s (transport stopped, all four stems parked together)`);
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
              this.timelineBus.emit({ type: "GlideChange", from, to: rate, tau: glide, cause: "rate.set" });
              this.timelineBus.emit({
                type: "RateChange",
                rate,
                musicalRate: rate,
                rampFrames: Math.round(glideDurationS(glide) * (this.ctx?.sampleRate ?? 48000)),
                cause: "rate.set glide",
              });
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
              this.timelineBus.emit({ type: "RateChange", rate, musicalRate: rate, rampFrames: 0, cause: "rate.set" });
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
          {
            const b = t.buffer ? resolveLoop(t.loop, t.buffer.duration) : null;
            this.timelineBus.emit({
              type: "WindowChange",
              track: id,
              startS: b?.start ?? 0,
              lengthS: b ? b.end - b.start : 0,
            });
          }
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
          this.timelineBus.emit({ type: "ChopChange", track: id, div });
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
          this.timelineBus.emit({ type: "LinkChange", mask: this.tracks.map((x) => (x.linked ? "1" : "0")).join("") });
          return this.ack(cmd, "completed", `stem ${id + 1} ${p["on"] ? "linked" : "unlinked"} — phase-continuous, no restart`);
        }
        case "stem.select":
          return this.ack(cmd, "completed", `active stem → ${Number(p["stem"]) + 1}`);
        case "fx.overlay":
          return this.ack(cmd, "completed", `FX overlay ${p["on"] ? "open" : "closed"} — tape audio unaffected`);
        case "fx.bank.select":
          return this.ack(cmd, "completed", `stem ${Number(p["track"]) + 1} bank ${Number(p["bank"]) + 1} selected — nothing sounded yet`);
        case "fx.momentary.start":
        case "fx.momentary.end":
        case "fx.latch":
        case "fx.algorithm.cycle":
        case "fx.macro":
        case "fx.variation":
        case "fx.clearLatches": {
          const id = Number(p["track"]) as TrackId;
          const t = this.tracks[id];
          if (!t) return this.ack(cmd, "rejected", `no track ${id}`);
          if (!this.ctx) return this.ack(cmd, "rejected", "audio not unlocked");
          if (cmd.type === "fx.clearLatches") {
            const res = this.clearBanks(id);
            return this.ack(cmd, res.ok ? "completed" : "rejected", res.detail);
          }
          const latched = Boolean(p["latched"]);
          // Workstream 3 commands carry a bank + algorithm; the legacy
          // fx.variation command still carries a family.
          if (cmd.type === "fx.variation") {
            const family = String(p["family"]) as FxFamily;
            if (!FX_FAMILIES.includes(family)) return this.ack(cmd, "rejected", `unknown FX family ${String(p["family"])}`);
            void this.setFxVariation(id, family, Number(p["variation"]), latched);
            return this.ack(cmd, "completed", `stem ${id + 1} ${family} variation → ${Number(p["variation"])}`);
          }
          const bank = Number(p["bank"]) as BankIndex;
          if (!(bank >= 0 && bank <= 3)) return this.ack(cmd, "rejected", `unknown FX bank ${String(p["bank"])}`);
          const algorithm = Number(p["algorithm"] ?? 0) as AlgorithmIndex;
          if (cmd.type === "fx.algorithm.cycle") {
            const res = this.selectBankAlgorithm(id, bank, algorithm);
            return this.ack(cmd, res.ok ? "completed" : "rejected", res.detail);
          }
          if (cmd.type === "fx.macro") {
            const res = this.setBankMacro(id, bank, algorithm, Number(p["value"]));
            return this.ack(cmd, res.ok ? "completed" : "rejected", res.detail);
          }
          const active = cmd.type === "fx.latch" ? Boolean(p["on"]) : cmd.type === "fx.momentary.start";
          // Accepted immediately (zero hold latency), completed once the wet
          // ramp is scheduled. Distinct acknowledgements, never merged.
          const accepted = this.ack(cmd, "accepted", `stem ${id + 1} bank ${bank + 1} ${active ? "engaging" : "releasing"}`);
          void this.setBankActive(id, bank, algorithm, active, latched).then((res) =>
            this.ack(cmd, res.ok ? "completed" : "rejected", res.detail),
          );
          return accepted;

        }

        // ---- Phase 6: recording, grid, heads/PRINT -------------------------
        case "rec.requestInput": {
          const id = Number(p["track"]) as TrackId;
          const rec = this.recording();
          if (!rec) return this.ack(cmd, "rejected", "audio not unlocked");
          if (rec.model.inputEnabled) {
            rec.arm(id);
            this.pendingInputTrack = null;
            return this.ack(cmd, "completed", rec.model.lastAck);
          }
          // §2.2: the hold is remembered, not lost. The UI opens the grant
          // drawer; the track arms itself the moment permission lands.
          this.pendingInputTrack = id;
          return this.ack(cmd, "accepted", `track ${id + 1} is waiting for input — allow the microphone and it arms itself, nothing else changed`);
        }
        case "rec.cancelInput": {
          const had = this.pendingInputTrack;
          this.pendingInputTrack = null;
          return this.ack(cmd, "completed", had == null ? "no pending input request" : `pending input request for track ${had + 1} cancelled — track untouched`);
        }
        case "rec.recover": {
          const rec = this.recording();
          if (!rec) return this.ack(cmd, "rejected", "audio not unlocked");
          const recoverable = rec.takes.filter((t) => t.state === "ready" && t.durable);
          if (recoverable.length === 0)
            return this.ack(cmd, "rejected", "nothing recoverable — interrupted takes stay interrupted rather than being played back short");
          void Promise.all(recoverable.map((t) => rec.activateTake(t)));
          return this.ack(cmd, "accepted", `re-activating ${recoverable.length} durable take(s)`);
        }

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
        case "heads.enter": {
          const r = this.enterHeadsMode();
          return this.ack(cmd, r.ok ? "completed" : "rejected", r.detail);
        }
        case "heads.exit": {
          const r = this.exitHeadsMode();
          return this.ack(cmd, r.ok ? "completed" : "rejected", r.detail);
        }
        case "heads.source": {
          const r = this.setHeadsSource(Number(p["track"]) as TrackId);
          return this.ack(cmd, r.ok ? "completed" : "rejected", r.detail);
        }
        case "heads.level": {
          if (!this.heads.active) return this.ack(cmd, "rejected", "heads mode is not active");
          const i = Number(p["head"]);
          this.heads = setHeadLevel(this.heads, i, Number(p["level"]));
          this.pushHeads();
          return this.ack(cmd, "completed", `head ${i + 1} level → ${this.heads.heads[i]!.level.toFixed(3)}`);
        }
        case "heads.mute": {
          if (!this.heads.active) return this.ack(cmd, "rejected", "heads mode is not active");
          const i = Number(p["head"]);
          this.heads = toggleHeadMute(this.heads, i);
          this.pushHeads();
          return this.ack(cmd, "completed", `head ${i + 1} ${this.heads.heads[i]!.muted ? "muted" : "unmuted"} — still a head, geometry unchanged`);
        }
        case "heads.reverse": {
          if (!this.heads.active) return this.ack(cmd, "rejected", "heads mode is not active");
          const i = Number(p["head"]);
          this.heads = toggleHeadReverse(this.heads, i);
          this.pushHeads();
          this.restartHeadVoice(i);
          return this.ack(cmd, "completed", `head ${i + 1} → ${this.heads.heads[i]!.reverse ? "reverse" : "forward"} (crossfaded, no reversed PCM copy on the worklet path)`);
        }
        case "heads.scrub": {
          if (!this.heads.active) return this.ack(cmd, "rejected", "heads mode is not active");
          const i = Number(p["head"]);
          const pos = Number(p["position"]);
          const already = Math.abs((this.heads.heads[i]?.offset ?? -1) - ((pos % 1) + 1) % 1) < 1e-9;
          this.heads = scrubHead(this.heads, i, pos);
          if (already) {
            // The audible gesture already landed this head; re-seaming the
            // voice here would add a second, inaudible-but-real restart.
            return this.ack(cmd, "completed", `head ${i + 1} landing confirmed at ${(this.heads.heads[i]!.offset * 100).toFixed(1)}% (audible scrub already applied)`);
          }
          this.pushHeads();
          this.restartHeadVoice(i);
          return this.ack(cmd, "completed", `head ${i + 1} scrubbed to ${(this.heads.heads[i]!.offset * 100).toFixed(1)}% of the cycle`);
        }

        case "heads.print": {
          const target = Number(p["track"]) as TrackId;
          if (!this.heads.active) return this.ack(cmd, "rejected", "PRINT requires heads mode to be active");
          void this.printHeads(target).then((r) =>
            this.ack({ id: cmd.id, type: cmd.type }, r.ok ? "completed" : "failed", r.detail),
          );
          return this.ack(cmd, "accepted", `PRINT accepted — rendering one heads cycle into track ${target + 1}`);
        }
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
      targetRate: this.timeline.musicalRate(),
      transportPhase: this.transportPhase,
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
      heads: this.heads,
      headsSummary: headsSummary(this.heads),
      scrub: {
        open: this.scrubTrackers.map((t, i) =>
          t ? { head: i, pointerId: t.pointerId, previews: t.previewCount, lastVelocity: t.lastVelocity } : null,
        ),
        telemetry: this.scrubTelemetry,
        events: this.scrubLog.events.slice(0, 12),
      },

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
      t.bankRack?.dispose();
      t.bankRack = null;
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
