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
import { complementary, equalPower, measuredDryWet, sampleCurve, FILTER_FADE_S, SEAM_FADE_S } from "./crossfade";
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
import { midiClock } from "./midi/clock";
import type { MidiTransport, StemMidiEvent } from "./midi/contract";
import {
  CueStore,
  describeCueAction,
  type CueAction,
  type CueEventContext,
  type CueLane,
  type CueMarker,
  type EligibilitySnapshot,
  type QualifierSnapshot,
} from "./cues";

import { algorithmDef } from "@/machine/fx12";
import { FX_FAMILIES, type FxFamily } from "@/machine/stemPerformance";

import { TapeTimelineBus, type TapeTimelineEvent } from "./timelineEvents";
import { PerformanceRecorder } from "./export/performanceRecorder";
import { emptyGrid, tapGrid, type GridState } from "./grid";
import {
  analyzeSongGrid,
  barStartAt,
  describeGrid,
  nextBarAfter,
  type AnalysisInput,
  type SongGrid,
} from "./gridAnalysis";
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
import { HeadLanes, type HeadLaneSnapshot } from "./headLanes";
import {
  HEAD_SCRUB_MAX_RATE,
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
/** Single-lane shuttle rate. Slower than the global shuttle: it is an edit
 *  gesture on one stem, not a transport move. */
const LANE_SCRUB_RATE = 1.5;
/** Ceiling for finger-driven (FUNCTION + fader) per-lane scrubbing. 1× keeps the
 *  gesture tied to the finger's musical time, not a rewind shuttle. */
const MAX_FADER_SCRUB_RATE = 1;
/**
 * Scrub → playback handoff, seconds. The release must NOT use the 80 ms
 * transport lookahead: that overshoots audibly after key release. One shared
 * render-quantum-aligned frame instead.
 */
export const SCRUB_HANDOFF_MIN_S = 0.003;
/** Complementary (correlated) fade taking any sounding grain to zero. */
export const SCRUB_HANDOFF_FADE_S = 0.004;

export type TrackId = 0 | 1 | 2 | 3;

/** A one-shot cue voice. Deliberately NOT a LiveSource: nothing that walks
 *  `t.sources` — seams, wraps, respawns, relocations — can ever reach it. */
interface CueVoice {
  key: string;
  scope: "global" | "lane";
  lane: number;
  node: AudioBufferSourceNode;
  gain: GainNode;
  startAt: number;
  endAt: number;
  startPos: number;
  underlayAtStart: number | null;
}

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
  /** Mute/solo gate for the ordinary stem copy (head voices bypass it). */
  stemGate: GainNode;
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
  /** Automatic song grid (§8). Null means the project is not performance-ready. */
  songGrid: SongGrid | null;
  gridDetail: string;
  /** Transient per-lane scrub landings, in song seconds. */
  scrubCandidates: (number | null)[];
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
  /** Heads v2 truth: the four independent lane heads, straight from HeadLanes. */
  headLanes: HeadLaneSnapshot[];
  headsSummary: string;
  /** The ONE stem the four heads read while Heads is active. */
  headsSource: { index: number; name: string } | null;
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

/** RMS of whatever an analyser tap currently sees. */
function tapRms(a: AnalyserNode | null): number {
  if (!a) return 0;
  const buf = new Float32Array(a.fftSize);
  a.getFloatTimeDomainData(buf);
  let sum = 0;
  for (let i = 0; i < buf.length; i++) sum += buf[i]! * buf[i]!;
  return Math.sqrt(sum / buf.length);
}

export class AudioEngine {
  ctx: AudioContext | null = null;
  private master: GainNode | null = null;
  private masterAnalyser: AnalyserNode | null = null;
  /** Gate for the ENTIRE normal four-stem mix (dry + every FX return). */
  private normalBus: GainNode | null = null;
  /** Measurement tap AFTER the normal bus gain. */
  private normalTap: AnalyserNode | null = null;

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
  /**
   * The memory-mode concept was removed from the product: the engine always
   * runs at the platform's high-memory ceiling. Kept as a field only because
   * judge()/describeVerdict() take it as a parameter.
   */
  readonly highMemoryMode = true;
  lastError: string | null = null;
  lastDecodeMs: number | null = null;
  /** Published for the diagnostics panel. */
  readonly antiClickMatrix = ANTI_CLICK_MATRIX;
  /** Phase 6 — frame-anchored tempo grid, learned by tap. */
  grid: GridState = emptyGrid(48000);
  lastGridTapFrame: number | null = null;
  quantisePunch = false;

  // ------------------------------------------------ automatic song grid (§8)

  /**
   * Analysed shared song grid. Present automatically whenever stems are
   * loaded; a null grid means the project is NOT performance-ready.
   */
  songGrid: SongGrid | null = null;
  gridAnalysisDetail = "not analysed — no stems loaded";
  gridAnalysisMs: number | null = null;
  /** Generation of the analysis, bumped on every source change. */
  gridGeneration = 0;

  /**
   * Sequential, bounded analysis over the ALREADY-DECODED buffers. Reads the
   * channel views in place (no second decode, no duplicate PCM) and yields
   * between stems so the surface stays responsive.
   */
  async analyzeGrid(hashes: string[] = []): Promise<SongGrid | null> {
    const started = typeof performance !== "undefined" ? performance.now() : 0;
    const inputs: AnalysisInput[] = [];
    for (let i = 0; i < this.tracks.length; i++) {
      const buf = this.tracks[i]?.buffer;
      if (!buf) continue;
      inputs.push({ channel: buf.getChannelData(0), sampleRate: buf.sampleRate, hash: hashes[i] ?? "" });
      // Bounded: hand the main thread back between stems.
      await new Promise((r) => setTimeout(r, 0));
    }
    if (inputs.length === 0) {
      this.songGrid = null;
      this.gridAnalysisDetail = "not analysed — no decoded stems";
      return null;
    }
    const grid = analyzeSongGrid(inputs);
    this.gridAnalysisMs = (typeof performance !== "undefined" ? performance.now() : 0) - started;
    this.gridGeneration += 1;
    if (!grid) {
      this.songGrid = null;
      this.gridAnalysisDetail = "analysis found no periodic structure — tap Function ×4 to set the tempo";
      return null;
    }
    this.songGrid = grid;
    this.setBaseBpm(grid.bpm, "grid");
    this.grid = {
      ...this.grid,
      bpm: grid.bpm,
      sampleRate: grid.analysisSampleRate,
      source: "tapped",
      rejected: false,
    };
    this.gridAnalysisDetail = `${describeGrid(grid)} · ${inputs.length} stem(s) analysed in ${this.gridAnalysisMs.toFixed(0)} ms · single decode`;
    return grid;
  }

  /** Bar length in seconds from the analysed grid (falls back to baseBpm). */
  barSeconds(): number {
    return this.songGrid ? this.songGrid.barSeconds : (4 * 60) / (this.baseBpm || 120);
  }

  /**
   * Per-lane scrub landing candidate, in song-time seconds. Transient by
   * design: replaced by the next scrub, cleared on song switch or source
   * replacement, consumed by loop capture, never persisted.
   */
  scrubCandidate: (number | null)[] = [null, null, null, null];

  /**
   * The GLOBAL one-bar loop (Hold PLAY). One window shared by all four stems,
   * so a move or resize keeps them phase-locked. Null when no global loop is
   * running; per-lane loops are tracked separately on each track.
   */
  globalLoop: { start: number; lengthS: number; division: number } | null = null;

  setScrubCandidate(lane: number, seconds: number | null) {
    if (lane < 0 || lane > 3) return;
    this.scrubCandidate[lane] = seconds != null && Number.isFinite(seconds) ? Math.max(0, seconds) : null;
  }

  clearScrubCandidates(reason: string) {
    this.scrubCandidate = [null, null, null, null];
    void reason;
  }





  private perfRecorder: PerformanceRecorder | null = null;

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

  /** No-op: memory mode was removed; the ceiling is always the high one. */
  setHighMemoryMode(_on?: boolean) {}

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
      // Re-anchor the MIDI calibration pair on every unlock: cue frames are
      // derived from the event timestamp through THIS pair, so it must be
      // sampled together with a live context time.
      if (ok) midiClock.anchor(typeof performance !== "undefined" ? performance.now() : Date.now(), this.ctx.currentTime);
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

    // ---- explicit two-bus handoff ------------------------------------
    //   NORMAL STEM BUS → normalBusGain → normalTap ─┐
    //                                                ├→ master
    //   HEADS BUS       → headsBusGain  → headsTap  ─┘
    // Nothing else may connect to `master`. The taps sit AFTER the bus
    // gains, so a measurement proves what each bus contributes to master,
    // not what its stems produce upstream of the gate.
    this.normalBus = ctx.createGain();
    this.normalBus.gain.value = 1;
    this.normalTap = ctx.createAnalyser();
    this.normalTap.fftSize = 1024;
    this.normalBus.connect(this.normalTap);
    this.normalTap.connect(this.master);

    this.headsBus = ctx.createGain();
    this.headsBus.gain.value = 0;
    this.headsTap = ctx.createAnalyser();
    this.headsTap.fftSize = 1024;
    this.headsBus.connect(this.headsTap);
    this.headsTap.connect(this.master);


    this.tracks = [0, 1, 2, 3].map((i) => {
      const input = ctx.createGain();
      // Stem gate: mute/solo close THIS node, not the FX rack input, so a head
      // voice injected at `input` still rides the lane's FX chain while the
      // ordinary stem copy is silenced.
      const stemGate = ctx.createGain();
      stemGate.gain.value = 1;
      stemGate.connect(input);
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
      //         → BankRack (TONE → MOD → MOTION → SPACE)
      //         → fader → solo → analyser → master.
      const bankRack = new BankRack(ctx);
      const fxRack = new FxRack(ctx, bankRack.input);
      bankRack.output.connect(gain);
      preFx.connect(fxRack.input);

      gain.connect(soloGain);
      soloGain.connect(analyser);
      // Every normal path — dry AND every FX return — is already summed into
      // `gain → soloGain → analyser` upstream, so this ONE edge is the whole
      // normal contribution to the master. It lands on the normal bus.
      analyser.connect(this.normalBus!);

      gain.gain.value = [0.78, 0.72, 0.65, 0.7][i] ?? 0.7;

      return {
        buffer: null,
        reversed: null,
        trash: null,
        input,
        stemGate,
        preFx,
        fxRack,
        bankRack,
        gain,
        soloGain,
        soloed: false,
        linked: true,
        fxVariation: { filter: 1, echo: 1, reverb: 1 },
        fxActive: { filter: false, echo: false, reverb: false },
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
    meta: {
      name: string;
      provenance: TrackRuntime["provenance"];
      decodeMs?: number | undefined;
      reused?: boolean | undefined;
      /**
       * Stem identity. Cue markers are keyed to it, so it MUST be recorded the
       * instant the buffer is installed — recording it later (for example when
       * the grid analysis finishes) would invalidate every marker learned in
       * the meantime, because it would look like the stem had been replaced.
       */
      contentHash?: string | undefined;
    },
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
    // Stem identity, recorded synchronously with the buffer. A lane whose hash
    // actually changed retires its markers here and nowhere else.
    this.contentHashes[id] = meta.contentHash ?? "";
    this.cues.revalidate(this.contentHashes);
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

  /**
   * `offset` is always a SONG position (forward time). When the lane is
   * reversed the read offset is mirrored into the reversed copy here, so every
   * caller keeps speaking forward song time.
   */
  private spawn(t: TrackRuntime, startAt: number, offset: number, fadeIn: boolean): LiveSource | null {
    const ctx = this.ctx!;
    const buf = this.activeBuffer(t);
    if (!buf) return null;
    const readOffset = t.loop.reverse ? buf.duration - offset : offset;
    const fade = ctx.createGain();
    fade.connect(t.stemGate);
    const node = ctx.createBufferSource();
    node.buffer = buf;
    node.playbackRate.value = this.timeline.currentRate(ctx.currentTime);
    node.connect(fade);
    // Reversed + looping: ONLY the loop reverses. The node wraps itself inside
    // the mirrored loop window, so the shared seam scheduler stays out of it.
    if (t.loop.reverse && t.loop.enabled) {
      const b = this.loopBounds(t);
      if (b) {
        node.loop = true;
        node.loopStart = Math.max(0, buf.duration - b.end);
        node.loopEnd = Math.min(buf.duration, buf.duration - b.start);
      }
    }
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
    node.start(startAt, Math.min(Math.max(0, readOffset), Math.max(0, buf.duration - 1e-4)));
    t.sources.push(live);
    t.scheduledStartAt = startAt;
    return live;
  }

  /**
   * Per-lane respawn for a reverse flip. Unlike `relocate` it NEVER anchors the
   * shared timeline: the hidden song clock keeps running forward underneath
   * while this one lane reads backwards, which is exactly what lets a
   * reverse-off rejoin the current song position.
   */
  private respawnLane(t: TrackRuntime, songPos: number): boolean {
    const ctx = this.ctx;
    if (!ctx || !this.requestedPlaying) return false;
    const at = ctx.currentTime + 0.01;
    const outgoing = t.sources[t.sources.length - 1];
    if (outgoing) this.fadeOutAndStop(t, outgoing, at);
    const bounds = t.loop.enabled ? this.loopBounds(t) : null;
    const pos = bounds ? Math.min(Math.max(songPos, bounds.start), bounds.end) : songPos;
    const live = this.spawn(t, at, pos, true);
    if (live) t.committedSeamAt = null;
    return live != null;
  }


  private fadeOutAndStop(t: TrackRuntime, live: LiveSource, at: number) {
    // A scheduled release's replacement source is protected by IDENTITY: no
    // pre-boundary cleanup (wrap, relocate, respawn, sweep) may fade or stop
    // it, or the lane rejoins the song seconds behind the hidden timeline.
    if (this.isReleaseTarget(t, live)) return;
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
    // Cancellation: a teardown is allowed to delete a protected target.
    this.pendingRelease = [null, null, null, null];
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
    return this.startAllAt(offset, this.ctx!.currentTime + LOOKAHEAD_S);
  }

  /**
   * Start every stem at an EXPLICIT shared context time. The scrub release
   * uses this with a render-quantum handoff instead of the 80 ms lookahead.
   */
  private startAllAt(offset: number, startAt: number): { started: number; startAt: number } {
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
    for (let li = 0; li < this.tracks.length; li++) {
      const t = this.tracks[li]!;
      if (this.heads.active && this.heads.engine === "node" && this.heads.source === li) continue;
      if (!t.buffer || !t.loop.enabled) continue;
      // A reversed lane wraps itself inside the mirrored loop window (see
      // `spawn`), so the forward seam scheduler must not also cut it.
      if (t.loop.reverse) continue;


      if (t.committedSeamAt != null && t.committedSeamAt > now) continue;
      const bounds = this.loopBounds(t);
      if (!bounds || bounds.length <= SEAM_FADE_S * 2) continue;
      // A scheduled release has ALREADY queued its replacement source at the
      // bar boundary, and that replacement is the newest entry in `sources`.
      // The wrap scheduler must ignore it entirely: the audible voice is the
      // newest source from before the release (gen <= oldGen). Wrapping off the
      // raw tail would fade out the replacement and respawn the lane from the
      // loop start, which is exactly how a released loop used to rejoin the
      // song several seconds behind the hidden timeline.
      const pendingRel = this.pendingRelease[li];
      const pool = pendingRel ? t.sources.filter((s) => s !== pendingRel.target) : t.sources;
      const live = pool[pool.length - 1];
      if (!live) continue;
      // The lane's seam is derived from ITS OWN read pointer, expressed in the
      // shared integrated-rate timeline:
      //   seamPosition = position(live.startAt) + (loopEnd - live.startPos)
      // The shared timeline is NEVER re-anchored by a wrap, so the hidden song
      // clock keeps advancing underneath a looping lane. That hidden value is
      // what the lane rejoins on release.
      const seamPos = this.timeline.positionAt(live.startAt) + (bounds.end - live.startPos);
      const seamAt = this.timeline.timeAtPosition(now, seamPos);
      if (seamAt == null) continue;
      const fadeStart = seamAt - SEAM_FADE_S;
      if (fadeStart > now + SEAM_LOOKAHEAD_S) continue;
      const at = Math.max(fadeStart, now + 0.005);
      // Never wrap a lane past a scheduled loop release — the release spawn
      // owns everything at and after its bar boundary.
      if (pendingRel && at >= pendingRel.at - 1e-6) continue;
      this.fadeOutAndStop(t, live, at);
      const wrapped = this.spawn(t, at, bounds.start, true);
      // A wrap spawned UNDER a pending release must die at the boundary too —
      // the replacement owns everything from that bar onwards.
      if (wrapped && pendingRel) this.fadeOutAndStop(t, wrapped, pendingRel.at);
      t.committedSeamAt = seamAt;
      t.seamCount++;
    }
    this.settlePendingReleases(now);
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
  private relocateShared(t: TrackRuntime, toPosition: number): boolean {
    const ctx = this.ctx;
    if (!ctx || !this.requestedPlaying) return false;
    const at = ctx.currentTime + 0.01;
    const outgoing = t.sources[t.sources.length - 1];
    if (outgoing) this.fadeOutAndStop(t, outgoing, at);
    const ok = this.spawn(t, at, toPosition, true) != null;
    if (ok) {
      t.committedSeamAt = null;
      // Only a TRANSPORT relocation (seek/scrub landing) may move the shared
      // clock. Loop work must never come through here.
      this.timeline.anchor(at, toPosition);
    }
    return ok;
  }

  /**
   * Per-lane relocation: identical crossfade, but the shared hidden timeline is
   * left strictly alone. Loop capture, chop and loop-mode transitions use this,
   * which is what lets a released loop rejoin the song where it actually got to.
   */
  private relocateLane(t: TrackRuntime, toPosition: number): boolean {
    const ctx = this.ctx;
    if (!ctx || !this.requestedPlaying) return false;
    const at = ctx.currentTime + 0.01;
    const outgoing = t.sources[t.sources.length - 1];
    if (outgoing) this.fadeOutAndStop(t, outgoing, at);
    const ok = this.spawn(t, at, toPosition, true) != null;
    if (ok) t.committedSeamAt = null;
    return ok;
  }

  /**
   * Defect 2 — bar-synchronised loop release.
   *
   * A release is scheduled for the next shared bar boundary of the analysed
   * grid. At that instant the lane must resume from the HIDDEN song position —
   * the value the shared integrated-rate timeline has reached — NOT from the
   * bar the loop was captured on, and NOT from the loop window.
   */
  /** True while `live` is the protected replacement of a pending release. */
  private isReleaseTarget(t: TrackRuntime, live: LiveSource): boolean {
    const i = this.tracks.indexOf(t);
    if (i < 0) return false;
    const p = this.pendingRelease[i];
    return p != null && p.target === live;
  }

  private pendingRelease: ({
    at: number;
    pos: number;
    oldGen: number;
    newGen: number;
    target: LiveSource;
  } | null)[] = [
    null,
    null,
    null,
    null,
  ];

  /** Frames the lane will actually start reading at, per scheduled release. */
  lastReleasePlan: { lane: number; boundaryPos: number; sourceFrame: number; at: number }[] = [];

  /** Where the last global-loop release landed the SHARED timeline. */
  lastGlobalRelease: { at: number; position: number; lanes: number } | null = null;

  /**
   * GLOBAL loop release — the whole SONG was looping, so there is no hidden
   * timeline to rejoin. The shared audible frame at release becomes the song
   * position: it is captured, every lane crossfades to it click-free, the
   * shared timeline is re-anchored there and all four stems continue forward
   * from exactly that frame with one playback path each.
   */
  private releaseGlobalLoop(): { ok: boolean; detail: string } {
    const ctx = this.ctx;
    const dur = this.duration;
    if (!ctx || !this.requestedPlaying) {
      // Stopped: just drop the windows; the next start reads the song position.
      let n = 0;
      for (const t of this.tracks) {
        if (t.loop.enabled) {
          t.loop = { ...t.loop, enabled: false };
          n++;
        }
      }
      this.invalidateSeams();
      return { ok: true, detail: n ? `global loop released while stopped (${n} lanes)` : "no global loop was running" };
    }

    // The audible frame of the looping song: any lane reads the same window, so
    // lane read position = its source start offset + integrated distance since.
    const now = ctx.currentTime;
    const at = now + 0.01;
    const advance = this.timeline.positionAt(at) - this.timeline.positionAt(now);
    let audible: number | null = null;
    for (let i = 0; i < this.tracks.length; i++) {
      const t = this.tracks[i]!;
      if (!t.loop.enabled) continue;
      const live = t.sources[t.sources.length - 1];
      if (!live) continue;
      audible = live.startPos + (this.timeline.positionAt(now) - this.timeline.positionAt(live.startAt));
      break;
    }
    if (audible == null) {
      for (const t of this.tracks) t.loop = { ...t.loop, enabled: false };
      this.invalidateSeams();
      return { ok: true, detail: "no global loop was running" };
    }
    const landing = Math.max(0, dur ? Math.min(dur - 1e-3, audible + advance) : audible + advance);

    let lanes = 0;
    for (let i = 0 as TrackId; i < 4; i = (i + 1) as TrackId) {
      const t = this.tracks[i];
      if (!t) continue;
      const wasLooping = t.loop.enabled;
      // Close the window BEFORE spawning so the replacement is a straight
      // source, then fade every existing voice out across the same seam.
      t.loop = { ...t.loop, enabled: false };
      this.pendingRelease[i] = null;
      if (!wasLooping || !t.buffer) continue;
      const outgoing = [...t.sources];
      const live = this.spawn(t, at, landing, true);
      if (!live) continue;
      for (const s of outgoing) this.fadeOutAndStop(t, s, at);
      t.committedSeamAt = null;
      lanes++;
      if (t.engineMode === "worklet" && t.worklet) {
        void t.worklet.post({
          type: "releaseLoop",
          targetSourceFrame: Math.round(landing * ctx.sampleRate),
          fadeFrames: Math.round(SEAM_FADE_S * ctx.sampleRate),
          applyAtContextFrame: Math.round(at * ctx.sampleRate),
        });
      }
    }
    // The SONG itself was looping: the shared clock follows the audible frame.
    this.timeline.anchor(at, landing);
    this.invalidateSeams();
    this.lastGlobalRelease = { at, position: landing, lanes };
    return {
      ok: true,
      detail: `global loop released at the audible frame ${landing.toFixed(3)}s — ${lanes} lanes continue forward from there`,
    };
  }


  private scheduleLoopRelease(id: TrackId, t: TrackRuntime): { ok: boolean; detail: string } {
    const ctx = this.ctx;
    if (!ctx) return { ok: false, detail: "audio not unlocked" };
    if (!t.loop.enabled) return { ok: true, detail: `lane ${id + 1} had no loop to release` };

    // Stopped or silent lane: nothing is audible, so drop the window and let
    // the next start read the hidden timeline normally.
    if (!this.requestedPlaying || t.sources.length === 0) {
      t.loop = { ...t.loop, enabled: false };
      this.invalidateSeams();
      return { ok: true, detail: `lane ${id + 1} loop released while stopped — next start reads the song position` };
    }

    const grid = this.songGrid;
    const now = ctx.currentTime;
    const hiddenNow = this.timeline.positionAt(now);
    const boundaryPos = grid ? nextBarAfter(grid, hiddenNow) : hiddenNow + 0.02;
    const at = this.timeline.timeAtPosition(now, boundaryPos);
    if (at == null) {
      return {
        ok: false,
        detail: `lane ${id + 1} release deferred — the rate curve is still settling, so the bar boundary is not yet solvable`,
      };
    }

    const sr = ctx.sampleRate;
    const sourceFrame = Math.round(boundaryPos * sr);
    const outgoing = [...t.sources];
    const oldGen = t.generation;
    // The replacement source must be a STRAIGHT source: spawn reads
    // t.loop.enabled for its mirrored-window wrap, so the window is closed for
    // the spawn call and re-opened for the old source's remaining seams.
    const savedEnabled = t.loop.enabled;
    t.loop = { ...t.loop, enabled: false };
    const live = this.spawn(t, at, boundaryPos, true);
    t.loop = { ...t.loop, enabled: savedEnabled };
    if (!live) {
      t.loop = { ...t.loop, enabled: false };
      return { ok: false, detail: `lane ${id + 1} release failed — no decoded buffer` };
    }
    for (const s of outgoing) this.fadeOutAndStop(t, s, at);
    this.pendingRelease[id] = { at, pos: boundaryPos, oldGen, newGen: live.gen, target: live };
    this.lastReleasePlan = [
      ...this.lastReleasePlan.filter((r) => r.lane !== id).slice(-3),
      { lane: id, boundaryPos, sourceFrame, at },
    ];

    if (t.engineMode === "worklet" && t.worklet) {
      // Worklet parity: the processor gets the SAME landing expressed as an
      // absolute source frame plus the shared context frame to apply it on.
      void t.worklet.post({
        type: "releaseLoop",
        targetSourceFrame: sourceFrame,
        fadeFrames: Math.round(SEAM_FADE_S * sr),
        applyAtContextFrame: Math.round(at * sr),
      });
    }

    return {
      ok: true,
      detail:
        `lane ${id + 1} loop release scheduled for the next bar at ${boundaryPos.toFixed(3)}s ` +
        `(source frame ${sourceFrame}, ctx ${at.toFixed(3)}s) — rejoining the hidden song timeline`,
    };
  }

  /** Finalise any release whose crossfade has completed. */
  private settlePendingReleases(now: number) {
    for (let i = 0; i < this.pendingRelease.length; i++) {
      const p = this.pendingRelease[i];
      if (!p || now < p.at + SEAM_FADE_S) continue;
      const t = this.tracks[i]!;
      t.loop = { ...t.loop, enabled: false };
      t.committedSeamAt = null;
      t.seamGeneration++;
      // Any straggler from before the boundary is gone by construction, but
      // sweep by generation so a mis-timed wrap can never survive the release.
      // Promotion: protection is cleared FIRST so the sweep below is the only
      // thing that can touch sources, then every voice other than the promoted
      // target (by identity) is removed.
      this.pendingRelease[i] = null;
      for (const s of [...t.sources]) {
        if (s === p.target) continue;
        try {
          s.node.stop();
        } catch {
          /* already stopped */
        }
      }
    }
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


    t.migrationStatus = "checking";
    const pre = await this.preflight();
    if (!pre.ok) {
      t.migrationStatus = "refused";
      t.fallbackReason = pre.checks.filter((c) => !c.ok).map((c) => c.detail).join(" · ");
      this.note(`T${id + 1} migration refused — ${t.fallbackReason}`);
      return { ok: false, detail: t.fallbackReason };
    }

    const wt = new WorkletTrack(id, this.ctx, t.stemGate, (w, detail) => this.handleProcessorError(w, detail));
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

  /** Fan out at an EXPLICIT shared frame (scrub release handoff). */
  private fanoutAt(at: number, build: (t: TrackRuntime, at: number) => Parameters<WorkletTrack["post"]>[0] | null) {
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

  /**
   * Momentary audition (bare Track hold). A NON-destructive layer that sits on
   * top of latched solo and mutes: it is never written into track.muted or
   * track.soloed, so releasing the hold restores the exact prior mix.
   */
  private audition: boolean[] = [false, false, false, false];

  /** Lanes currently auditioned — surfaced in diagnostics. */
  auditionMask(): string {
    return this.audition.map((a) => (a ? "1" : "0")).join("");
  }

  /** Apply a whole audition mask at once ("0110", or "" to end the audition). */
  setAudition(mask: string) {
    this.audition = [0, 1, 2, 3].map((i) => mask[i] === "1");
    this.applyAudibilityAll();
  }

  private anySolo(): boolean {
    return this.tracks.some((t) => t.soloed);
  }

  /**
   * Correction 3. Solo never mutates saved mute state:
   *   audibleBySolo = anySolo ? track.soloed : true
   *   inputOpen     = audibleBySolo && (!track.muted || track.soloed)
   *
   * Momentary audition overrides both while it is held, and leaves no trace.
   */
  applyAudibility(id: TrackId) {
    const t = this.tracks[id];
    if (!t || !this.ctx) return;
    const auditing = this.audition.some(Boolean);
    const audibleBySolo = auditing ? this.audition[id] === true : this.anySolo() ? t.soloed : true;
    // Heads layer OVER the dry mix: the four head voices ride the source
    // track's own chain, so the source gate stays open while heads are active
    // even if the user has muted that stem in the dry mixer (which only
    // silences the normal copy — the head voices replace it).
    // A CUE voice owns this lane: it is injected at `input`, downstream of the
    // stem gate, so the ordinary copy is closed and the solo gate is forced
    // open. Neither `muted` nor `soloed` is touched — the override disappears
    // with the voice and the saved mix returns exactly as it was.
    const cued = this.cueOverride[id] === true;
    const stemAudible = !cued && audibleBySolo && (!t.muted || t.soloed || (auditing && this.audition[id] === true));
    // The FX rack stays OPEN whenever the stem is audible OR a head is riding
    // this lane, so Heads is processed by that lane's FX. The stem's own copy
    // is silenced at the gate instead.
    // Heads no longer ride the per-stem chains — they have their own bus into
    // the master — so the lane FX gate follows the stem alone.
    t.fxRack?.setInputOpen(stemAudible || cued);
    this.setGain(t.stemGate.gain, stemAudible ? 1 : 0);
    this.setGain(t.soloGain.gain, cued || audibleBySolo ? 1 : 0);
  }


  private applyAudibilityAll() {
    for (let i = 0; i < this.tracks.length; i++) this.applyAudibility(i as TrackId);
  }

  // ------------------------------------------------------- Stem Instrument
  //
  // A cue is a ONE-SHOT passage of a lane's own decoded buffer, injected at
  // `t.input`. That single connection point is the whole isolation design:
  //
  //   cueGain → input → dry/filter → preFx → FX rack → fader → solo → bus
  //             ▲
  //             └── the stem gate (mute/solo) sits UPSTREAM of `input`, so the
  //                 ordinary copy can be closed for the duration of the cue
  //                 without the cue itself being gated.
  //
  // The underlay is never stopped. It keeps running silently behind a closed
  // stem gate, which is why rejoin is exact rather than re-derived: when the
  // gate reopens, the lane is already at the frame it would have reached.
  //
  // Cue voices live in `cueVoices`, NOT in `t.sources`. Seam scheduling, loop
  // wraps, respawns and relocations only ever walk `t.sources`, so no ordinary
  // transport event can retarget or kill a cue voice — identity protection by
  // construction, not by flag checking.

  readonly cues = new CueStore();
  /** Content identity per lane, mirrored from the last grid analysis. */
  contentHashes: string[] = ["", "", "", ""];
  private cueVoices: (CueVoice | null)[] = [null, null, null, null];
  private cueOverride: boolean[] = [false, false, false, false];
  private cueTimers: (ReturnType<typeof setTimeout> | null)[] = [null, null, null, null];
  /** Song timeline parked by a held GLOBAL cue: the frame to resume from. */
  private cuePark: { pos: number; wasPlaying: boolean; phase: AudioEngine["transportPhase"] } | null = null;
  lastCueDetail: string | null = null;
  readonly cueLog: { t: number; action: string; detail: string }[] = [];

  /** Restore persisted markers, then re-check them against the loaded stems. */
  loadCueMarkers(markers: readonly CueMarker[]): { loaded: number; invalidated: number } {
    this.cues.load(markers);
    const r = this.cues.revalidate(this.contentHashes);
    return { loaded: markers.length, invalidated: r.invalidated.length };
  }

  /** Engine conditions right now, in the pure module's shape. */
  cueEligibility(): EligibilitySnapshot {
    const scrubActive =
      this.globalScrub != null ||
      this.laneFaderScrub.some((s) => s != null) ||
      this.laneScrub.some((s) => s != null);
    return {
      headsActive: this.heads.active || this.headLanes.active,
      scrubActive,
      reverseActive: this.tracks.some((t) => t.loop.reverse),
      loopActive: this.globalLoop != null || this.tracks.some((t) => t.loop.enabled),
      transportPlaying: this.requestedPlaying && this.transportPhase === "playing",
      rate: this.timeline.musicalRate(),
    };
  }

  /**
   * The frame the event actually landed on.
   *
   * Derived from the MIDI timestamp through the calibrated clock and the
   * integrated rate curve — never from `position()` at the moment JavaScript
   * happened to run the handler.
   */
  private cueFrameOf(ev: Pick<StemMidiEvent, "timestampMs">): number {
    const sr = this.ctx?.sampleRate ?? 48000;
    const ctxTime = midiClock.isAnchored() ? midiClock.ctxTimeOf(ev) : (this.ctx?.currentTime ?? 0);
    return Math.max(0, Math.round(this.positionAtCtxTime(ctxTime) * sr));
  }

  private positionAtCtxTime(ctxTime: number): number {
    if (!this.ctx) return 0;
    if (this.globalScrub) return Math.max(0, Math.min(this.duration, this.globalScrub.pos));
    if (!this.requestedPlaying) return this.timeline.positionAt(this.timelineFrozenAt);
    return Math.max(0, Math.min(this.duration, this.timeline.positionAt(ctxTime)));
  }

  private noteCue(action: string, detail: string) {
    this.lastCueDetail = detail;
    this.cueLog.push({ t: typeof performance !== "undefined" ? performance.now() : Date.now(), action, detail });
    if (this.cueLog.length > 120) this.cueLog.splice(0, this.cueLog.length - 120);
  }

  /** One normalized MIDI event → one cue decision, plus its audible effect. */
  handleMidiCue(ev: StemMidiEvent, qualifiers: QualifierSnapshot): { action: CueAction; detail: string } {
    const eligibility = this.cueEligibility();
    // An eligibility condition that began MID-capture discards the open
    // captures first, so a commit can never straddle a loop or a scrub.
    for (const stale of this.cues.syncEligibility(eligibility)) {
      this.noteCue(stale.type, describeCueAction(stale));
    }
    const ctx: CueEventContext = {
      frame: this.cueFrameOf(ev),
      qualifiers,
      eligibility,
      sampleRate: this.ctx?.sampleRate ?? 48000,
      contentHashes: this.contentHashes,
      decodedLanes: this.tracks.map((t) => !!t?.buffer),
    };
    const action = this.cues.handle(ev, ctx);
    let detail = describeCueAction(action);
    if (action.type === "cue.play") {
      const r = this.playCue(action.marker);
      detail = r.detail;
    }
    if (action.type === "cue.release") {
      detail = this.releaseCueByKey(action.key).detail;
    }
    if (ev.kind === "allNotesOff") this.stopAllCues("all notes off");
    this.noteCue(action.type, detail);
    return { action, detail };
  }

  /**
   * Held-pad release, keyed by channel:note. Every cue voice carrying that key
   * fades out across the seam; a global cue also unparks the song timeline and
   * resumes all four stems from the exact frame the Note On froze.
   */
  private releaseCueByKey(key: string): { ok: boolean; detail: string } {
    let released = 0;
    for (let lane = 0; lane < this.cueVoices.length; lane++) {
      const voice = this.cueVoices[lane];
      if (!voice || voice.key !== key) continue;
      this.stopCueVoice(lane, "released");
      const timer = this.cueTimers[lane];
      if (timer) clearTimeout(timer);
      this.cueTimers[lane] = null;
      this.cueOverride[lane] = false;
      if (this.ctx) this.applyAudibility(lane as TrackId);
      released++;
    }
    if (released === 0) return { ok: false, detail: `cue ${key} was not sounding` };
    const resumed = this.resumeCuePark();
    return {
      ok: true,
      detail: `cue ${key} released — ${released} lane${released === 1 ? "" : "s"} rejoined${resumed ? ` · song resumed at ${resumed.toFixed(3)}s` : ""}`,
    };
  }

  /**
   * Unpark the song after the last global cue voice goes away. The tape restarts
   * from the frame the cue froze — transport, loop, mixer and FX untouched.
   */
  private resumeCuePark(): number | null {
    const park = this.cuePark;
    if (!park || !this.ctx) return null;
    if (this.cueVoices.some((v) => v && v.scope === "global")) return null;
    this.cuePark = null;
    const now = this.ctx.currentTime;
    this.timeline.anchor(now, park.pos);
    this.timelineFrozenAt = now;
    if (park.wasPlaying) {
      this.stopSources();
      this.requestedPlaying = true;
      this.transportPhase = park.phase;
      this.startAll(park.pos);
      this.invalidateSeams();
    }
    return park.pos;
  }


  /** Schedule one marker. Global markers hit all four lanes at the SAME time. */
  private playCue(marker: CueMarker): { ok: boolean; detail: string } {
    const ctx = this.ctx;
    if (!ctx) return { ok: false, detail: "cue rejected — audio not unlocked" };
    if (this.heads.active || this.headLanes.active) return { ok: false, detail: "cue rejected — Heads mode is active" };
    if (this.globalScrub) return { ok: false, detail: "cue rejected — the shuttle is open" };
    const lanes: number[] = marker.scope === "global" ? [0, 1, 2, 3] : [marker.lane ?? 0];
    for (const lane of lanes) {
      const t = this.tracks[lane];
      if (!t?.buffer) return { ok: false, detail: `cue rejected — lane ${lane + 1} has no decoded stem` };
      if (t.loop.reverse) return { ok: false, detail: `cue rejected — lane ${lane + 1} is reversed` };
      if (this.laneFaderScrub[lane] || this.laneScrub[lane]) {
        return { ok: false, detail: `cue rejected — lane ${lane + 1} is scrubbing` };
      }
    }
    const at = ctx.currentTime + SEAM_FADE_S;
    const startS = marker.startFrame / marker.sampleRate;
    const durS = (marker.endFrame - marker.startFrame) / marker.sampleRate;
    // A GLOBAL cue parks the song: the timeline freezes on the exact frame the
    // Note On landed on and every ordinary source stops, so nothing advances
    // underneath. Transport, loop, mixer and FX state are left untouched.
    if (marker.scope === "global" && !this.cuePark) {
      const pos = this.position();
      this.cuePark = { pos, wasPlaying: this.requestedPlaying, phase: this.transportPhase };
      if (this.requestedPlaying) {
        this.stopSources();
        this.requestedPlaying = false;
      }
      this.timeline.anchor(ctx.currentTime, pos);
      this.timelineFrozenAt = ctx.currentTime;
    }
    for (const lane of lanes) this.spawnCue(lane, marker, startS, durS, at);
    return {
      ok: true,
      detail: `cue ${marker.key} · ${marker.scope === "global" ? "all four stems (song parked)" : `lane ${lanes[0]! + 1}`} · ${(durS * 1000).toFixed(0)} ms from ${startS.toFixed(3)}s`,
    };
  }


  /**
   * DEDICATED cue spawn. Not `spawn()`: a cue never loops, never mirrors into
   * the reversed copy, never follows varispeed and never joins `t.sources`.
   */
  private spawnCue(lane: number, marker: CueMarker, startS: number, durS: number, at: number): CueVoice | null {
    const ctx = this.ctx!;
    const t = this.tracks[lane];
    const buf = t?.buffer;
    if (!t || !buf) return null;
    // Retrigger: the previous voice fades out across the same seam the new one
    // fades in over — at most two cue voices, for at most SEAM_FADE_S.
    this.stopCueVoice(lane, "retriggered", at);

    const offset = Math.max(0, Math.min(buf.duration, startS));
    const length = Math.max(0, Math.min(buf.duration - offset, durS));
    if (length <= 0) return null;

    const gain = ctx.createGain();
    gain.gain.value = 0;
    gain.connect(t.input);
    const node = ctx.createBufferSource();
    node.buffer = buf;
    node.playbackRate.value = 1;
    node.connect(gain);

    // Uncorrelated material against the underlay → equal power, both ends.
    const endAt = at + length;
    const rise = sampleCurve(equalPower, "b", 32);
    const fall = sampleCurve(equalPower, "a", 32);
    gain.gain.setValueCurveAtTime(rise, at, SEAM_FADE_S);
    gain.gain.setValueAtTime(1, at + SEAM_FADE_S);
    if (length > SEAM_FADE_S * 2) gain.gain.setValueCurveAtTime(fall, endAt - SEAM_FADE_S, SEAM_FADE_S);
    node.start(at, offset, length);
    node.stop(endAt + 0.001);

    const voice: CueVoice = {
      key: marker.key,
      scope: marker.scope,
      lane,
      node,
      gain,
      startAt: at,
      endAt,
      startPos: offset,
      underlayAtStart: this.laneUnderlayPosition(lane),
    };
    this.cueVoices[lane] = voice;
    this.cueOverride[lane] = true;
    this.applyAudibility(lane as TrackId);
    node.onended = () => this.releaseCue(lane, voice, "completed");
    const ms = Math.max(0, (endAt - ctx.currentTime) * 1000) + 40;
    const timer = this.cueTimers[lane];
    if (timer) clearTimeout(timer);
    this.cueTimers[lane] = setTimeout(() => this.releaseCue(lane, voice, "completed"), ms);
    return voice;
  }

  /** Identity-checked release: a stale voice can never reopen a live lane. */
  private releaseCue(lane: number, voice: CueVoice, reason: string) {
    if (this.cueVoices[lane] !== voice) return;
    this.cueVoices[lane] = null;
    const timer = this.cueTimers[lane];
    if (timer) clearTimeout(timer);
    this.cueTimers[lane] = null;
    this.cueOverride[lane] = false;
    try {
      voice.node.disconnect();
      voice.gain.disconnect();
    } catch {
      /* already torn down */
    }
    if (this.ctx) this.applyAudibility(lane as TrackId);
    // Passage ended before the pad was released: return automatically.
    this.resumeCuePark();
    this.noteCue("cue.release", `lane ${lane + 1} rejoined its underlay — ${reason}`);
  }

  private stopCueVoice(lane: number, reason: string, at?: number) {
    const voice = this.cueVoices[lane];
    if (!voice || !this.ctx) return;
    const now = at ?? this.ctx.currentTime;
    try {
      voice.gain.gain.cancelScheduledValues(now);
      voice.gain.gain.setValueCurveAtTime(sampleCurve(equalPower, "a", 32), now, SEAM_FADE_S);
      voice.node.stop(now + SEAM_FADE_S + 0.001);
    } catch {
      /* already stopped */
    }
    // Ownership passes to the incoming voice; the outgoing one may not reopen
    // the gate when its own `onended` fires.
    this.cueVoices[lane] = null;
    this.noteCue("cue.stop", `lane ${lane + 1} cue ${voice.key} — ${reason}`);
  }

  /** Song change, panic, disposal. */
  stopAllCues(reason: string) {
    for (let i = 0; i < this.tracks.length; i++) {
      const voice = this.cueVoices[i];
      if (!voice) continue;
      this.stopCueVoice(i, reason);
      this.cueOverride[i] = false;
      const timer = this.cueTimers[i];
      if (timer) clearTimeout(timer);
      this.cueTimers[i] = null;
      if (this.ctx) this.applyAudibility(i as TrackId);
    }
    this.resumeCuePark();
    for (const a of this.cues.cancelAllCaptures("cancelled")) this.noteCue(a.type, describeCueAction(a));
  }

  /**
   * Where the lane's ordinary (possibly silenced) voice is reading right now.
   * This is the underlay a finished cue rejoins; it never stopped, so the
   * rejoin error is the difference between this and the song clock.
   */
  laneUnderlayPosition(lane: number): number | null {
    const t = this.tracks[lane];
    const ctx = this.ctx;
    if (!t || !ctx) return null;
    const live = t.sources[t.sources.length - 1];
    if (!live) return null;
    const now = this.requestedPlaying ? ctx.currentTime : this.timelineFrozenAt;
    return live.startPos + (this.timeline.positionAt(now) - this.timeline.positionAt(live.startAt));
  }

  /** Read-only cue truth for the status strip and the browser proofs. */
  cueSnapshot() {
    const markers = this.cues.list();
    return {
      learned: markers.filter((m) => !m.invalidReason).length,
      invalid: markers.filter((m) => m.invalidReason).length,
      markers: markers.map((m) => ({
        key: m.key,
        scope: m.scope,
        lane: m.lane,
        frames: m.endFrame - m.startFrame,
        startFrame: m.startFrame,
        invalidReason: m.invalidReason,
      })),
      openCaptures: this.cues.openCaptures(),
      owned: this.cueOverride.slice(),
      voices: this.cueVoices.map((v, i) => (v ? { lane: i, key: v.key, scope: v.scope, startAt: v.startAt, endAt: v.endAt } : null)),
      /** Per lane: cue voice + ordinary sources that are NOT gated shut. */
      audibleVoices: this.tracks.map(
        // A cue OWNS its lane: the ordinary copy is gated shut for the whole
        // voice, so the lane carries exactly one audible voice. `gain.value`
        // does not reflect a scheduled ramp, so the override — not the param —
        // is the truth here.
        (t, i) => (this.cueVoices[i] ? 1 : t.stemGate.gain.value > 0.01 ? t.sources.length : 0),
      ),
      underlay: this.tracks.map((_, i) => this.laneUnderlayPosition(i)),
      stemGate: this.tracks.map((t) => Number(t.stemGate.gain.value.toFixed(4))),
      soloGate: this.tracks.map((t) => Number(t.soloGain.gain.value.toFixed(4))),
      fader: this.tracks.map((t) => Number(t.gain.gain.value.toFixed(4))),
      eligibility: this.cueEligibility(),
      lastDetail: this.lastCueDetail,
      log: this.cueLog.slice(-40),
    };
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
    stage.select(algorithm, this.ctx.currentTime);
    return { ok: true, detail: `stem ${id + 1} bank ${bank + 1} → ${algorithmDef(bank, algorithm).label}` };
  }

  /** Macro values are PER ALGORITHM, never shared across a bank. */
  setBankMacro(id: TrackId, bank: BankIndex, algorithm: AlgorithmIndex, value: number): { ok: boolean; detail: string } {
    const t = this.tracks[id];
    if (!t?.bankRack || !this.ctx) return { ok: false, detail: "audio not unlocked" };
    const v = Math.min(1, Math.max(0, value));
    t.bankRack.stage(bank).setMacro(algorithm, v, this.ctx.currentTime);
    return { ok: true, detail: `stem ${id + 1} ${algorithmDef(bank, algorithm).label} macro → ${v.toFixed(2)}` };
  }

  clearBanks(id: TrackId): { ok: boolean; detail: string } {
    const t = this.tracks[id];
    if (!t?.bankRack || !this.ctx) return { ok: false, detail: "audio not unlocked" };
    for (const stage of t.bankRack.stages) stage.setActive(false, this.ctx.currentTime);
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
  /** Analyser on the heads bus — head-path output, isolated from the transport. */
  private headsTap: AnalyserNode | null = null;

  /** RMS of the heads bus only. 0 when heads mode is not serving audio. */
  /**
   * Heads Mode: FOUR INDEPENDENT READERS over ONE selected stem buffer, each
   * parked at a different moment of the song. The decoded buffer is shared by
   * reference — never duplicated, never mutated — and the four voices mix into
   * a dedicated heads bus that lands in the master chain.
   */
  readonly headLanes = new HeadLanes({
    ctx: () => this.ctx,
    destination: () => this.headsBus,
    sourceBuffer: () => {
      const s = this.headLanes.source;
      return s == null ? null : (this.tracks[s]?.buffer ?? null);
    },
    reversedSource: () => {
      const s = this.headLanes.source;
      const t = s == null ? null : this.tracks[s];
      if (!t?.buffer || !this.ctx) return null;
      if (!t.reversed) t.reversed = reverseBuffer(this.ctx, t.buffer);
      return t.reversed;
    },
    barSeconds: () => this.barSeconds(),
    songPosition: () => this.position(),
  });

  /**
   * Most recently targeted track — the Heads source when Heads is entered.
   * Updated by any track-scoped command while Heads is NOT active.
   */
  lastTargetedTrack = 0;


  headsRms(): number {
    return this.headLanes.rms();
  }

  /**
   * The ONE gate that decides whether the normal four-stem mix reaches the
   * master. Heads and the stem mix are UNCORRELATED sources, so the handoff is
   * equal-power (sin/cos), not complementary-linear.
   */
  private crossfadeBuses(toHeads: boolean, seconds = 0.04) {
    const ctx = this.ctx;
    const n = this.normalBus;
    const h = this.headsBus;
    if (!ctx || !n || !h) return;
    const at = ctx.currentTime;
    const N = 64;
    const up = new Float32Array(N);
    const down = new Float32Array(N);
    for (let i = 0; i < N; i++) {
      const x = (i / (N - 1)) * (Math.PI / 2);
      up[i] = Math.sin(x);
      down[i] = Math.cos(x);
    }
    const apply = (p: AudioParam, curve: Float32Array) => {
      try {
        p.cancelScheduledValues(at);
        p.setValueAtTime(p.value, at);
        p.setValueCurveAtTime(curve, at, seconds);
      } catch {
        p.value = curve[curve.length - 1]!;
      }
    };
    apply(n.gain, toHeads ? down : up);
    apply(h.gain, toHeads ? up : down);
    this.busGate = { normal: toHeads ? 0 : 1, heads: toHeads ? 1 : 0 };
    // Hard-pin the endpoint so float curve tails can never leave a residual
    // open gate: after the fade the closed bus is EXACTLY 0.
    setTimeout(() => {
      if (this.ctx !== ctx) return;
      const closing = toHeads ? n.gain : h.gain;
      const opening = toHeads ? h.gain : n.gain;
      try {
        closing.cancelScheduledValues(ctx.currentTime);
        closing.setValueAtTime(0, ctx.currentTime);
        opening.cancelScheduledValues(ctx.currentTime);
        opening.setValueAtTime(1, ctx.currentTime);
      } catch {
        /* noop */
      }
    }, Math.ceil(seconds * 1000) + 20);
  }

  /** Post-gate RMS of the normal four-stem bus (what it gives the master). */
  normalBusRms(): number {
    return tapRms(this.normalTap);
  }

  /** Post-gate RMS of the heads bus. */
  headsBusRms(): number {
    return tapRms(this.headsTap);
  }

  /**
   * Gate values — proof of which bus is open.
   *
   * `normal`/`heads` are the SCHEDULED gate the engine committed to. The live
   * AudioParam readings are reported separately because Chrome stops updating
   * `AudioParam.value` on a node with no live inputs (an idle heads bus keeps
   * reporting its last rendered value even though its automation ran), which
   * makes the raw param a misleading isolation witness on its own.
   */
  busGains(): { normal: number; heads: number; liveNormal: number; liveHeads: number } {
    return {
      normal: this.busGate.normal,
      heads: this.busGate.heads,
      liveNormal: this.normalBus?.gain.value ?? 0,
      liveHeads: this.headsBus?.gain.value ?? 0,
    };
  }

  /** The gate the engine last committed to, independent of param rendering. */
  private busGate = { normal: 1, heads: 0 };


  /** Independent read position (seconds) of each of the four lane heads. */
  headPositions(): number[] {
    return [0, 1, 2, 3].map((i) => this.headLanes.position(i));
  }

  /** Absolute source-frame read position of each of the four heads. */
  headReadFrames(): number[] {
    const sr = this.ctx?.sampleRate ?? 48000;
    return [0, 1, 2, 3].map((i) => this.headLanes.position(i) * sr);
  }

  /** Mute mask of the non-source tracks, restored verbatim on heads exit. */
  private preHeadsMutes: boolean[] | null = null;

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
    // The heads bus itself is PERMANENT (built in buildGraph) — only voices
    // die. Nulling the bus here is what previously left orphaned voices with
    // no gate to close, so the bus gain stays as the single audible authority.
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

  /**
   * Snapshot of the NORMAL song mixer taken when Heads is entered: mutes,
   * solos, reverse flags, loop windows and fader levels. Heads is a temporary
   * performance layer, so everything here is restored verbatim on exit.
   * The hidden song timeline is deliberately NOT part of it — it keeps
   * advancing while the transport plays, and the stems rejoin wherever it has
   * got to.
   */
  private headsEntrySnapshot:
    | { muted: boolean; soloed: boolean; level: number; loop: TrackRuntime["loop"] }[]
    | null = null;

  enterHeadsMode(): { ok: boolean; detail: string } {
    if (!this.ctx) return { ok: false, detail: "audio not unlocked" };
    if (this.heads.active) return { ok: true, detail: "heads already active" };
    // ONE source stem for all four heads: the most recently targeted track if
    // it is decoded, otherwise the first decoded stem.
    const preferred = this.tracks[this.lastTargetedTrack]?.buffer ? this.lastTargetedTrack : null;
    const source = preferred ?? this.tracks.findIndex((t) => t.buffer != null);
    if (source < 0) return { ok: false, detail: "heads rejected — no decoded lane to read" };
    const sourceName = (this.tracks[source]?.name ?? `stem ${source + 1}`).replace(/\.[a-z0-9]+$/i, "");
    const r = this.headLanes.enter({ source, sourceName, playing: this.requestedPlaying });
    if (!r.ok) return r;


    this.headsEntrySnapshot = this.tracks.map((t) => ({
      muted: t.muted,
      soloed: t.soloed,
      level: t.level,
      loop: { ...t.loop },
    }));
    // The reducer-visible flag only; geometry belongs to HeadLanes now. There
    // is no heads SOURCE any more: head N is lane N by construction.
    this.heads = { ...emptyHeads(), active: true, engine: null, enteredAtFrame: Math.round(this.ctx.currentTime * this.ctx.sampleRate) };
    this.preHeadsMutes = null;
    this.applyAudibilityAll();
    // Defect 1: the normal four-stem bus is GATED SHUT for the whole of heads
    // mode. Its sources keep running (the hidden song timeline must not stop),
    // they simply contribute nothing to the master.
    this.crossfadeBuses(true);
    return r;
  }

  exitHeadsMode(): { ok: boolean; detail: string } {
    if (!this.heads.active) return { ok: true, detail: "heads already off" };
    const r = this.headLanes.exit();
    this.heads = exitHeads(this.heads);
    // Restore the entry mixer exactly. Anything the musician did to a LANE
    // while Heads was open (reverse, loop capture, resize) was a heads-only
    // performance and is discarded here.
    const snap = this.headsEntrySnapshot;
    this.headsEntrySnapshot = null;
    if (snap) {
      for (let i = 0; i < this.tracks.length; i++) {
        const t = this.tracks[i]!;
        const s = snap[i]!;
        t.muted = s.muted;
        t.soloed = s.soloed;
        t.level = s.level;
        if (t.loop.reverse !== s.loop.reverse) {
          this.execute({
            id: Date.now() + i,
            t: Date.now(),
            type: "tape.reverse",
            payload: { track: i, on: s.loop.reverse },
          } as AudioCommand);
        }
        t.loop = { ...s.loop };
        if (this.ctx) this.setGain(t.gain.gain, s.level);
      }
    }
    // Stems rejoin the hidden timeline where it now is, through the normal
    // click-free audibility ramp.
    this.applyAudibilityAll();
    // Re-open the normal bus; heads voices are already stopped and their bus
    // closes on the same equal-power curve.
    this.crossfadeBuses(false);
    return r;
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
    const rate = Math.min(HEAD_SCRUB_MAX_RATE, Math.abs(deltaFrames) / Math.max(1e-4, dtS) / sr);
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
      /** Release evidence: one record per stem for the most recent handoff. */
      handoff: this.lastScrubHandoff,

    };
  }

  /** Why the most recent shuttle attempt was refused, for the diagnostic record. */
  lastScrubRejection: string | null = null;

  /**
   * Every scrub grain node that is currently alive. Registered on creation and
   * removed by the node's own `onended`, so `scrubPathCount()` is a measurement
   * of the graph rather than an assertion about it.
   */
  readonly liveScrubPaths = new Set<AudioBufferSourceNode>();

  /** Active scrub audio paths right now (0 once a release has settled). */
  scrubPathCount(): number {
    return this.liveScrubPaths.size;
  }

  /** Live AudioParam value of each stem fader — what is actually audible. */


  trackGainValues(): number[] {
    return this.tracks.map((t) => t.gain.gain.value);
  }

  /** Per-stem post-FX RMS — proves a stem's own path is sounding. */
  trackRms(): number[] {
    return this.tracks.map((t) => {
      const a = t.analyser;
      const buf = new Float32Array(a.fftSize);
      a.getFloatTimeDomainData(buf);
      let sum = 0;
      for (let i = 0; i < buf.length; i++) sum += buf[i]! * buf[i]!;
      return Math.sqrt(sum / buf.length);
    });
  }

  /** Normal (non-scrub) playback paths per stem — 1 per loaded, playing stem. */

  playbackPathCounts(): number[] {
    return this.tracks.map((t) => t.sources.length);
  }



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
      g.connect(t.stemGate);
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
      // Measured, not asserted: every sounding scrub path is registered here
      // and only removed when the node actually ends, so a harness can read a
      // true "active scrub path" count after release.
      this.liveScrubPaths.add(node);
      node.onended = () => {
        const cur = this.globalScrub;
        if (cur) cur.live[i] = (cur.live[i] ?? []).filter((r) => r !== rec);
        this.liveScrubPaths.delete(node);

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
  // ------------------------------------------------- per-lane audible scrub
  /**
   * ONE lane shuttles while the other three keep playing normally. The lane is
   * a head in Heads mode and a tape track otherwise — the same universal lane
   * meaning as `lane.reverse`. Grains are rendered through the lane's own input
   * node, so its fader, mute, solo and FX all stay in the path; on release the
   * landing frame is stored as the lane's scrub candidate so a following
   * `loop.capture` snaps to the bar the performer actually parked on.
   */
  private laneScrub: ({
    dir: 1 | -1;
    pos: number;
    startPos: number;
    posCtxTime: number;
    timer: ReturnType<typeof setInterval> | null;
    last: number;
    lastGrainAt: number;
    grains: number;
    gen: number;
    live: { node: AudioBufferSourceNode; gain: GainNode; at: number; gen: number }[];
  } | null)[] = [null, null, null, null];

  /** Live normalized read position of a head shuttle, per head. */
  private headLaneScrubNorm: ((() => number) | null)[] = [null, null, null, null];
  private headLaneTimer: (ReturnType<typeof setInterval> | null)[] = [null, null, null, null];

  /** Cycle length of the heads engine in seconds (0 when there is no cycle). */
  private headCycleSeconds(): number {
    const sr = this.ctx?.sampleRate ?? 48000;
    return this.heads.cycleFrames > 0 ? this.heads.cycleFrames / sr : 0;
  }

  /** Silence one lane's normal playback without touching the other three. */
  private stopTrackSources(lane: number) {
    const t = this.tracks[lane];
    if (!t) return;
    for (const src of t.sources) {
      try {
        src.node.stop();
        src.node.disconnect();
      } catch {
        /* already stopped */
      }
    }
    t.sources = [];
  }

  /** Rejoin one lane to the SHARED playhead — it never gets its own clock. */
  private restartTrack(lane: number, offset: number) {
    const t = this.tracks[lane];
    if (!t?.buffer || !this.ctx) return;
    this.stopTrackSources(lane);
    const bounds = t.loop.enabled ? this.loopBounds(t) : null;
    const from = bounds ? Math.max(offset, bounds.start) : offset;
    this.spawn(t, this.ctx.currentTime + LOOKAHEAD_S, from, false);
  }

  laneScrubState(lane: number) {
    const ls = this.laneScrub[lane];
    if (!ls) return null;
    return { dir: ls.dir, pos: ls.pos, startPos: ls.startPos, grains: ls.grains, live: ls.live.length };
  }

  private laneScrubTick(lane: number) {
    const ls = this.laneScrub[lane];
    const ctx = this.ctx;
    const t = this.tracks[lane];
    if (!ls || !ctx || !t) return;
    const nowMs = typeof performance !== "undefined" ? performance.now() : Date.now();
    const dt = Math.min(0.2, Math.max(0, (nowMs - ls.last) / 1000));
    ls.last = nowMs;
    if (dt <= 0) return;
    const rate = LANE_SCRUB_RATE;
    const before = ls.pos;
    ls.pos = Math.min(this.duration, Math.max(0, ls.pos + ls.dir * rate * dt));
    ls.posCtxTime = ctx.currentTime;
    if (ls.pos === before) return;
    const src = t.buffer ?? this.scrubPcm(t);
    if (!src) return;
    const backwards = ls.dir < 0;
    if (backwards && !t.reversed) t.reversed = reverseBuffer(ctx, src);
    const buffer = backwards ? t.reversed : src;
    if (!buffer) return;
    const now = ctx.currentTime;
    const dur = Math.min(0.12, Math.max(0.03, dt * 1.6));
    const at = Math.max(now, ls.lastGrainAt);
    const offsetS = backwards ? buffer.duration - ls.pos : ls.pos;
    const g = ctx.createGain();
    g.connect(t.stemGate);
    const tap = this.scrubTap(lane);
    if (tap) g.connect(tap);
    const node = ctx.createBufferSource();
    node.buffer = buffer;
    node.playbackRate.value = rate;
    node.connect(g);
    const fade = Math.min(0.006, dur / 3);
    g.gain.setValueAtTime(0, at);
    g.gain.linearRampToValueAtTime(GLOBAL_SCRUB_LEVEL, at + fade);
    g.gain.setValueAtTime(GLOBAL_SCRUB_LEVEL, at + dur - fade);
    g.gain.linearRampToValueAtTime(0, at + dur);
    node.start(at, Math.min(Math.max(0, offsetS), Math.max(0, buffer.duration - 1e-3)), dur * rate);
    ls.lastGrainAt = at + dur * 0.8;
    ls.grains++;
    const rec = { node, gain: g, at, gen: ls.gen };
    ls.live.push(rec);
    this.liveScrubPaths.add(node);
    node.onended = () => {
      const cur = this.laneScrub[lane];
      if (cur) cur.live = cur.live.filter((r) => r !== rec);
      this.liveScrubPaths.delete(node);
      try {
        node.disconnect();
        g.disconnect();
      } catch {
        /* noop */
      }
    };
    this.sampleScrubTaps();
  }

  beginLaneScrub(lane: number, dir: 1 | -1): { ok: boolean; detail: string } {
    const ctx = this.ctx;
    if (!ctx) return { ok: false, detail: "audio not unlocked" };
    if (ctx.state !== "running") void ctx.resume();
    if (this.duration === 0) return { ok: false, detail: "no stems decoded" };
    const t = this.tracks[lane];
    if (!t) return { ok: false, detail: `no lane ${lane + 1}` };
    // Heads mode: the head lane scrubs at its own 1.5× ceiling inside the
    // heads engine; the tape lanes are untouched.
    if (this.heads.active) {
      // Heads v2: the lane IS an independent head, so the shuttle drives that
      // head's OWN normalized read position through the heads engine at the
      // same 1.5x ceiling. The tape lanes and the song playhead never move.
      const dur = this.headLanes.duration(lane);
      if (dur <= 0) return { ok: false, detail: `head ${lane + 1} has no source` };
      this.headLanes.beginScrub(lane);
      let norm = this.headLanes.position(lane) / dur;
      const step = (LANE_SCRUB_RATE * (GLOBAL_SCRUB_INTERVAL_MS / 1000)) / dur;
      const timer = setInterval(() => {
        norm = Math.min(1, Math.max(0, norm + dir * step));
        this.headLanes.previewScrub(lane, norm);
      }, GLOBAL_SCRUB_INTERVAL_MS);
      this.headLaneTimer[lane] = timer;
      this.headLaneScrubNorm[lane] = () => norm;
      return { ok: true, detail: `head ${lane + 1} shuttle ${dir > 0 ? "forward" : "backward"} at ${LANE_SCRUB_RATE}x of its own source` };
    }
    const existing = this.laneScrub[lane];
    if (existing) {
      existing.dir = dir;
      return { ok: true, detail: `lane ${lane + 1} shuttle → ${dir > 0 ? "forward" : "backward"}` };
    }
    const now = ctx.currentTime;
    const pos = this.position();
    this.laneScrub[lane] = {
      dir,
      pos,
      startPos: pos,
      posCtxTime: now,
      timer: null,
      last: typeof performance !== "undefined" ? performance.now() : Date.now(),
      lastGrainAt: now,
      grains: 0,
      gen: 1,
      live: [],
    };
    // The lane's own normal playback is silenced for the duration of the
    // shuttle; the other three lanes keep running on the shared timeline.
    this.stopTrackSources(lane);
    this.laneScrub[lane]!.timer = setInterval(() => this.laneScrubTick(lane), GLOBAL_SCRUB_INTERVAL_MS);
    return {
      ok: true,
      detail: `lane ${lane + 1} shuttle ${dir > 0 ? "forward" : "backward"} at ${LANE_SCRUB_RATE}× from ${pos.toFixed(3)}s — the other three lanes keep playing`,
    };
  }

  endLaneScrub(lane: number): { ok: boolean; detail: string } {
    if (this.heads.active) {
      const timer = this.headLaneTimer[lane];
      if (timer) clearInterval(timer);
      this.headLaneTimer[lane] = null;
      const norm = this.headLaneScrubNorm[lane]?.() ?? 0;
      this.headLaneScrubNorm[lane] = null;
      return this.headLanes.endScrub(lane, norm);
    }
    const ls = this.laneScrub[lane];
    const ctx = this.ctx;
    if (!ls || !ctx) return { ok: false, detail: `no lane ${lane + 1} scrub active` };
    if (ls.timer) clearInterval(ls.timer);
    ls.timer = null;
    const sr = ctx.sampleRate;
    const quantum = 128 / sr;
    const handoff = ctx.currentTime + Math.max(2 * quantum, SCRUB_HANDOFF_MIN_S);
    const dt = Math.max(0, handoff - ls.posCtxTime);
    const landing = Math.min(this.duration, Math.max(0, ls.pos + ls.dir * LANE_SCRUB_RATE * dt));
    // Invalidate the scheduler generation FIRST: no further grains, ever.
    ls.gen++;
    for (const rec of ls.live) {
      try {
        rec.gain.gain.cancelScheduledValues(handoff);
        if (rec.at >= handoff) {
          rec.gain.gain.setValueAtTime(0, handoff);
          rec.node.stop(handoff);
        } else {
          rec.gain.gain.setValueAtTime(complementary(0).a * GLOBAL_SCRUB_LEVEL, handoff);
          rec.gain.gain.linearRampToValueAtTime(0, handoff + SCRUB_HANDOFF_FADE_S);
          rec.node.stop(handoff + SCRUB_HANDOFF_FADE_S);
        }
      } catch {
        /* already stopped */
      }
    }
    ls.live = [];
    this.laneScrub[lane] = null;
    // The landing frame becomes the lane's loop-capture candidate.
    this.setScrubCandidate(lane, landing);
    // Rejoin the shared transport: the lane is NOT given its own playhead.
    if (this.requestedPlaying) this.restartTrack(lane, this.position());
    return {
      ok: true,
      detail: `lane ${lane + 1} landed at ${landing.toFixed(3)}s (frame ${Math.round(landing * sr)}) after ${ls.grains} grains — candidate stored for loop capture`,
    };
  }

  /**
   * FUNCTION + fader = positional per-lane scrub.
   *
   * Unlike the rocker shuttle (constant rate, signed direction) this is a
   * *travelling read pointer driven by the finger*: the fader's normalized
   * value maps onto song time, every rAF-coalesced move renders an audible
   * grain in the direction the finger travelled, and the release stores the
   * exact landing second as that lane's loop-capture candidate — which the
   * following Track double-tap consumes to place a one-bar loop.
   */
  private laneFaderScrub: ({
    pos: number;
    lastNorm: number;
    lastTs: number;
    lastGrainAt: number;
    grains: number;
    live: { node: AudioBufferSourceNode; gain: GainNode; at: number }[];
  } | null)[] = [null, null, null, null];

  laneFaderScrubState(lane: number) {
    const s = this.laneFaderScrub[lane];
    return s ? { pos: s.pos, grains: s.grains, live: s.live.length } : null;
  }

  beginLaneFaderScrub(lane: number, normalized: number, timestamp: number): { ok: boolean; detail: string } {
    const ctx = this.ctx;
    if (!ctx) return { ok: false, detail: "audio not unlocked" };
    if (ctx.state !== "running") void ctx.resume();
    if (this.duration === 0) return { ok: false, detail: "no stems decoded" };
    if (!this.tracks[lane]) return { ok: false, detail: `no lane ${lane + 1}` };
    if (this.laneFaderScrub[lane]) return { ok: true, detail: `lane ${lane + 1} fader scrub already live` };
    const pos = Math.min(this.duration, Math.max(0, normalized * this.duration));
    this.laneFaderScrub[lane] = {
      pos,
      lastNorm: normalized,
      lastTs: timestamp,
      lastGrainAt: ctx.currentTime,
      grains: 0,
      live: [],
    };
    // Only this lane goes quiet; the other three keep the shared timeline.
    this.stopTrackSources(lane);
    return { ok: true, detail: `lane ${lane + 1} fader scrub from ${pos.toFixed(3)}s` };
  }

  previewLaneFaderScrub(lane: number, normalized: number, timestamp: number): { ok: boolean; detail: string } {
    if (!this.laneFaderScrub[lane]) {
      // Keyboard lanes (F + Y/H, U/J, I/K, O/L) and mid-drag rebases never send
      // an explicit start; the first movement opens the gesture.
      const opened = this.beginLaneFaderScrub(lane, normalized, timestamp);
      if (!opened.ok) return opened;
    }
    const s = this.laneFaderScrub[lane];
    const ctx = this.ctx;
    const t = this.tracks[lane];
    if (!s || !ctx || !t) return { ok: false, detail: `no lane ${lane + 1} fader scrub` };
    const dt = Math.min(0.25, Math.max(1e-3, (timestamp - s.lastTs) / 1000));
    const target = Math.min(this.duration, Math.max(0, normalized * this.duration));
    const delta = target - s.pos;
    s.lastNorm = normalized;
    s.lastTs = timestamp;
    s.pos = target;
    if (Math.abs(delta) < 1e-4) return { ok: true, detail: "stationary" };
    const backwards = delta < 0;
    const rate = Math.min(MAX_FADER_SCRUB_RATE, Math.max(0.25, Math.abs(delta) / dt));
    const src = t.buffer ?? this.scrubPcm(t);
    if (!src) return { ok: false, detail: "no pcm" };
    if (backwards && !t.reversed) t.reversed = reverseBuffer(ctx, src);
    const buffer = backwards ? t.reversed : src;
    if (!buffer) return { ok: false, detail: "no pcm" };
    const now = ctx.currentTime;
    const dur = Math.min(0.12, Math.max(0.03, dt * 1.6));
    const at = Math.max(now, s.lastGrainAt);
    const offsetS = backwards ? buffer.duration - target : target;
    const g = ctx.createGain();
    g.connect(t.stemGate);
    const tap = this.scrubTap(lane);
    if (tap) g.connect(tap);
    const node = ctx.createBufferSource();
    node.buffer = buffer;
    node.playbackRate.value = rate;
    node.connect(g);
    const fade = Math.min(0.006, dur / 3);
    g.gain.setValueAtTime(0, at);
    g.gain.linearRampToValueAtTime(GLOBAL_SCRUB_LEVEL, at + fade);
    g.gain.setValueAtTime(GLOBAL_SCRUB_LEVEL, at + dur - fade);
    g.gain.linearRampToValueAtTime(0, at + dur);
    node.start(at, Math.min(Math.max(0, offsetS), Math.max(0, buffer.duration - 1e-3)), dur * rate);
    s.lastGrainAt = at + dur * 0.8;
    s.grains++;
    const rec = { node, gain: g, at };
    s.live.push(rec);
    this.liveScrubPaths.add(node);
    node.onended = () => {
      const cur = this.laneFaderScrub[lane];
      if (cur) cur.live = cur.live.filter((r) => r !== rec);
      this.liveScrubPaths.delete(node);
      try {
        node.disconnect();
        g.disconnect();
      } catch {
        /* noop */
      }
    };
    this.sampleScrubTaps();
    return { ok: true, detail: `lane ${lane + 1} scrub → ${target.toFixed(3)}s at ${rate.toFixed(2)}×` };
  }

  endLaneFaderScrub(lane: number, normalized: number, cancelled = false): { ok: boolean; detail: string } {
    const s = this.laneFaderScrub[lane];
    const ctx = this.ctx;
    if (!s || !ctx) return { ok: false, detail: `no lane ${lane + 1} fader scrub` };
    const landing = Math.min(this.duration, Math.max(0, normalized * this.duration));
    const quantum = 128 / ctx.sampleRate;
    const handoff = ctx.currentTime + Math.max(2 * quantum, SCRUB_HANDOFF_MIN_S);
    for (const rec of s.live) {
      try {
        rec.gain.gain.cancelScheduledValues(handoff);
        if (rec.at >= handoff) {
          rec.gain.gain.setValueAtTime(0, handoff);
          rec.node.stop(handoff);
        } else {
          rec.gain.gain.setValueAtTime(complementary(0).a * GLOBAL_SCRUB_LEVEL, handoff);
          rec.gain.gain.linearRampToValueAtTime(0, handoff + SCRUB_HANDOFF_FADE_S);
          rec.node.stop(handoff + SCRUB_HANDOFF_FADE_S);
        }
      } catch {
        /* already stopped */
      }
    }
    const grains = s.grains;
    s.live = [];
    this.laneFaderScrub[lane] = null;
    if (!cancelled) this.setScrubCandidate(lane, landing);
    if (this.requestedPlaying) this.restartTrack(lane, this.position());
    return {
      ok: true,
      detail: cancelled
        ? `lane ${lane + 1} fader scrub cancelled after ${grains} grains`
        : `lane ${lane + 1} parked at ${landing.toFixed(3)}s (frame ${Math.round(landing * ctx.sampleRate)}) after ${grains} grains — double-tap Track ${lane + 1} to capture one bar there`,
    };
  }



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
    // Remember the most recently targeted stem — it becomes the Heads source.
    if (!this.heads.active) {
      const targeted = (p as Record<string, unknown> | undefined)?.["track"];
      if (typeof targeted === "number" && targeted >= 0 && targeted < 4) this.lastTargetedTrack = targeted;
    }
    if (!this.ctx && (cmd.type.startsWith("track.") || cmd.type.startsWith("loop.") || cmd.type.startsWith("tape."))) {
      return this.ack(cmd, "rejected", "audio not unlocked — enable audio, then repeat the gesture");
    }
    try {
      switch (cmd.type) {
        // ---- Stem Instrument Mode ------------------------------------------
        case "cue.event":
        case "cue.panic": {
          if (!this.ctx) return this.ack(cmd, "rejected", "audio not unlocked — connect MIDI to unlock, then repeat");
          const mask = String(p["tracksHeld"] ?? "");
          const ev: StemMidiEvent = {
            kind:
              cmd.type === "cue.panic"
                ? "allNotesOff"
                : ((String(p["kind"] ?? "noteOn") === "noteOff" ? "noteOff" : "noteOn") as StemMidiEvent["kind"]),
            note: Number(p["note"] ?? 0),
            velocity: Number(p["velocity"] ?? 0),
            channel: Number(p["channel"] ?? 0),
            timestampMs: Number(p["timestampMs"] ?? cmd.t),
            source: String(p["source"] ?? "test") as MidiTransport,
            deviceId: String(p["deviceId"] ?? ""),
            deviceName: String(p["deviceName"] ?? ""),
          };
          const qualifiers: QualifierSnapshot = {
            functionHeld: Boolean(p["functionHeld"]),
            tracksHeld: [0, 1, 2, 3].filter((i) => mask[i] === "1") as CueLane[],
          };
          const { action, detail } = this.handleMidiCue(ev, qualifiers);
          const bad = action.type === "learn.reject" || action.type === "cue.reject" || action.type === "learn.discard";
          return this.ack(cmd, bad ? "rejected" : "completed", detail);
        }
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
          const relocated = bounds && enabled ? this.relocateLane(t, bounds.start) : false;
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
          const relocated = bounds ? this.relocateLane(t, bounds.start) : false;
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
          // The song clock is NEVER touched by a lane flip: reverse-on reads
          // backwards from the current song position, reverse-off rejoins the
          // song position the hidden timeline has reached meanwhile. With a
          // loop armed, only that loop reverses (mirrored loop window).
          const pos = this.position();
          t.loop = { ...t.loop, reverse: on };
          if (!on) t.reversed = null;
          const relocated = this.respawnLane(t, pos);
          const bounds = t.loop.enabled ? this.loopBounds(t) : null;
          return this.ack(
            cmd,
            "completed",
            `track ${id + 1} reverse ${on ? "on" : "off"} at song ${pos.toFixed(3)}s` +
              `${bounds ? ` · loop only [${bounds.start.toFixed(3)}, ${bounds.end.toFixed(3)}]` : " · song timeline keeps running"}` +
              `${relocated ? " · dual-source crossfade" : ""}`,
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
            this.stopAllCues("song loaded");
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

        // ---- Phase 6: grid and export -------------------------------------
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
        case "heads.level": {
          if (!this.heads.active) return this.ack(cmd, "rejected", "heads mode is not active");
          const i = Number(p["head"]);
          this.headLanes.setLevel(i, Number(p["level"]));
          return this.ack(cmd, "completed", `head ${i + 1} level → ${Number(p["level"]).toFixed(3)}`);
        }
        case "heads.mute": {
          const r = this.headLanes.toggleMute(Number(p["head"]));
          return this.ack(cmd, r.ok ? "completed" : "rejected", r.detail);
        }
        case "heads.play.hold": {
          const r = this.headLanes.setHeld(String(p["mask"] ?? ""));
          return this.ack(cmd, r.ok ? "completed" : "rejected", r.detail);
        }
        case "heads.latch": {
          const r = this.headLanes.toggleLatch(Number(p["head"]));
          return this.ack(cmd, r.ok ? "completed" : "rejected", r.detail);
        }
        case "heads.loop.capture": {
          const r = this.headLanes.captureLoop(Number(p["head"]), Math.max(0.25, Number(p["bars"] ?? 1)));
          return this.ack(cmd, r.ok ? "completed" : "rejected", r.detail);
        }
        case "heads.loop.resize": {
          const r = this.headLanes.resizeLoop(Number(p["head"]), Number(p["direction"]) < 0 ? -1 : 1);
          return this.ack(cmd, r.ok ? "completed" : "rejected", r.detail);
        }
        case "heads.reverse": {
          const r = this.headLanes.toggleReverse(Number(p["head"]));
          return this.ack(cmd, r.ok ? "completed" : "rejected", r.detail);
        }
        // ---- universal lane layer ------------------------------------------
        // ONE implementation for Tape, Heads and the FX overlay. In heads mode
        // the lane is a head; otherwise it is the tape track. `heads.reverse`
        // no longer exists as a command type.
        case "lane.reverse": {
          const i = Number(p["lane"]);
          if (this.heads.active) {
            // Heads v2: the lane IS an independent head, so the flip happens on
            // that head's own clock and the tape lane is left untouched.
            const r = this.headLanes.toggleReverse(i);
            return this.ack(cmd, r.ok ? "completed" : "rejected", r.detail);
          }
          return this.execute({ ...cmd, type: "tape.reverse", payload: { track: i, on: Boolean(p["reverse"]) } });
        }
        case "lane.scrub.start": {
          const i = Number(p["lane"]);
          const dir: 1 | -1 = Number(p["direction"] ?? 1) < 0 ? -1 : 1;
          const r = this.beginLaneScrub(i, dir);
          return this.ack(cmd, r.ok ? "accepted" : "rejected", r.detail);
        }
        case "lane.scrub.end": {
          const i = Number(p["lane"]);
          const r = this.endLaneScrub(i);
          return this.ack(cmd, r.ok ? "completed" : "rejected", r.detail);
        }
        case "lane.audition": {
          const mask = String(p["mask"] ?? "");
          this.setAudition(mask);
          return this.ack(
            cmd,
            "completed",
            mask.includes("1")
              ? `momentary audition ${mask} — mutes and latched solo untouched`
              : "audition released — prior mix restored exactly",
          );
        }
        case "loop.capture":
        case "loop.release":
        case "loop.resize": {
          const i = Number(p["lane"]) as TrackId;
          const t = this.tracks[i];
          if (!t) return this.ack(cmd, "rejected", `no lane ${i}`);
          const bars = Math.max(0.25, Number(p["bars"] ?? 1));
          if (cmd.type === "loop.release") {
            const r = this.scheduleLoopRelease(i, t);
            return this.ack(cmd, r.ok ? "completed" : "rejected", r.detail);
          }
          // Length and start both come from the ANALYSED song grid: bars are
          // real bars of the detected tempo, and a capture begins on the bar
          // containing the accepted double-tap (or the stored scrub landing).
          const grid = this.songGrid;
          const barS = this.barSeconds();
          const lengthS = bars * barS;
          const dur = this.duration || lengthS;
          const here = Math.max(0, this.position());
          let start: number;
          let origin: string;
          if (cmd.type === "loop.resize") {
            // Resize keeps the existing loop start fixed.
            start = Math.max(0, resolveLoop(t.loop, dur).start);
            origin = "existing loop start held";
          } else if (this.scrubCandidate[i] != null) {
            start = Math.max(0, this.scrubCandidate[i]!);
            origin = `scrub landing ${start.toFixed(3)}s`;
            this.scrubCandidate[i] = null; // consumed, never persisted
          } else if (grid) {
            start = Math.max(0, barStartAt(grid, here));
            origin = `grid bar containing ${here.toFixed(3)}s`;
          } else {
            start = here;
            origin = "no grid — captured at the playhead";
          }
          const res = this.execute({
            ...cmd,
            type: "loop.set",
            payload: { track: i, enabled: true, start: start / dur, end: Math.min(1, (start + lengthS) / dur) },
          });
          const activateAt = grid ? nextBarAfter(grid, here) : here;
          return this.ack(
            cmd,
            res.status,
            `lane ${i + 1} loop ${bars} bar (${lengthS.toFixed(3)}s @ ${(grid?.bpm ?? this.baseBpm).toFixed(2)} BPM, ${origin}, active at ${activateAt.toFixed(3)}s) — ${res.detail}`,
          );
        }

        case "heads.scrub": {
          const r = this.headLanes.endScrub(Number(p["head"]), Number(p["position"]));
          return this.ack(cmd, r.ok ? "completed" : "rejected", r.detail);
        }

        // ---- GLOBAL loop: one bar-locked window shared by all four stems ----
        // It is a single window applied to every lane, not four lane loops:
        // one start, one length, so the stems can never drift apart. Lanes that
        // already own a per-lane loop keep it — their audible pointer wins and
        // the global window only supplies the hidden song target they rejoin.
        case "loop.global.start":
        case "loop.global.resize":
        case "loop.global.move": {
          const division = Math.max(1, Number(p["division"] ?? this.globalLoop?.division ?? 1));
          const dur = this.duration;
          if (!dur) return this.ack(cmd, "rejected", "no audio loaded");
          const lengthS = this.barSeconds() / division;
          const grid = this.songGrid;
          const here = Math.max(0, this.position());
          let start: number;
          if (cmd.type === "loop.global.move" && this.globalLoop) {
            const steps = Number(p["steps"] ?? 0);
            start = this.globalLoop.start + steps * lengthS;
          } else if (cmd.type === "loop.global.resize" && this.globalLoop) {
            start = this.globalLoop.start;
          } else {
            start = grid ? Math.max(0, barStartAt(grid, here)) : here;
          }
          start = Math.max(0, Math.min(Math.max(0, dur - lengthS), start));
          this.globalLoop = { start, lengthS, division };
          const details: string[] = [];
          for (let i = 0 as TrackId; i < 4; i = (i + 1) as TrackId) {
            if (!this.tracks[i]) continue;
            const res = this.execute({
              ...cmd,
              type: "loop.set",
              payload: { track: i, enabled: true, start: start / dur, end: Math.min(1, (start + lengthS) / dur) },
            });
            details.push(`lane ${i + 1} ${res.status}`);
          }
          return this.ack(
            cmd,
            "completed",
            `global loop 1/${division} bar @ ${start.toFixed(3)}s (${lengthS.toFixed(3)}s) — ${details.join(", ")}`,
          );
        }
        case "loop.global.release": {
          this.globalLoop = null;
          const r = this.releaseGlobalLoop();
          return this.ack(cmd, r.ok ? "completed" : "rejected", r.detail);
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
      songGrid: this.songGrid,
      gridDetail: this.gridAnalysisDetail,
      scrubCandidates: [...this.scrubCandidate],
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
      headLanes: this.headLanes.snapshot(),
      headsSummary: headsSummary(this.heads),
      headsSource:
        this.headLanes.active && this.headLanes.source != null
          ? { index: this.headLanes.source, name: this.headLanes.sourceName ?? `stem ${this.headLanes.source + 1}` }
          : null,
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
    this.stopAllCues("engine disposed");
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
