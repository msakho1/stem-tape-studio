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
import { allowed, defaultBudget, describeVerdict, judge, SSR_BUDGET, type MemoryBudget } from "./memory";
import { DEFAULT_WINDOW, resolveLoop, TapeTimeline, type LoopWindow } from "./tape";

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
  gain: GainNode;
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
  }[];
  decodedBytes: number;
  reverseBytes: number;
  budget: MemoryBudget;
  highMemoryMode: boolean;
  memoryStatement: string;
  lastError: string | null;
  lastDecodeMs: number | null;
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

      input.connect(dry);
      input.connect(filter);
      filter.connect(wet);
      dry.connect(gain);
      wet.connect(gain);
      gain.connect(analyser);
      analyser.connect(this.master!);
      gain.gain.value = [0.78, 0.72, 0.65, 0.7][i] ?? 0.7;

      return {
        buffer: null,
        reversed: null,
        trash: null,
        input,
        gain,
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
      } satisfies TrackRuntime;
    });
  }

  get ready() {
    return this.ctx != null && this.ctx.state === "running";
  }

  get decodedTotalBytes() {
    return this.tracks.reduce(
      (sum, t) => sum + (t.buffer ? bufferBytes(t.buffer) : 0) + (t.reversed ? bufferBytes(t.reversed) : 0),
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
    return this.tracks.map((t) => (t.buffer ? bufferBytes(t.buffer) : 0));
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
    return this.tracks.reduce((max, t) => Math.max(max, t.buffer?.duration ?? 0), 0);
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
    this.setGain(t.gain.gain, t.muted ? 0 : level);
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
    t.dry.gain.cancelScheduledValues(now);
    t.wet.gain.cancelScheduledValues(now);
    t.dry.gain.setValueCurveAtTime(sampleCurve(curve, "a"), now, FILTER_FADE_S);
    t.wet.gain.setValueCurveAtTime(sampleCurve(curve, "b"), now, FILTER_FADE_S);
    t.filterMode = mode;
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
          if (started === 0) {
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
          return this.ack(cmd, "completed", `stopped at ${pos.toFixed(3)}s`);
        }
        case "transport.restart": {
          if (!this.ctx) return this.ack(cmd, "rejected", "audio not unlocked");
          if (this.duration === 0) return this.ack(cmd, "rejected", "no stems decoded");
          this.stopSources();
          this.requestedPlaying = true;
          const { started, startAt } = this.startAll(0);
          return started > 0
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
          if (this.ctx) this.setGain(t.gain.gain, t.muted ? 0 : t.level);
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
              if (this.ctx) this.setGain(t.gain.gain, t.muted ? 0 : t.level);
            });
          }
          return this.ack(
            cmd,
            "completed",
            `rolled back ${String(p["control"])} ×${String(p["toCount"])}${mask ? ` · mutes=${mask}` : ""}`,
          );
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
        };
      }),
      decodedBytes: this.decodedTotalBytes,
      reverseBytes: this.reverseTotalBytes,
      budget: this.budget,
      highMemoryMode: this.highMemoryMode,
      memoryStatement: describeVerdict(this.decodedTotalBytes, this.budget, this.highMemoryMode),
      lastError: this.lastError,
      lastDecodeMs: this.lastDecodeMs,
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
