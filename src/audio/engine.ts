/**
 * AudioEngine — the single authority over what is actually audible.
 *
 * Rules enforced here:
 *  - exactly ONE AudioContext for the whole app, created inside a user gesture;
 *  - persistent per-track graph (gain → filter → analyser → master bus); only
 *    the one-shot AudioBufferSourceNodes are recreated, tracked by generation
 *    so a stale `onended` can never mutate live state;
 *  - all four sources always receive the IDENTICAL scheduled `startAt` and the
 *    identical offset — never four sequential "play now" calls;
 *  - the playhead is DERIVED from ctx.currentTime + an anchor, never ticked;
 *  - every gain change is ramped (setTargetAtTime) so nothing clicks;
 *  - every command answers with an ack, and a rejected command must not be
 *    allowed to light an LED.
 */

import type { Ack, AudioCommand } from "./commands";
import { bufferBytes } from "./format";
import { allowed, defaultBudget, describeVerdict, judge, SSR_BUDGET, type MemoryBudget } from "./memory";


export const LOOKAHEAD_S = 0.08;
export const RAMP_TAU = 0.008;

export type TrackId = 0 | 1 | 2 | 3;

export interface TrackRuntime {
  buffer: AudioBuffer | null;
  /** Deleted buffers go here, recoverable until the project is compacted. */
  trash: AudioBuffer | null;
  gain: GainNode;
  filter: BiquadFilterNode;
  analyser: AnalyserNode;
  source: AudioBufferSourceNode | null;
  generation: number;
  muted: boolean;
  level: number;
  /** Exact context time this track's current source was scheduled to start. */
  scheduledStartAt: number | null;
  /** Where the bytes came from — user audio is never network-fetched. */
  provenance: "user-private" | "bundled-demo" | null;
  name: string | null;
  /** Decode instrumentation (Phase 4.1): must be exactly 1 per load. */
  decodeCount: number;
  decodeMs: number | null;
  bufferReused: boolean;
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
    /** Exact retained bytes of this track's decoded buffer. */
    decodedBytes: number;
    /** How many times this track's audio has been decoded this session. */
    decodeCount: number;
    decodeMs: number | null;
    /** true when the AudioBuffer produced by the probe was adopted as-is. */
    bufferReused: boolean;
  }[];
  decodedBytes: number;
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
  private anchorCtxTime = 0;
  private anchorPos = 0;
  private rate = 1;
  private requestedPlaying = false;
  private masterLevel = 0.7;
  private listeners = new Set<Listener>();
  budget: MemoryBudget = SSR_BUDGET;
  /** Explicit opt-in; only meaningful above the standard threshold. */
  highMemoryMode = false;
  lastError: string | null = null;
  lastDecodeMs: number | null = null;

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

  private buildGraph() {
    const ctx = this.ctx!;
    this.master = ctx.createGain();
    this.master.gain.value = this.masterLevel;
    this.masterAnalyser = ctx.createAnalyser();
    this.masterAnalyser.fftSize = 1024;
    this.master.connect(this.masterAnalyser);
    this.masterAnalyser.connect(ctx.destination);

    this.tracks = [0, 1, 2, 3].map((i) => {
      const gain = ctx.createGain();
      const filter = ctx.createBiquadFilter();
      const analyser = ctx.createAnalyser();
      analyser.fftSize = 256;
      filter.type = "allpass";
      gain.connect(filter);
      filter.connect(analyser);
      analyser.connect(this.master!);
      gain.gain.value = [0.78, 0.72, 0.65, 0.7][i] ?? 0.7;
      return {
        buffer: null,
        trash: null,
        gain,
        filter,
        analyser,
        source: null,
        generation: 0,
        muted: false,
        level: gain.gain.value,
        scheduledStartAt: null,
        provenance: null,
        name: null,
        decodeCount: 0,
        decodeMs: null,
        bufferReused: false,
      } satisfies TrackRuntime;
    });
  }

  get ready() {
    return this.ctx != null && this.ctx.state === "running";
  }

  get decodedTotalBytes() {
    return this.tracks.reduce((sum, t) => sum + (t.buffer ? bufferBytes(t.buffer) : 0), 0);
  }

  /** Read-only access to a decoded buffer (Memory Saver derives from it). */
  getBuffer(id: TrackId): AudioBuffer | null {
    return this.tracks[id]?.buffer ?? null;
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

  /**
   * Stage 1 of the two-stage gate: decide from a pre-decode ESTIMATE, before an
   * AudioBuffer is ever allocated.
   */
  preDecodeGate(id: TrackId, estimateBytes: number): { ok: boolean; detail: string } {
    const projected = this.projectedBytes(id, estimateBytes);
    const verdict = judge(projected, this.budget, this.highMemoryMode);
    return { ok: allowed(verdict), detail: describeVerdict(projected, this.budget, this.highMemoryMode) };
  }

  /**
   * Stage 2 of the two-stage gate: the exact post-decode verdict, applied to the
   * ONE buffer the probe already produced. Accepted → adopted as-is (no second
   * decode). Rejected → dereferenced here and never installed.
   */
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
   * is probe → adoptBuffer, which decodes exactly once. Taking this path after
   * a successful probe pushes decodeCount to 2, which the acceptance test
   * asserts never happens.
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

  /** Explicit dereference: unloads a track's PCM and its trash copy. */
  releaseTrack(id: TrackId): number {
    const t = this.tracks[id];
    if (!t) return 0;
    const freed = (t.buffer ? bufferBytes(t.buffer) : 0) + (t.trash ? bufferBytes(t.trash) : 0);
    t.buffer = null;
    t.trash = null;
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
    if (!this.ctx) return this.anchorPos;
    if (!this.requestedPlaying) return this.anchorPos;
    const elapsed = this.ctx.currentTime - this.anchorCtxTime;
    if (elapsed <= 0) return this.anchorPos;
    return Math.min(this.duration, this.anchorPos + elapsed * this.rate);
  }

  private stopSources() {
    for (const t of this.tracks) {
      if (t.source) {
        try {
          t.source.onended = null;
          t.source.stop();
        } catch {
          /* already stopped */
        }
        t.source.disconnect();
        t.source = null;
        t.scheduledStartAt = null;
      }
    }
  }

  /** All four stems start at ONE shared, scheduled context time. */
  private startAll(offset: number): { started: number; startAt: number } {
    const ctx = this.ctx!;
    const startAt = ctx.currentTime + LOOKAHEAD_S;
    let started = 0;
    for (const t of this.tracks) {
      if (!t.buffer) continue;
      const src = ctx.createBufferSource();
      src.buffer = t.buffer;
      src.playbackRate.value = this.rate;
      src.connect(t.gain);
      const gen = ++t.generation;
      src.onended = () => {
        if (t.generation !== gen) return; // stale node, ignore
        t.source = null;
      };
      const off = Math.min(offset, Math.max(0, t.buffer.duration - 0.0001));
      src.start(startAt, off);
      t.source = src;
      t.scheduledStartAt = startAt;
      started++;
    }
    this.anchorCtxTime = startAt;
    this.anchorPos = offset;
    return { started, startAt };
  }

  /** Largest difference between the scheduled start times of live sources. */
  startSpreadMs(): number {
    const times = this.tracks.map((t) => t.scheduledStartAt).filter((v): v is number => v != null);
    if (times.length < 2) return 0;
    return (Math.max(...times) - Math.min(...times)) * 1000;
  }

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

  /** Executes one ordered command and answers with an ack. */
  execute(cmd: AudioCommand): Ack {
    const p = cmd.payload;
    // Before unlock there is no graph at all: say so instead of blaming the track.
    if (!this.ctx && cmd.type.startsWith("track.")) {
      return this.ack(cmd, "rejected", "audio not unlocked — enable audio, then repeat the gesture");
    }
    try {
      switch (cmd.type) {
        case "transport.play": {
          if (!this.ctx) return this.ack(cmd, "rejected", "audio not unlocked — press Play again after enabling audio");
          if (this.ctx.state !== "running") return this.ack(cmd, "rejected", `AudioContext is ${this.ctx.state}`);
          if (this.duration === 0) return this.ack(cmd, "rejected", "no stems decoded");
          this.stopSources();
          this.requestedPlaying = true;
          const { started, startAt } = this.startAll(this.anchorPos >= this.duration ? 0 : this.anchorPos);
          if (started === 0) {
            this.requestedPlaying = false;
            return this.ack(cmd, "failed", "no source could be created");
          }
          return this.ack(cmd, "completed", `${started} stems scheduled at t=${startAt.toFixed(4)}s (spread 0.000 ms)`);
        }
        case "transport.stop": {
          if (!this.ctx) return this.ack(cmd, "rejected", "audio not unlocked");
          this.anchorPos = this.position();
          this.requestedPlaying = false;
          this.stopSources();
          return this.ack(cmd, "completed", `stopped at ${this.anchorPos.toFixed(3)}s`);
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
          if (t.source) {
            try {
              t.source.onended = null;
              t.source.stop();
            } catch {
              /* noop */
            }
            t.source.disconnect();
            t.source = null;
            t.scheduledStartAt = null;
          }
          t.trash = t.buffer;
          t.buffer = null;
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
          if (!Number.isFinite(rate) || rate <= 0) return this.ack(cmd, "rejected", `invalid rate ${String(p["rate"])}`);
          if (this.ctx && this.requestedPlaying) {
            // Re-anchor BEFORE changing rate so the derived playhead stays exact.
            this.anchorPos = this.position();
            this.anchorCtxTime = this.ctx.currentTime;
          }
          this.rate = rate;
          if (this.ctx) {
            for (const t of this.tracks) t.source?.playbackRate.setValueAtTime(rate, this.ctx.currentTime);
          }
          return this.ack(cmd, "completed", `playback rate → ${rate.toFixed(4)}×`);
        }
        case "song.load": {
          // P4: stop transport, load, wait for an explicit Play. No crossfade.
          if (this.ctx) {
            this.stopSources();
            this.requestedPlaying = false;
            this.anchorPos = 0;
          }
          return this.ack(cmd, "completed", `song ${Number(p["song"]) + 1} armed — transport stopped, waiting for Play`);
        }
        case "rollback": {
          // Revoke an optimistic multi-tap action. Absolute commands that follow
          // re-assert the truth, so the engine only has to drop the optimistic
          // transport/rate side effects here.
          const rate = Number(p["rate"]);
          if (Number.isFinite(rate) && rate > 0) {
            this.rate = rate;
            if (this.ctx) for (const t of this.tracks) t.source?.playbackRate.setValueAtTime(rate, this.ctx.currentTime);
          }
          // Re-assert the pre-tap mute mask: an optimistic ×1 mute must not
          // survive the upgrade to ×2/×3.
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
    return {
      contextState: this.ctx?.state ?? "none",
      sampleRate: this.ctx?.sampleRate ?? null,
      currentTime: this.ctx?.currentTime ?? 0,
      requestedPlaying: this.requestedPlaying,
      actuallyPlaying: this.requestedPlaying && this.ctx?.state === "running" && this.tracks.some((t) => t.source != null),
      position: this.position(),
      duration: this.duration,
      rate: this.rate,
      masterGain: this.masterLevel,
      startSpreadMs: this.startSpreadMs(),
      tracks: this.tracks.map((t, i) => ({
        id: i,
        decoded: t.buffer != null,
        sourceLive: t.source != null,
        generation: t.generation,
        gain: t.muted ? 0 : t.level,
        muted: t.muted,
        scheduledStartAt: t.scheduledStartAt,
        name: t.name,
        provenance: t.provenance,
        trashed: t.trash != null,
        decodedBytes: t.buffer ? bufferBytes(t.buffer) : 0,
        decodeCount: t.decodeCount,
        decodeMs: t.decodeMs,
        bufferReused: t.bufferReused,
      })),
      decodedBytes: this.decodedTotalBytes,
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
      this.anchorPos = this.position();
      this.requestedPlaying = false;
      this.stopSources();
      this.lastError = `AudioContext became ${this.ctx.state} — transport stopped, position held at ${this.anchorPos.toFixed(2)}s`;
    }
  }

  dispose() {
    this.stopSources();
    void this.ctx?.close();
    this.ctx = null;
  }
}

let singleton: AudioEngine | null = null;

/** One context per app, never one per track. */
export function getAudioEngine(): AudioEngine {
  if (!singleton) singleton = new AudioEngine();
  return singleton;
}
