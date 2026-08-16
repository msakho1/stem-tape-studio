import { COMMAND_LOG_LIMIT, makeCommand, type AudioCommand, type AudioCommandType } from "@/audio/commands";
import type { Control, TrackIndex } from "@/device/geometry";
import { type Gesture } from "@/input/gestures";

/**
 * How long a bare FUNCTION tap keeps active-track selection armed. Long enough
 * to look down at the four Track buttons, short enough that the next unrelated
 * Track tap is a normal mute and not a stray selection.
 */
export const TRACK_SELECT_ARM_MS = 1200;
import { V26_ROW_BY_ID } from "@/machine/v26map";
import type { PerfIntent } from "@/machine/chordArbiter";
import {
  FX_FAMILIES,
  clearLatches,
  cycleBankAlgorithm,
  fxStateOf,
  fxTargetOf,
  initialStemPerformance,
  nudgeBankMacro,
  patchSlot,
  selectBank,
  selectStem,
  setBankMomentary,
  toggleBankLatch,
  toggleLink,
  toggleSolo,
  type FxFamily,
  type StemPerformanceState,
} from "@/machine/stemPerformance";
import { BANKS, algorithmDef, bankOfButton } from "@/machine/fx12";
import {
  DEFAULT_SCRUB_SPEED_INDEX,
  GLOBAL_SCRUB_SPEEDS,
  type ScrubSpeedIndex,
} from "@/audio/inertia";


export type LedPattern = "dark" | "faint" | "solid" | "pulse" | "blink" | "breathe" | "chase";

export type LedId =
  | "track-led-1"
  | "track-led-2"
  | "track-led-3"
  | "track-led-4"
  | "side-led-1"
  | "side-led-2"
  | "side-led-3"
  | "side-led-4"
  | "play-indicator"
  | "function-led-1"
  | "function-led-2";

export interface LedState {
  pattern: LedPattern;
  /** Why this pattern won arbitration — surfaced verbatim in diagnostics. */
  reason: string;
  priority: number;
}

export type LedFrame = Record<LedId, LedState>;

export type TrackContent = "empty" | "loaded" | "muted";

export interface TrackSlice {
  content: TrackContent;
  volume: number;
  stem: "vocals" | "drums" | "bass" | "instruments";
  /** Heads mode: per-head scrub position 0..1. */
  headPos: number;
  headReverse: boolean;
  headMuted: boolean;
  headLevel: number;
  /** Heads v2: triple-tap independent playback for this head. */
  headLatched: boolean;
  /** Heads v2: this head is repeating a captured loop. */
  headLoop: { active: boolean; bars: number };
  /** PLAY + Track latching solo (§2.1). Independent of momentary audition. */
  soloLatched: boolean;
  /**
   * Universal lane layer. `laneReverse` replaces the old heads-only
   * `heads.reverse`; one flag serves Tape, Heads and the FX overlay so the
   * three layers cannot diverge.
   */
  laneReverse: boolean;
  /** Dedicated one-bar (or resized) capture loop for this lane. */
  laneLoop: { active: boolean; bars: number };
}



export interface FiredRow {
  id: number;
  rowId: string;
  detail: string;
  t: number;
}

export interface SurfaceState {
  power: "off" | "on";
  playing: boolean;
  functionHeld: boolean;
  pressed: Control[];
  tracks: [TrackSlice, TrackSlice, TrackSlice, TrackSlice];
  activeTrack: TrackIndex;
  masterVolume: number;
  rocker: "rewind" | "center" | "forward";
  lastGesture: string | null;

  // ---- Tape Looper v2.6 machine ----
  speed: number;
  speedGlide: boolean;
  chopDiv: number;
  chopGlide: boolean;
  chopWindowOffset: number;
  window: { start: number; end: number; shift: number; reverse: boolean };
  filter: { mode: "off" | "lp" | "hp"; amount: number };
  loopMode: "fixed" | "variable";
  headsMode: boolean;
  /** Track the four heads are reading while heads mode is active. */
  headsSource: number | null;
  /**
   * GLOBAL one-bar loop — all four stems, bar-locked. Distinct from the per
   * lane loops: `active` while PLAY is physically held, `latched` once a
   * FUNCTION tap during the hold makes it survive the release. `division` is
   * the bar fraction set by FUNCTION + Volume ±.
   */
  globalLoop: { active: boolean; latched: boolean; division: 1 | 2 | 4 | 8 };
  /**
   * `performance.now()` of the FUNCTION tap that armed active-track selection,
   * or null. While armed, a Track gesture emits ONLY `stem.select` — no mute,
   * loop, audition or FX action may leak out of the selection.
   */
  trackSelectArmedAt: number | null;

  lights: "full" | "dim";
  song: number;
  bank: number;
  bankJumpArmed: boolean;
  grid: { bpm: number | null; rejected: boolean; source: "none" | "tapped" | "beatmatched" | "rounded" };
  fnTapTimes: number[];
  fnTapCount: number;
  /** FUNCTION hold has crossed the dedicated POWER threshold (not the 450 ms hold). */
  fnHoldReached: boolean;
  /** FUNCTION was used as a modifier during this hold — so it must NOT power-toggle. */
  fnModifierUsed: boolean;
  /**
   * Multi-tap transaction. Taps fire optimistically at ×1; when ×2 / ×3 arrives
   * the WHOLE machine is rolled back to the snapshot taken before ×1 and only
   * then does the higher-count row run. Nothing from an intermediate count is
   * allowed to survive (e.g. FN+PLAY ×3 must not keep the ×2 1.0× snap).
   */
  txn: { control: Control; count: number; snapshot: TxnSnapshot } | null;
  /** Per-song memory: loops, speed, chop, mutes, grid (v2.6 songs.memory). */
  songMemory: Record<number, TxnSnapshot>;
  /** v2.6 songs.length: to 8:00 · longer with the tape slowed. */
  maxTakeSeconds: number;

  /**
   * Ordered audio command stream (Phase 4). Append-only, monotonic ids, drained
   * by the AudioEngine on a watermark. Audio is NEVER inferred from snapshot
   * diffs: repeats, optimistic taps and rollbacks would be lost.
   */
  commands: AudioCommand[];

  /** Held global four-stem shuttle: +1 forward, -1 backward, 0 idle. */
  globalScrub: 0 | 1 | -1;
  /**
   * Stock-SP-1 addendum §3 — the shuttle speed is a PERSISTENT machine setting,
   * not a per-gesture value: it survives release and the next shuttle starts
   * there. Index into `GLOBAL_SCRUB_SPEEDS`.
   */
  scrubSpeed: ScrubSpeedIndex;
  /**
   * Addendum §4 — a FUNCTION tap during a shuttle latches it, so forward or
   * reverse shuttling continues after the keys come up. Direction may still be
   * flipped while latched; an explicit release ends it.
   */
  scrubLatched: boolean;
  /**
   * Addendum §5 — `performance.now()` of the last FX latch toggle. The LED
   * arbiter turns this into a short four-light confirmation flash.
   */
  fxFlashAt: number | null;
  /** Per-lane shuttle direction (FUNCTION + Track held + rocker). 0 = idle. */
  laneScrub: (0 | 1 | -1)[];

  /**
   * Lanes currently claimed by a Track-button CHORD audition. Pressing two or
   * three Track buttons together auditions those lanes as a chord; the taps
   * that arrive on release are consumed here so the chord never toggles mute.
   */
  auditionChord: number[];


  /** Phase 5C stem-performance layer (serializable, no audio objects). */
  perf: StemPerformanceState;

  fired: FiredRow[];
  coverage: Record<string, number>;
  note: string;
}

/** Everything a multi-tap transaction (and a song slot) has to remember. */
export interface TxnSnapshot {
  tracks: SurfaceState["tracks"];
  speed: number;
  chopDiv: number;
  chopWindowOffset: number;
  window: SurfaceState["window"];
  filter: SurfaceState["filter"];
  masterVolume: number;
  playing: boolean;
  headsMode: boolean;
  loopMode: SurfaceState["loopMode"];
  activeTrack: TrackIndex;
  grid: SurfaceState["grid"];
  lights: SurfaceState["lights"];
}

export function snapshotOf(s: SurfaceState): TxnSnapshot {
  return {
    tracks: s.tracks,
    speed: s.speed,
    chopDiv: s.chopDiv,
    chopWindowOffset: s.chopWindowOffset,
    window: s.window,
    filter: s.filter,
    masterVolume: s.masterVolume,
    playing: s.playing,
    headsMode: s.headsMode,
    loopMode: s.loopMode,
    activeTrack: s.activeTrack,
    grid: s.grid,
    lights: s.lights,
  };
}

export function restoreSnapshot(s: SurfaceState, snap: TxnSnapshot): SurfaceState {
  return { ...s, ...snap };
}


export const STEM_ROLES = ["vocals", "drums", "bass", "instruments"] as const;

const FIRED_LIMIT = 60;

/** Append one ordered audio command. Pure: returns a new state. */
function emit(
  state: SurfaceState,
  type: AudioCommandType,
  payload: AudioCommand["payload"],
  opts: { rowId?: string; txnId?: string; t?: number } = {},
): SurfaceState {
  return { ...state, commands: [...state.commands, makeCommand(type, payload, opts)].slice(-COMMAND_LOG_LIMIT) };
}
let firedSeq = 0;

function track(stem: TrackSlice["stem"], volume: number): TrackSlice {
  return {
    content: "loaded",
    volume,
    stem,
    headPos: 0,
    headReverse: false,
    headMuted: false,
    headLevel: 0.8,
    headLatched: false,
    headLoop: { active: false, bars: 1 },
    soloLatched: false,
    laneReverse: false,
    laneLoop: { active: false, bars: 1 },
  };
}



/**
 * Momentary hold dispatch. In Heads Mode the four Tracks are the four heads,
 * so a hold PLAYS exactly the held group on the heads' own clock; everywhere
 * else it is the tape-lane momentary audition. One mask, two destinations —
 * the two layers can never disagree about who is held.
 */
function emitHold(state: SurfaceState, mask: string, t: number, rowId: string): SurfaceState {
  const heads = state.headsMode && !state.perf.fxOverlay;
  return emit(state, heads ? "heads.play.hold" : "lane.audition", { mask }, { rowId: heads ? "heads.play" : rowId, t });
}

/**
 * Stem Instrument Mode ingress.
 *
 * The reducer does NOT decide learn vs play — only the engine knows the frame
 * the event landed on and whether the tape was eligible at that instant. It
 * appends ONE ordered command carrying the normalized event plus the hardware
 * qualifiers held at that moment, so cues stay in the same ordered stream as
 * every gesture and can never overtake a mute or a loop.
 */
export function applyMidiEvent(
  state: SurfaceState,
  ev: {
    kind: "noteOn" | "noteOff" | "allNotesOff";
    note: number;
    velocity: number;
    channel: number;
    timestampMs: number;
    source: string;
    deviceId: string;
    deviceName: string;
  },
): SurfaceState {
  const tracksHeld = [0, 1, 2, 3]
    .map((i) => (state.pressed.includes(`track-button-${i + 1}` as Control) ? "1" : "0"))
    .join("");
  return emit(
    state,
    ev.kind === "allNotesOff" ? "cue.panic" : "cue.event",
    {
      kind: ev.kind,
      note: ev.note,
      velocity: ev.velocity,
      channel: ev.channel,
      timestampMs: ev.timestampMs,
      source: ev.source,
      deviceId: ev.deviceId,
      deviceName: ev.deviceName,
      functionHeld: state.functionHeld,
      tracksHeld,
    },
    { rowId: "cue.midi", t: ev.timestampMs },
  );
}

export function initialSurfaceState(): SurfaceState {
  return {
    power: "on",
    playing: false,
    functionHeld: false,
    pressed: [],
    tracks: [track("vocals", 0.78), track("drums", 0.72), track("bass", 0.65), track("instruments", 0.7)],
    activeTrack: 0,
    masterVolume: 0.7,
    rocker: "center",
    lastGesture: null,
    speed: 1,
    speedGlide: false,
    chopDiv: 1,
    chopGlide: false,
    chopWindowOffset: 0,
    window: { start: 0, end: 1, shift: 0, reverse: false },
    filter: { mode: "off", amount: 0 },
    loopMode: "variable",
    headsMode: false,
    headsSource: null,
    globalLoop: { active: false, latched: false, division: 1 },
    trackSelectArmedAt: null,

    lights: "full",
    song: 0,
    bank: 0,
    bankJumpArmed: false,
    grid: { bpm: null, rejected: false, source: "none" },
    fnTapTimes: [],
    fnTapCount: 0,
    fnHoldReached: false,
    fnModifierUsed: false,
    txn: null,
    songMemory: {},
    maxTakeSeconds: 480,

    commands: [],
    globalScrub: 0,
    scrubSpeed: DEFAULT_SCRUB_SPEED_INDEX as ScrubSpeedIndex,
    scrubLatched: false,
    fxFlashAt: null,
    laneScrub: [0, 0, 0, 0],
    auditionChord: [],


    perf: initialStemPerformance(),

    fired: [],
    coverage: {},
    note: "phase 4 — v2.6 simulation + ordered audio command stream",
  };
}

/**
 * Hotfix — held global four-stem shuttle (FUNCTION + rocker, or F+Q / F+A).
 * Emits ONE ordered start command on entry and ONE end command on release; the
 * discrete `transport.scrub` step row is never dispatched while it is held.
 */
export function applyGlobalScrub(state: SurfaceState, dir: 1 | -1 | null, t = 0): SurfaceState {
  // Universal lane qualifier: with Track buttons held, the shuttle is scoped to
  // those lanes and the shared transport is never moved.
  const held = state.pressed.filter((x) => x.startsWith("track-button")).map(trackIndexOf);
  const active = state.laneScrub.map((v, i) => (v !== 0 ? i : -1)).filter((i) => i >= 0);
  if (dir === null || held.length === 0) {
    let next = state;
    if (active.length > 0) {
      const laneScrub = [...next.laneScrub] as (0 | 1 | -1)[];
      for (const i of active) {
        laneScrub[i] = 0;
        next = emit(next, "lane.scrub.end", { lane: i }, { rowId: "lane.scrub", t });
      }
      next = { ...next, laneScrub, lastGesture: `lane shuttle release (${active.map((i) => i + 1).join(",")})` };
      if (dir === null) return next;
    }
    if (dir === null) {
      if (next.globalScrub === 0) return next;
      // Addendum §4: a LATCHED shuttle ignores the key release entirely — the
      // reels keep turning until an explicit release. Direction is kept.
      if (next.scrubLatched) return next;
      return emit(
        { ...next, globalScrub: 0, lastGesture: "global shuttle release" },
        "transport.scrub.end",
        {},
        { rowId: "transport.scrub.global", t },
      );
    }
    if (next.globalScrub === dir) return next;
    return emit(
      { ...next, globalScrub: dir, lastGesture: `global shuttle ${dir > 0 ? "forward" : "backward"}` },
      "transport.scrub.start",
      { direction: dir },
      { rowId: "transport.scrub.global", t },
    );
  }
  // Lane-scoped shuttle. Release any global shuttle first so only one shuttle
  // scope can ever be sounding.
  let next = state;
  if (next.globalScrub !== 0) {
    next = emit(
      { ...next, globalScrub: 0, scrubLatched: false },
      "transport.scrub.end",
      {},
      { rowId: "transport.scrub.global", t },
    );
  }
  const laneScrub = [...next.laneScrub] as (0 | 1 | -1)[];
  let changed = false;
  for (const i of held) {
    if (laneScrub[i] === dir) continue;
    laneScrub[i] = dir;
    changed = true;
    next = emit(next, "lane.scrub.start", { lane: i, direction: dir }, { rowId: "lane.scrub", t });
  }
  // Lanes released from the chord stop shuttling immediately.
  for (const i of active) {
    if (held.some((h) => h === i)) continue;
    laneScrub[i] = 0;
    changed = true;
    next = emit(next, "lane.scrub.end", { lane: i }, { rowId: "lane.scrub", t });
  }
  if (!changed) return next;
  return {
    ...next,
    laneScrub,
    lastGesture: `lane shuttle ${dir > 0 ? "forward" : "backward"} · ${held.map((i) => i + 1).join(",")}`,
  };
}

/**
 * Addendum §4 — end a LATCHED shuttle. This is the only way a latched shuttle
 * can stop, so it is also what the transport calls before any command that
 * assumes the tape is under normal control.
 */
export function releaseScrubLatch(state: SurfaceState, t = 0): SurfaceState {
  if (!state.scrubLatched) return state;
  const next = emit(
    { ...state, scrubLatched: false, globalScrub: 0, lastGesture: "shuttle latch released" },
    "transport.scrub.end",
    {},
    { rowId: "transport.scrub.global", t },
  );
  return fire(next, "rocker.scrub", "shuttle latch released — transport returns to the song", t);
}

/**
 * Addendum §3 — step the PERSISTENT shuttle speed. It is remembered whether or
 * not a shuttle is currently open, and while one IS open the change is audible
 * immediately. Master volume is never touched on this path.
 */
export function stepScrubSpeed(state: SurfaceState, dir: 1 | -1, t = 0): SurfaceState {
  const index = Math.max(
    0,
    Math.min(GLOBAL_SCRUB_SPEEDS.length - 1, state.scrubSpeed + dir),
  ) as ScrubSpeedIndex;
  const rate = GLOBAL_SCRUB_SPEEDS[index]!;
  let next: SurfaceState = { ...state, scrubSpeed: index };
  next = emit(next, "transport.scrub.speed", { index, rate }, { rowId: "rocker.scrub", t });
  return fire(next, "rocker.scrub", `shuttle speed ${index + 1}/4 → ${rate.toFixed(2)}×`, t);
}

function fire(state: SurfaceState, rowId: string, detail: string, t: number): SurfaceState {
  const row = V26_ROW_BY_ID[rowId];
  return {
    ...state,
    fired: [{ id: ++firedSeq, rowId, detail: row ? `${row.command} — ${detail}` : detail, t }, ...state.fired].slice(
      0,
      FIRED_LIMIT,
    ),
    coverage: { ...state.coverage, [rowId]: (state.coverage[rowId] ?? 0) + 1 },
  };
}

const clamp01 = (n: number) => (n < 0 ? 0 : n > 1 ? 1 : n);
const trackIndexOf = (control: Control) => (Number(control.slice(-1)) - 1) as TrackIndex;

/** Lanes whose Track button is currently held (plus `extra`, deduped, sorted). */
function heldTrackLanes(state: SurfaceState, extra?: number): number[] {
  const set = new Set<number>();
  for (const c of state.pressed) if (c.startsWith("track-button")) set.add(trackIndexOf(c));
  if (extra != null) set.add(extra);
  return [...set].sort((a, b) => a - b);
}

/** Four-character audibility mask, "1" for every lane in `lanes`. */
const maskOf = (lanes: number[]) => [0, 1, 2, 3].map((k) => (lanes.includes(k) ? "1" : "0")).join("");


function setTrack(state: SurfaceState, i: number, patch: Partial<TrackSlice>): SurfaceState["tracks"] {
  const tracks = [...state.tracks] as SurfaceState["tracks"];
  const slice = tracks[i];
  if (slice) tracks[i] = { ...slice, ...patch };
  return tracks;
}
/**
 * v2.6 songs.memory — "every song remembers loops, speed, chop, mutes, grid".
 * Leaving a song writes its snapshot to memory; arriving at one restores it
 * (or starts a clean slot if it has never been visited).
 */
function loadSong(state: SurfaceState, song: number): SurfaceState {
  if (song === state.song) return state;
  const songMemory = { ...state.songMemory, [state.song]: snapshotOf(state) };
  const target = songMemory[song];
  const base: SurfaceState = { ...state, songMemory, song };
  const next = target ? restoreSnapshot(base, target) : base;
  return fire(
    next,
    "songs.memory",
    target
      ? `song ${song + 1}/16 recalled — loops, speed ${next.speed.toFixed(4)}×, chop 1/${next.chopDiv}, mutes, grid`
      : `song ${song + 1}/16 opened — new slot, song ${state.song + 1} stored`,
    performance.now(),
  );
}


/** ---------------- v2.6 gesture → command dispatch ---------------- */

/** FN + rocker scrub step, seconds of song time per tap. */
export const SCRUB_STEP_S = 0.5;

export function applyGesture(state: SurfaceState, g: Gesture): SurfaceState {
  let next: SurfaceState = { ...state };
  // FUNCTION qualification is sampled at PRESS time for taps: deferred Track
  // arbitration commits up to trackDecisionMs after the release, by which time
  // the user has usually let FUNCTION go. Without this, FN + double-tap
  // (lane reverse) degraded into a bare double-tap (loop capture).
  const fn = state.functionHeld || (g.type === "tap" && g.qualified === true);
  const t = g.t;

  // Power is a v2.6 row: FUNCTION hold. It uses its OWN configurable threshold
  // (timings.powerHoldMs), never the general 450 ms hold, and it only commits on
  // FUNCTION release (see releaseControl) so FN + X chords can't power down.
  if (g.type === "holdStart" && g.control === "function" && g.level === "power") {
    return { ...next, fnHoldReached: true };
  }
  // While off, nothing but that row responds.
  if (state.power === "off") return next;

  switch (g.type) {
    case "tap": {
      const c = g.control;

      // Transactional multi-tap. ×1 snapshots the machine; ×2 / ×3 restore that
      // snapshot IN FULL before running, so no intermediate count leaves state
      // behind (rocker ×1 +1 BPM must not compound into the ×2 semitone, and
      // FN+PLAY ×2's 1.0× snap must not survive into ×3 heads mode).
      if (g.count > 1 && state.txn && state.txn.control === c) {
        // Revoke the optimistic ×1 action in the engine too, before ×2/×3 runs.
        // The payload carries the pre-tap truth the engine must re-assert:
        // rate plus the per-track mute mask (an optimistic mute must not stick).
        next = emit(
          restoreSnapshot(next, state.txn.snapshot),
          "rollback",
          {
            control: c,
            toCount: g.count,
            rate: state.txn.snapshot.speed,
            mutes: state.txn.snapshot.tracks.map((t) => (t.content === "muted" ? 1 : 0)).join(""),
          },
          { txnId: `${c}:${state.txn.count}`, t },
        );
        next = { ...next, txn: { control: c, count: g.count, snapshot: state.txn.snapshot } };
      } else if (g.count === 1) {
        next = { ...next, txn: { control: c, count: 1, snapshot: snapshotOf(state) } };
      }


      if (c === "play") {
        if (fn && g.count === 3) {
          // PROVISIONAL. The reducer does NOT flip headsMode here: the audio
          // engine's ack is the only thing that may change it (applyHeadsFeedback).
          // Optimistic entry is what allowed the surface to claim Heads while
          // HeadLanes had rejected it.
          const on = !next.headsMode;
          next = emit(next, on ? "heads.enter" : "heads.exit", {}, { rowId: "play.heads", t });
          next = fire(next, "play.heads", `heads ${on ? "on" : "off"} requested — waiting for the audio engine`, t);
          return next;
        }

        if (fn && g.count === 2) {
          next = { ...next, speed: 1 };
          return fire(emit(next, "rate.set", { rate: 1 }, { rowId: "play.snap", t }), "play.snap", "speed snapped to 1.000×", t);
        }
        if (fn && g.count === 1) {
          // ×1 = half-speed toggle. Mutually exclusive with ×2 and ×3, which is
          // exactly why the gesture engine defers FUNCTION-qualified PLAY for
          // fnPlayDecisionMs instead of firing this optimistically.
          const rate = Math.abs(next.speed - 0.5) < 1e-6 ? 1 : 0.5;
          next = { ...next, speed: rate };
          return fire(
            emit(next, "rate.set", { rate }, { rowId: "play.halfSpeed", t }),
            "play.halfSpeed",
            `speed ${rate.toFixed(3)}× — glided, all four stems`,
            t,
          );
        }
        if (!fn && g.count === 1) {
          if (next.globalLoop.latched) {
            // A latched global loop owns the bare PLAY tap: the first tap
            // releases the loop and the transport keeps running.
            next = { ...next, globalLoop: { ...next.globalLoop, active: false, latched: false } };
            next = emit(next, "loop.global.release", {}, { rowId: "tape.loop.global.release", t });
            return fire(next, "tape.loop.global.release", "global loop released — song continues", t);
          }
          next = { ...next, playing: !next.playing };
          next = emit(next, next.playing ? "transport.play" : "transport.stop", {}, { rowId: "play.toggle", t });
          return fire(next, "play.toggle", next.playing ? "play" : "stop", t);
        }
        return next;
      }

      if (c === "function") {
        // Tempo tapping, FN ×4 rounding and beatmatch re-tap are REMOVED: the
        // grid is detected automatically from the stems. A bare FUNCTION tap is
        // now CONTEXT-SENSITIVE, most-specific running operation first:
        //   shuttle open, unlatched (§4)  → LATCH the shuttle
        //   shuttle latched (§4)          → RELEASE it
        //   global loop held (§2)         → LATCH it
        //   global loop latched (§2)      → RELEASE it
        //   otherwise                     → ARM active-track selection
        if (next.globalScrub !== 0 && !next.scrubLatched) {
          next = { ...next, scrubLatched: true };
          return fire(
            next,
            "rocker.scrub",
            `shuttle latched ${next.globalScrub > 0 ? "forward" : "reverse"} — the rocker may be released`,
            t,
          );
        }
        if (next.scrubLatched) return releaseScrubLatch(next, t);
        if (next.globalLoop.active && !next.globalLoop.latched) {
          next = { ...next, globalLoop: { ...next.globalLoop, latched: true } };
          return fire(next, "tape.loop.global.latch", "global loop latched — PLAY may be released", t);
        }
        if (next.globalLoop.latched) {
          next = { ...next, globalLoop: { ...next.globalLoop, active: false, latched: false } };
          next = emit(next, "loop.global.release", {}, { rowId: "tape.loop.global.release", t });
          return fire(next, "tape.loop.global.release", "latched global loop released — song continues", t);
        }
        next = { ...next, trackSelectArmedAt: t };
        return fire(next, "tape.track.arm", "active-track selection armed — tap a Track button", t);
      }



      if (c.startsWith("track-button")) {
        const i = trackIndexOf(c);
        const slice = next.tracks[i]!;

        // ---- armed selection is EXCLUSIVE ------------------------------------
        // While the FUNCTION tap's arming window is open, the Track gesture
        // emits ONLY `stem.select`. No mute, no loop capture/release, no
        // audition, no FX bank action may leak out of a selection.
        if (next.trackSelectArmedAt != null && t - next.trackSelectArmedAt <= TRACK_SELECT_ARM_MS) {
          next = { ...next, trackSelectArmedAt: null, activeTrack: i, headsSource: next.headsMode ? i : next.headsSource };
          next = emit(next, "stem.select", { stem: i }, { rowId: "tape.track.select", t });
          return fire(next, "tape.track.select", `active track = ${i + 1} (selection only)`, t);
        }
        if (next.trackSelectArmedAt != null) next = { ...next, trackSelectArmedAt: null };


        // A Track button that took part in a CHORD audition never toggles mute
        // or loop on release: the chord already consumed that press.
        if (next.auditionChord.includes(i)) {
          next = { ...next, auditionChord: next.auditionChord.filter((k) => k !== i), activeTrack: i };
          return fire(next, "lane.audition", `lane ${i + 1} chord press consumed`, t);
        }


        // ---- universal Function-qualified lane gesture ----------------------
        // FUNCTION + double-tap Track = lane reverse, in EVERY layer (Tape,
        // Heads, FX overlay). This is one implementation, not three: the layer
        // only decides which lane the command lands on, never what it means.
        if (fn && g.count === 2) {
          const rev = !slice.laneReverse;
          next = { ...next, tracks: setTrack(next, i, { laneReverse: rev, headReverse: rev }), activeTrack: i };
          next = emit(next, "lane.reverse", { lane: i, reverse: rev }, { rowId: "lane.reverse", t });
          return fire(next, "lane.reverse", `lane ${i + 1} → ${rev ? "reverse" : "forward"}`, t);
        }

        if (fn) {
          // FN + track ×1 = bank jump. The next-song row now lives behind the
          // bank-jump arm rather than FN + double-tap, which lane reverse owns.
          next = { ...next, bank: i, bankJumpArmed: true, activeTrack: i };
          return fire(next, "track.bank", `bank jump → bank ${i + 1}`, t);
        }

        next = { ...next, activeTrack: i };

        // PLAY + Track = LATCHING solo (§2.1). Distinct from the momentary
        // audition, which is a bare hold and never latches.
        if (next.pressed.includes("play")) {
          const on = !slice.soloLatched;
          next = { ...next, tracks: setTrack(next, i, { soloLatched: on }) };
          const mask = next.tracks.map((s) => (s.soloLatched ? "1" : "0")).join("");
          next = emit(next, "stem.solo", { stem: i, on, mask }, { rowId: "track.solo", t });
          return fire(next, "track.solo", `lane ${i + 1} solo ${on ? "latched" : "released"} — mask ${mask}`, t);
        }

        // ---- Heads Mode v2: the four Tracks ARE the four heads ---------------
        // Deferred recognition guarantees exactly one of ×1 / ×2 / ×3 reaches
        // the engine: no mute or loop command is ever fired and then undone.
        if (next.headsMode && !next.perf.fxOverlay) {
          if (g.count === 3) {
            const latched = !slice.headLatched;
            next = { ...next, tracks: setTrack(next, i, { headLatched: latched }) };
            next = emit(next, "heads.latch", { head: i, latched }, { rowId: "heads.latch", t });
            return fire(next, "heads.latch", `head ${i + 1} independent playback ${latched ? "latched" : "released"} — transport untouched`, t);
          }
          if (g.count === 2) {
            // FUNCTION + double-tap is handled by the UNIVERSAL lane layer
            // above (lane.reverse); in heads mode the engine routes it to the
            // head. A bare double-tap captures this head's loop.
            const active = !slice.headLoop.active;
            next = {
              ...next,
              tracks: setTrack(next, i, { headLoop: { ...slice.headLoop, active }, headLatched: active ? true : slice.headLatched }),
            };
            next = emit(next, "heads.loop.capture", { head: i, bars: slice.headLoop.bars }, { rowId: "heads.loop", t });
            return fire(next, "heads.loop", `head ${i + 1} ${active ? `captured a ${slice.headLoop.bars} bar loop` : "loop released"}`, t);
          }
          // ×1: release the loop if one is running, otherwise mute / unmute.
          if (slice.headLoop.active) {
            next = { ...next, tracks: setTrack(next, i, { headLoop: { ...slice.headLoop, active: false } }) };
            next = emit(next, "heads.mute", { head: i, muted: slice.headMuted }, { rowId: "heads.loop", t });
            return fire(next, "heads.loop", `head ${i + 1} loop released — back into normal playback`, t);
          }
          const muted = !slice.headMuted;
          next = { ...next, tracks: setTrack(next, i, { headMuted: muted }) };
          next = emit(next, "heads.mute", { head: i, muted }, { rowId: "heads.mute", t });
          return fire(next, "heads.mute", `head ${i + 1} ${muted ? "muted" : "unmuted"}`, t);
        }

        // ---- approved bare-Track state table (§2.1) -------------------------
        // ×2 = capture / release the dedicated lane loop. Delete is gone from
        // the surface entirely; nothing here is destructive.
        if (g.count === 2) {
          const active = !slice.laneLoop.active;
          next = { ...next, tracks: setTrack(next, i, { laneLoop: { ...slice.laneLoop, active } }) };
          next = emit(
            next,
            active ? "loop.capture" : "loop.release",
            { lane: i, bars: slice.laneLoop.bars },
            { rowId: active ? "loop.capture" : "loop.release", t },
          );
          return fire(
            next,
            active ? "loop.capture" : "loop.release",
            `lane ${i + 1} ${active ? `loop captured (${slice.laneLoop.bars} bar)` : "loop released"}`,
            t,
          );
        }

        // ×1 while the lane loop is running = EXIT the loop and return to the
        // song. It must not touch mute: the lane keeps sounding, unlooped.
        if (slice.laneLoop.active) {
          next = { ...next, tracks: setTrack(next, i, { laneLoop: { ...slice.laneLoop, active: false } }) };
          next = emit(next, "loop.release", { lane: i, bars: slice.laneLoop.bars }, { rowId: "loop.release", t });
          return fire(next, "loop.release", `lane ${i + 1} loop released — back to the song`, t);
        }

        // ×1 = mute / unmute. In heads mode the lane is a head, so the mute
        // lands on the head level instead of the dry stem.



        switch (slice.content) {
          case "empty":
            return fire(next, "track.tap", `track ${i + 1} empty — nothing to stop or mute`, t);
          default: {
            const muted = slice.content === "muted";
            next = { ...next, tracks: setTrack(next, i, { content: muted ? "loaded" : "muted" }) };
            next = emit(next, muted ? "track.unmute" : "track.mute", { track: i }, { rowId: "track.tap", t });
            return fire(next, "track.tap", `track ${i + 1} ${muted ? "unmute" : "mute"}`, t);
          }
        }
      }



      if (c === "rocker-fwd" || c === "rocker-rwd") {
        const dir = c === "rocker-fwd" ? 1 : -1;
        // Stem Tape extension `rocker.chop.play`: chop lives on PLAY + rocker.
        // The arbiter claims PLAY before dispatch, so the transport never fires.
        if (state.pressed.includes("play")) {
          // PLAY held + rocker MOVES the global loop window by one division.
          // The chop family is retired: the same physical gesture now nudges
          // the loop that Hold PLAY just created, which is what the rocker
          // deflection was always suppressing the transport for.
          next = emit(
            next,
            "loop.global.move",
            { steps: dir, division: next.globalLoop.division },
            { rowId: "tape.loop.global.move", t },
          );
          return fire(
            next,
            "tape.loop.global.move",
            `global loop moved ${dir > 0 ? "forward" : "back"} 1/${next.globalLoop.division} bar`,
            t,
          );
        }
        if (!next.playing && !fn) {
          // Stopped: the rocker is the stock SONG SKIP, not varispeed. Varispeed
          // on a parked transport is inaudible and loses the cue.
          const song = Math.max(0, next.song + dir);
          next = { ...next, song };
          next = emit(next, "transport.cue", { frame: 0 }, { rowId: "rocker.songSkip", t });
          return fire(next, "rocker.songSkip", `song ${dir > 0 ? "next" : "previous"} → ${song + 1}, cued at 0:00`, t);
        }
        if (fn) {
          const seconds = dir * SCRUB_STEP_S;
          next = emit(next, "transport.scrub", { seconds }, { rowId: "rocker.scrub", t });
          return fire(
            next,
            "rocker.scrub",
            `global scrub ${dir > 0 ? "+" : "−"}${SCRUB_STEP_S.toFixed(2)}s — one shared playhead, all four stems`,
            t,
          );
        }
        if (g.count === 2) {
          const speed = next.speed * Math.pow(2, dir / 12);
          next = emit({ ...next, speed }, "rate.set", { rate: speed }, { rowId: "rocker.semitone", t });
          return fire(next, "rocker.semitone", `exact semitone ${dir > 0 ? "+1" : "−1"} → ${speed.toFixed(4)}×`, t);
        }
        const bpmBase = next.grid.bpm ?? 120;
        const speed = next.speed * ((bpmBase + dir) / bpmBase);
        next = emit({ ...next, speed }, "rate.set", { rate: speed }, { rowId: "rocker.speed", txnId: `${c}:1`, t });

        return fire(next, "rocker.speed", `${dir > 0 ? "+" : "−"}1 BPM → ${speed.toFixed(4)}×`, t);
      }


      if (c === "volume-plus" || c === "volume-minus") {
        const dir = c === "volume-plus" ? 1 : -1;
        // Addendum §3: while the shuttle is open (held OR latched) Volume ± is
        // the shuttle-speed selector, with or without FUNCTION, and it must
        // never move the master gain.
        if (next.globalScrub !== 0 || next.scrubLatched) return stepScrubSpeed(next, dir, t);
        if (fn) {
          // Addendum §1: with the FX overlay open, FUNCTION + Volume belongs to
          // the FX target chord layer — the transport must not read it here.
          if (next.perf.fxOverlay) return next;
          // Universal lane loop resize: FUNCTION + Track held + Volume ±.
          // Halve / double the captured loop of every held lane, in bars, so
          // the resize is grid-exact instead of a free-running time nudge.
          const held = next.pressed.filter((x) => x.startsWith("track-button")).map(trackIndexOf);
          if (held.length > 0 && next.headsMode && !next.perf.fxOverlay) {
            // FUNCTION + Track held + Volume ± resizes THAT head's loop.
            const notes: string[] = [];
            for (const i of held) {
              const slice = next.tracks[i]!;
              const bars = Math.max(0.25, Math.min(8, dir > 0 ? slice.headLoop.bars * 2 : slice.headLoop.bars / 2));
              next = { ...next, tracks: setTrack(next, i, { headLoop: { ...slice.headLoop, bars } }) };
              next = emit(next, "heads.loop.resize", { head: i, bars, direction: dir }, { rowId: "heads.loop", t });
              notes.push(`head ${i + 1} → ${bars} bar`);
            }
            return fire(next, "heads.loop", notes.join(" · "), t);
          }
          if (held.length > 0) {
            const notes: string[] = [];
            for (const i of held) {
              const slice = next.tracks[i]!;
              const bars = Math.max(0.25, Math.min(8, dir > 0 ? slice.laneLoop.bars * 2 : slice.laneLoop.bars / 2));
              next = { ...next, tracks: setTrack(next, i, { laneLoop: { ...slice.laneLoop, bars } }) };
              next = emit(next, "loop.resize", { lane: i, bars }, { rowId: "loop.resize", t });
              notes.push(`lane ${i + 1} → ${bars} bar`);
            }
            return fire(next, "loop.resize", notes.join(" · "), t);
          }
          // Addendum §1: FUNCTION + Volume with NO Track held is CONTEXTUAL.
          // A global loop that is running or latched owns it — the division is
          // the thing being performed. With no loop, the same chord selects the
          // active stem, so the gesture is never dead.
          if (next.globalLoop.active || next.globalLoop.latched) {
            const order: (1 | 2 | 4 | 8)[] = [1, 2, 4, 8];
            const at = order.indexOf(next.globalLoop.division);
            const division = order[Math.max(0, Math.min(order.length - 1, at + (dir > 0 ? 1 : -1)))]!;
            next = { ...next, globalLoop: { ...next.globalLoop, division } };
            next = emit(next, "loop.global.resize", { division }, { rowId: "tape.loop.global.resize", t });
            return fire(next, "tape.loop.global.resize", `global loop division → 1/${division} bar`, t);
          }
          const stem = ((next.activeTrack + (dir > 0 ? 1 : 3)) % 4) as TrackIndex;
          next = {
            ...next,
            activeTrack: stem,
            perf: { ...next.perf, activeStem: stem as StemPerformanceState["activeStem"] },
          };
          next = emit(next, "stem.select", { stem }, { rowId: "tape.track.arm", t });
          return fire(next, "tape.track.arm", `active stem → ${stem + 1}`, t);
        }
        const masterVolume = clamp01(next.masterVolume + dir * 0.0625);
        next = emit({ ...next, masterVolume }, "master.gain", { level: masterVolume }, { rowId: "volume.master", t });
        return fire(next, "volume.master", `master → ${masterVolume.toFixed(3)}`, t);
      }


      // Heads reverse is a Track double-tap (§3.3), never a fader double-tap.

      return next;
    }

    case "holdStart": {
      const c = g.control;
      if (c === "play") {
        if (g.level === "long") {
          next = { ...next, lights: state.lights === "full" ? "dim" : "full" };
          return fire(next, "play.lights", `lights ${next.lights}`, t);
        }
        if (g.level === "hold" && !fn) {
          // Hold PLAY is transport-state dependent (stock S1 LOOP):
          //   playing → the GLOBAL one-bar loop, held, at the current division
          //   stopped → the utility cue, every stem parked on frame zero
          if (state.playing) {
            next = { ...next, globalLoop: { ...next.globalLoop, active: true } };
            next = emit(
              next,
              "loop.global.start",
              { division: next.globalLoop.division },
              { rowId: "tape.loop.global.hold", t },
            );
            return fire(
              next,
              "tape.loop.global.hold",
              `global loop held · 1/${next.globalLoop.division} bar — all four stems, bar-locked`,
              t,
            );
          }
          return fire(
            emit({ ...next, playing: false }, "transport.cue", { frame: 0 }, { rowId: "play.cue", t }),
            "play.cue",
            "cued at frame 0 — next Play tap launches EXACT",
            t,
          );
        }
        return next;
      }
      if (c.startsWith("track-button") && g.level === "hold") {
        const i = trackIndexOf(c);
        const slice = next.tracks[i]!;

        if (fn) {
          // FUNCTION + Track hold in heads mode assigns the source lane; the
          // heads source is a PANEL/qualified decision, never a bare hold.
          // Heads v2 has NO source assignment: head N is lane N. FUNCTION +
          // Track hold is only a qualifier here (for Volume ± loop resize).
          return next;
        }

        // ---- momentary audition (§2.1) --------------------------------------
        // Bare Track hold auditions the HELD lanes for as long as they are
        // held. Two or three Track buttons together audition as a CHORD.
        // It writes NOTHING into mute or latched-solo state, so the release
        // restores the previous mix exactly.
        const lanes = heldTrackLanes(next, i);
        const mask = maskOf(lanes);
        next = { ...next, activeTrack: i, auditionChord: lanes.length > 1 ? lanes : [] };
        next = emitHold(next, mask, t, "lane.audition");
        return fire(
          next,
          "lane.audition",
          `${lanes.length > 1 ? "chord" : `lane ${i + 1}`} momentary audition — mask ${mask}`,
          t,
        );

      }


      if ((c === "rocker-fwd" || c === "rocker-rwd") && g.level === "hold") {
        // Hold PLAY + rocker = chop glide. FN + rocker is the audible shuttle,
        // which owns its own hold and must not also start a glide.
        if (state.pressed.includes("play")) return fire({ ...next, chopGlide: true }, "rocker.chop.play", "hold PLAY + rocker = chop glide", t);
        if (fn) return next;
        return fire({ ...next, speedGlide: true }, "rocker.speed", "hold = continuous speed glide", t);
      }

      if ((c === "volume-plus" || c === "volume-minus") && g.level === "hold" && fn) {
        // Addendum §1: press duration must NOT change the command. The glide
        // only exists for the held-lane window resize; in the shuttle-speed,
        // loop-division and stem-select contexts a hold is just a long tap.
        const heldLanes = next.pressed.some((x) => x.startsWith("track-button"));
        if (!heldLanes) return next;
        return fire({ ...next, chopGlide: true }, "volume.chopWindow", "hold = glide", t);
      }

      return next;
    }

    case "holdEnd": {
      if (g.control === "play" && next.globalLoop.active) {
        // Release-order latching: a FUNCTION tap during the hold set `latched`,
        // so the loop survives PLAY coming up. Otherwise the loop is momentary
        // and ends exactly with the button.
        if (next.globalLoop.latched) {
          return fire(next, "tape.loop.global.latch", "global loop latched — PLAY released, loop continues", t);
        }
        next = { ...next, globalLoop: { ...next.globalLoop, active: false } };
        next = emit(next, "loop.global.release", {}, { rowId: "tape.loop.global.release", t });
        return fire(next, "tape.loop.global.release", "global loop released — back to the song", t);
      }
      if (g.control.startsWith("rocker") || g.control.startsWith("volume")) {
        return { ...next, speedGlide: false, chopGlide: false };
      }
      if (g.control.startsWith("track-button") && g.level === "hold") {
        // One member of a chord audition can lift while the others stay down:
        // the audition narrows to whatever is still held. An empty mask is the
        // explicit "restore" instruction — the engine never remembers state.
        const lanes = heldTrackLanes(next);
        const mask = lanes.length ? maskOf(lanes) : "";
        next = { ...next, auditionChord: lanes.length > 1 ? lanes : next.auditionChord };
        next = emitHold(next, mask, t, "lane.audition");
        return fire(
          next,
          "lane.audition",
          mask ? `audition narrowed — mask ${mask}` : "audition released — prior mix restored",
          t,
        );
      }

      return next;
    }


    case "tapThenHold": {
      // FUNCTION tap-then-hold used to round / clear the tapped tempo grid.
      // Tempo tapping and FN ×4 rounding are REMOVED: the grid is detected
      // automatically (local, deterministic, non-AI) and manual correction
      // lives in Projects. FUNCTION is a pure modifier + selection arm now.
      if (g.control === "function") return { ...next, fnModifierUsed: true };
      return next;
    }

    case "chordStart": {
      // Two or three Track buttons pressed together = an IMMEDIATE chord
      // audition. It must not wait for the 450 ms hold threshold, and it must
      // not be replaced lane-by-lane as each individual hold matures.
      const lanes = g.controls.filter((c) => c.startsWith("track-button")).map(trackIndexOf);
      if (lanes.length >= 2 && lanes.length === g.controls.length && !fn && !next.pressed.includes("play")) {
        const mask = maskOf(lanes);
        next = { ...next, auditionChord: lanes, activeTrack: lanes[0] as TrackIndex };
        next = emitHold(next, mask, t, "lane.audition");
        return fire(next, "lane.audition", `chord audition — mask ${mask}`, t);
      }
      return next;
    }

    case "chordRelease": {
      const set = g.controls;
      // `play.loopMode` (FUNCTION + PLAY released together) is REMOVED: FUNCTION
      // + PLAY is now a deferred ×1/×2/×3 group and a simultaneous release must
      // not also flip a loop mode behind it.
      if (set.every((c) => c.startsWith("track-button")) && set.length >= 2) {
        const lanes = heldTrackLanes(next);
        const mask = lanes.length ? maskOf(lanes) : "";
        next = emitHold(next, mask, t, "lane.audition");
        return fire(next, "lane.audition", mask ? `chord narrowed — mask ${mask}` : "chord released — prior mix restored", t);
      }
      return next;
    }


    default:
      return next;
  }
}

/** Raw press bookkeeping. FUNCTION + anything marks FUNCTION as a modifier. */
export function pressControl(state: SurfaceState, control: Control): SurfaceState {
  if (state.pressed.includes(control)) return state;
  const pressed = [...state.pressed, control];
  return {
    ...state,
    pressed,
    functionHeld: control === "function" ? true : state.functionHeld,
    fnModifierUsed:
      control !== "function" && state.functionHeld ? true : state.fnModifierUsed,

    fnHoldReached: control === "function" ? false : state.fnHoldReached,
    rocker:
      control === "rocker-fwd" ? "forward" : control === "rocker-rwd" ? "rewind" : state.rocker,
  };
}

/**
 * Raw release bookkeeping. FUNCTION hold is the only power row in v2.6, and it
 * commits here: past 450 ms AND never used as a modifier during that hold.
 */
export function releaseControl(state: SurfaceState, control: Control): SurfaceState {
  const pressed = state.pressed.filter((c) => c !== control);
  let next: SurfaceState = {
    ...state,
    pressed,
    rocker: control.startsWith("rocker") ? "center" : state.rocker,
  };
  if (control !== "function") return next;

  const shouldToggle = state.fnHoldReached && !state.fnModifierUsed;
  next = { ...next, functionHeld: false, fnHoldReached: false, fnModifierUsed: false };
  if (!shouldToggle) return next;
  const power = state.power === "on" ? "off" : "on";
  next = { ...next, power, playing: power === "off" ? false : next.playing };
  if (power === "off") next = emit(next, "transport.stop", {}, { rowId: "fn.power" });
  return fire(next, "fn.power", `power ${power}`, performance.now());
}


// ------------------------------------------------- engine → surface feedback
//
// The engine is authoritative for what is audible. These setters are the ONLY
// way its verdicts reach the surface state, so no handler ever fakes them.


export function setTrackContent(state: SurfaceState, i: number, content: TrackContent): SurfaceState {
  return { ...state, tracks: setTrack(state, i, { content }) };
}

/**
 * Heads verdicts from the engine. This is the ONLY writer of `headsMode`:
 * the triple-tap emits a provisional command and the engine's ack lands here,
 * so a rejected entry can never leave the surface claiming Heads is active.
 */
export function applyHeadsFeedback(
  state: SurfaceState,
  patch: { active?: boolean; source?: number | null; printedTrack?: number },
): SurfaceState {
  let next: SurfaceState = { ...state };
  if (patch.active !== undefined && patch.active !== state.headsMode) {
    // Head-layer state belongs to a heads SESSION: it resets on every accepted
    // transition, never on the provisional gesture.
    const tracks = [...next.tracks] as SurfaceState["tracks"];
    for (let i = 0; i < 4; i++)
      tracks[i] = {
        ...tracks[i]!,
        headPos: 0,
        headReverse: false,
        headMuted: false,
        headLevel: 0.8,
        headLatched: false,
        headLoop: { active: false, bars: 1 },
      };
    next = { ...next, headsMode: patch.active, tracks, headsSource: patch.active ? next.headsSource : null };
  }
  if (patch.source !== undefined) next = { ...next, headsSource: patch.source };
  if (patch.printedTrack !== undefined) next = { ...next, tracks: setTrack(next, patch.printedTrack, { content: "loaded" }) };
  return next;
}

/** Fader commit routing: heads layer, FN layer, otherwise track volume. */

/**
 * `claimed` is the layer the gesture latched at pointer-down. A modifier
 * released mid-drag must not retarget a scrub into a level or a volume.
 */
export function applyFader(
  state: SurfaceState,
  index: number,
  value: number,
  claimed?: "headScrub" | "headLevel" | "window" | "laneScrub" | "fader",
): SurfaceState {
  const t = performance.now();
  // Heads claims the fader layer before the v2.6 FN window/filter rows (§3.3).
  if ((claimed === "headScrub" || claimed === "headLevel" || (!claimed && state.headsMode)) && !state.perf.fxOverlay) {
    if (claimed === "headScrub" || (!claimed && state.functionHeld)) {
      // Landing position of an audible scrub gesture (audio already travelled).
      const next = emit(
        { ...state, tracks: setTrack(state, index, { headPos: value }) },
        "heads.scrub",
        { head: index, position: value },
        { rowId: "heads.scrub", t },
      );
      return fire(next, "heads.scrub", `head ${index + 1} scrubbed to ${(value * 100).toFixed(1)}% of the cycle`, t);
    }
    // Releasing FUNCTION returns faders to level control with the STORED level.
    const next = emit(
      { ...state, tracks: setTrack(state, index, { headLevel: value }) },
      "heads.level",
      { head: index, level: value },
      { rowId: "heads.level", t },
    );
    return fire(next, "heads.level", `head ${index + 1} level → ${value.toFixed(3)}`, t);
  }
  if (claimed === "laneScrub") {
    // FUNCTION + fader parked lane N at this point in the song. The audio has
    // already travelled on the control bus; this is the semantic record, and
    // the landing is what the next Track double-tap captures one bar from.
    const next = emit(state, "lane.scrub.park", { lane: index, position: value }, { rowId: "lane.scrub", t });
    return fire(
      next,
      "lane.scrub.park",
      `lane ${index + 1} parked at ${(value * 100).toFixed(1)}% — double-tap track ${index + 1} to capture one bar`,
      t,
    );
  }
  if (claimed === "window" || (!claimed && state.functionHeld)) {

    if (index === 3) {
      const mode = value > 0.58 ? "hp" : value < 0.42 ? "lp" : "off";
      const amount = mode === "off" ? 0 : Math.abs(value - 0.5) * 2;
      return fire({ ...state, filter: { mode, amount } }, "fader.filter", `filter ${mode} · ${amount.toFixed(2)}`, t);
    }
    const key = (["start", "end", "shift"] as const)[index]!;
    const window = { ...state.window, [key]: value };
    window.reverse = window.start > window.end;
    const next = { ...state, window };
    const fired = fire(next, "fader.window", `window ${key} = ${value.toFixed(3)}`, t);
    return window.reverse
      ? fire(fired, "fader.windowReverse", "start past end → window plays in reverse", t)
      : fired;
  }

  // Commit value MUST equal the last audible preview value pushed by the
  // continuous control bus during the drag — same number, no re-derivation.
  return fire(
    emit(
      { ...state, tracks: setTrack(state, index, { volume: value }) },
      "track.gain",
      { track: index, level: value },
      { rowId: "fader.trackVolume", t },
    ),
    "fader.trackVolume",
    `track ${index + 1} volume → ${value.toFixed(3)}`,
    t,
  );
}

/**
 * Phase 5C — ONE semantic command per resolved chord.
 *
 * The arbiter has already suppressed the base Play / Volume / Track command, so
 * nothing here is emitted-then-undone.
 */
/** Human label for whichever rack the overlay is driving. */
function fxLabel(perf: StemPerformanceState): string {
  return perf.fxScope === "global" ? "global mix" : `stem ${perf.activeStem + 1}`;
}

export function applyPerfIntent(state: SurfaceState, intent: PerfIntent): SurfaceState {
  const t = performance.now();
  let next = { ...state };
  const perf = state.perf;
  const stemOf = (i: number) => i;

  switch (intent.type) {
    case "stem.select": {
      const p = selectStem(perf, intent.dir);
      next = { ...next, perf: p, activeTrack: p.activeStem as TrackIndex };
      next = emit(next, "stem.select", { stem: p.activeStem }, { rowId: "stem.select", t });
      return fire(next, "stem.select", `active stem → ${p.activeStem + 1}`, t);
    }
    case "stem.solo": {
      const p = toggleSolo(perf, intent.stem);
      const on = p.tracks[intent.stem]!.soloed;
      next = emit({ ...next, perf: p }, "stem.solo", { track: stemOf(intent.stem), on }, { rowId: "stem.solo", t });
      return fire(next, "stem.solo", `stem ${intent.stem + 1} solo ${on ? "on" : "off"} (overlap ${intent.overlapMs.toFixed(0)} ms)`, t);
    }
    case "stem.link": {
      const p = toggleLink(perf, intent.stem);
      const linked = p.tracks[intent.stem]!.linked;
      next = emit({ ...next, perf: p }, "stem.link", { track: stemOf(intent.stem), on: linked }, { rowId: "stem.link", t });
      return fire(next, "stem.link", `stem ${intent.stem + 1} ${linked ? "linked" : "unlinked"} (overlap ${intent.overlapMs.toFixed(0)} ms)`, t);
    }
    case "fx.overlay": {
      // Opening sets the scope; closing keeps the scope it was opened with so
      // the engine can address the right rack while the release fades.
      const scope = intent.on ? intent.scope : perf.fxScope;
      next = emit(
        { ...next, perf: { ...perf, fxOverlay: intent.on, fxScope: scope } },
        "fx.overlay",
        { on: intent.on, scope },
        { rowId: "fx.overlay.toggle", t },
      );
      return fire(
        next,
        "fx.overlay.toggle",
        `FX overlay ${intent.on ? `open — ${scope.toUpperCase()} scope` : "closed"} — tape audio continues`,
        t,
      );
    }
    case "system.pairing":
      return fire(next, "system.pairing", "Bluetooth pairing gesture (stock)", t);
    case "system.noop":
      return fire(next, "system.volumechord.ambiguous", intent.detail, t);
    case "fx.bank.select": {
      const target = fxTargetOf(perf);
      const p = selectBank(perf, target, intent.bank);
      next = emit({ ...next, perf: p }, "fx.bank.select", { track: intent.stem, bank: intent.bank, scope: perf.fxScope }, { rowId: "fx.bank.select", t });
      return fire(next, "fx.bank.select", `${fxLabel(perf)} bank ${BANKS[intent.bank]!.id} selected`, t);
    }
    case "fx.momentary.start":
    case "fx.momentary.end": {
      const on = intent.type === "fx.momentary.start";
      const target = fxTargetOf(perf);
      const p = setBankMomentary(perf, target, intent.bank, on);
      const bank = fxStateOf(p, target).banks[intent.bank]!;
      const algo = algorithmDef(intent.bank, bank.selectedAlgorithm);
      next = emit(
        { ...next, perf: p },
        intent.type,
        { track: intent.stem, bank: intent.bank, algorithm: bank.selectedAlgorithm, latched: bank.latched, scope: perf.fxScope },
        { rowId: `fx.${BANKS[intent.bank]!.id}.momentary`, t },
      );
      return fire(next, `fx.${BANKS[intent.bank]!.id}.momentary`, `${fxLabel(perf)} ${algo.label} ${on ? "engaged" : "released"}`, t);
    }
    case "fx.algorithm.cycle": {
      const target = fxTargetOf(perf);
      const p = cycleBankAlgorithm(perf, target, intent.bank, intent.dir);
      const bank = fxStateOf(p, target).banks[intent.bank]!;
      const algo = algorithmDef(intent.bank, bank.selectedAlgorithm);
      next = emit(
        { ...next, perf: p },
        "fx.algorithm.cycle",
        { track: intent.stem, bank: intent.bank, algorithm: bank.selectedAlgorithm, latched: bank.latched, scope: perf.fxScope },
        { rowId: `fx.${BANKS[intent.bank]!.id}.algorithm`, t },
      );
      return fire(next, `fx.${BANKS[intent.bank]!.id}.algorithm`, `${fxLabel(perf)} → ${algo.label}`, t);
    }
    case "fx.macro": {
      const target = fxTargetOf(perf);
      const p = nudgeBankMacro(perf, target, intent.bank, intent.dir);
      const bank = fxStateOf(p, target).banks[intent.bank]!;
      const value = bank.algorithms[bank.selectedAlgorithm]!.macroAmount;
      next = emit(
        { ...next, perf: p },
        "fx.macro",
        { track: intent.stem, bank: intent.bank, algorithm: bank.selectedAlgorithm, value, scope: perf.fxScope },
        { rowId: `fx.${BANKS[intent.bank]!.id}.macro`, t },
      );
      return fire(
        next,
        `fx.${BANKS[intent.bank]!.id}.macro`,
        `${fxLabel(perf)} ${algorithmDef(intent.bank, bank.selectedAlgorithm).label} macro → ${value.toFixed(2)}`,
        t,
      );
    }
    case "fx.latch": {
      const target = fxTargetOf(perf);
      const p = toggleBankLatch(perf, target, intent.bank);
      const bank = fxStateOf(p, target).banks[intent.bank]!;
      next = emit(
        // Addendum §5: every latch toggle stamps the confirmation flash so the
        // surface answers the gesture even when the effect itself is subtle.
        { ...next, perf: p, fxFlashAt: t },
        "fx.latch",
        { track: intent.stem, bank: intent.bank, on: bank.latched, algorithm: bank.selectedAlgorithm, latched: bank.latched, scope: perf.fxScope },
        { rowId: `fx.${BANKS[intent.bank]!.id}.latch`, t },
      );
      return fire(
        next,
        `fx.${BANKS[intent.bank]!.id}.latch`,
        `${fxLabel(perf)} ${algorithmDef(intent.bank, bank.selectedAlgorithm).label} ${bank.latched ? "latched" : "unlatched"}`,
        t,
      );
    }
    case "fx.clearLatches": {
      const target = fxTargetOf(perf);
      const p = clearLatches(perf, target);
      next = emit({ ...next, perf: p }, "fx.clearLatches", { track: intent.stem, scope: perf.fxScope }, { rowId: "fx.clearLatches", t });
      return fire(next, "fx.clearLatches", `${fxLabel(perf)} latches cleared`, t);
    }
  }
  return next;

}

/**
 * LED priority table (Phase 5C corrections 9/10, extended for Phase 6 §5).
 * No handler writes LEDs: every frame is derived here and the winner always
 * states why it won.
 *
 *   error > failed print/take > printing/finalising > recording > overdubbing >
 *   armed > momentary FX > latched FX > heads source/head state > soloed >
 *   unlinked > active > muted > base
 */
export const LED_PRIORITY = {
  error: 98,
  failedPrint: 95,
  printing: 93,
  recording: 90,
  overdubbing: 89,
  armed: 88,
  momentaryFx: 82,
  latchedFx: 78,
  heads: 76,
  soloed: 74,
  unlinked: 70,
  active: 66,
  muted: 30,
  base: 10,
} as const;


/**
 * LED arbitration — v2.6 LIGHTS block is the base layer:
 * dark = empty · faint = muted content · pulse = playing (own loop wrap) ·
 * song row: solid = song, blink = bank. Input feedback sits above it and every
 * winner states its reason so diagnostics can explain the arbitration.
 */
export function deriveLeds(state: SurfaceState): LedFrame {
  const frame = {} as LedFrame;
  const off = state.power === "off";

  state.tracks.forEach((track, i) => {
    const id = `track-led-${i + 1}` as LedId;
    const control = `track-button-${i + 1}` as Control;
    if (off) {
      frame[id] = { pattern: "dark", reason: "powered off (FUNCTION hold)", priority: 100 };
    } else if (state.grid.rejected) {
      frame[id] = { pattern: "blink", reason: "grid far off — all four blink, nothing moves (v2.6)", priority: 98 };
    } else if (state.perf.fxOverlay) {
      // Overlay on: track LEDs show STEM state, not v2.6 track content.
      const st = state.perf.tracks[i]!;
      const isActive = state.perf.activeStem === i;
      if (st.soloed) {
        frame[id] = { pattern: "solid", reason: "soloed stem (overlay)", priority: LED_PRIORITY.soloed };
      } else if (!st.linked) {
        frame[id] = { pattern: "blink", reason: "unlinked stem — double pulse (overlay)", priority: LED_PRIORITY.unlinked };
      } else if (isActive) {
        frame[id] = { pattern: "breathe", reason: "active stem (overlay)", priority: LED_PRIORITY.active };
      } else if (state.perf.tracks.some((x) => x.soloed)) {
        frame[id] = { pattern: "faint", reason: "non-solo stem (overlay)", priority: LED_PRIORITY.base };
      } else {
        frame[id] = { pattern: "faint", reason: "stem idle (overlay)", priority: LED_PRIORITY.base };
      }
    } else if (state.pressed.includes(control)) {
      frame[id] = { pattern: "solid", reason: "button held (input feedback)", priority: 95 };
    } else if (state.headsMode) {
      // Heads language (§5): full-bright chase over loaded content, faint chase
      // for an empty (printable) head, dark-but-distinguishable when muted.
      const loaded = track.content !== "empty";
      const dir = track.headReverse ? "reversed chase" : "chase";
      if (track.headLatched)
        frame[id] = { pattern: "solid", reason: `head ${i + 1} latched — playing independently of the transport`, priority: LED_PRIORITY.heads + 1 };
      else if (track.headMuted) frame[id] = { pattern: "faint", reason: `head ${i + 1} muted (still a head, not an empty slot)`, priority: LED_PRIORITY.heads - 2 };
      else if (loaded) frame[id] = { pattern: "chase", reason: `head ${i + 1} ${dir} over loaded content`, priority: LED_PRIORITY.heads };
      else frame[id] = { pattern: "faint", reason: `head ${i + 1} hollow — lane ${i + 1} is empty`, priority: LED_PRIORITY.heads - 1 };
    } else if (track.content === "empty") {
      frame[id] = { pattern: "dark", reason: "dark = empty (v2.6)", priority: 0 };
    } else if (track.content === "muted") {
      frame[id] = { pattern: "faint", reason: "faint = muted content (v2.6)", priority: LED_PRIORITY.muted };
    } else if (state.playing) {
      frame[id] = { pattern: "pulse", reason: "pulse = playing — pulses on its own loop wrap (v2.6)", priority: 20 };
    } else {
      frame[id] = { pattern: "faint", reason: "loaded, stopped", priority: LED_PRIORITY.base };
    }

  });

  // Song row: solid = song · blink = bank (v2.6).
  for (let i = 0; i < 4; i++) {
    const id = `side-led-${i + 1}` as LedId;
    if (off) frame[id] = { pattern: "dark", reason: "powered off", priority: 100 };
    else if (state.perf.fxOverlay) {
      // Side LEDs 1–4 = the four BANKS (physical order) for the ACTIVE stem,
      // read from the authoritative twelve-FX state, not the legacy families.
      const bankIndex = bankOfButton(i);
      // …and from the GLOBAL rack instead when the overlay was opened in
      // global scope, so the LEDs always describe the rack being played.
      const bank = fxStateOf(state.perf, fxTargetOf(state.perf)).banks[bankIndex]!;
      const def = algorithmDef(bankIndex, bank.selectedAlgorithm);
      const alg = bank.algorithms[bank.selectedAlgorithm]!;
      if (alg.rejected)
        frame[id] = { pattern: "blink", reason: `${def.label} rejected: ${alg.rejected}`, priority: LED_PRIORITY.error };
      else if (bank.momentary)
        frame[id] = { pattern: "breathe", reason: `${def.label} momentary (held)`, priority: LED_PRIORITY.momentaryFx };
      else if (bank.latched)
        frame[id] = { pattern: "solid", reason: `${def.label} latched`, priority: LED_PRIORITY.latchedFx };
      else frame[id] = { pattern: "dark", reason: `${def.label} inactive`, priority: LED_PRIORITY.base };
    } else if (state.bankJumpArmed && i === state.bank)
      frame[id] = { pattern: "blink", reason: "blink = bank (v2.6 song row)", priority: 40 };
    else if (i === state.song % 4)
      frame[id] = { pattern: "solid", reason: "solid = song (v2.6 song row)", priority: 20 };
    else frame[id] = { pattern: "dark", reason: "no song in this slot", priority: 0 };
  }


  frame["play-indicator"] = off
    ? { pattern: "dark", reason: "powered off", priority: 100 }
    : state.playing
      ? { pattern: "solid", reason: "transport running", priority: 30 }
      : { pattern: "faint", reason: "transport stopped", priority: 10 };

  frame["function-led-1"] = off
    ? { pattern: "dark", reason: "powered off", priority: 100 }
    : state.perf.fxOverlay
    ? { pattern: "pulse", reason: "FX overlay open — FUNCTION LEDs alternate-pulse", priority: LED_PRIORITY.momentaryFx }
    : state.functionHeld
      ? { pattern: "solid", reason: "function held", priority: 60 }
      : state.headsMode
        ? { pattern: "breathe", reason: "heads mode on", priority: 45 }
        : { pattern: "dark", reason: "function idle", priority: 0 };

  frame["function-led-2"] = off
    ? { pattern: "dark", reason: "powered off", priority: 100 }
    : state.perf.fxOverlay
    ? { pattern: "blink", reason: "FX overlay open — FUNCTION LEDs alternate-pulse", priority: LED_PRIORITY.momentaryFx }
    : state.pressed.some((c) => c.startsWith("rocker"))
      ? { pattern: "blink", reason: "rocker engaged", priority: 50 }
      : state.grid.bpm != null
        ? { pattern: "pulse", reason: `tempo grid ${state.grid.bpm.toFixed(1)} BPM (metronome)`, priority: 35 }
        : { pattern: "dark", reason: "rocker centred, no grid", priority: 0 };

  // Lights dim/full is a global brightness layer, not a pattern change.
  return frame;
}

/**
 * Rows that are not "fired" by a gesture but are OBSERVABLE in the rendered
 * frame (the LIGHTS block) or are pure documentation. Coverage reporting has to
 * account for all 37 rows, and these are satisfied by observation, not by a
 * dispatch. Returns rowId -> the evidence for it right now.
 */
export function observedRows(state: SurfaceState, leds: LedFrame): Record<string, string> {
  const out: Record<string, string> = {};
  const patterns = ([1, 2, 3, 4] as const).map((i) => leds[`track-led-${i}` as LedId]!.pattern);

  const dark = patterns.filter((p) => p === "dark").length;
  const faint = patterns.filter((p) => p === "faint").length;
  if (dark || faint)
    out["lights.base"] = `${dark} dark (empty) · ${faint} faint (muted / stopped content)`;

  const pulsing = patterns.filter((p) => p === "pulse").length;
  if (pulsing) out["lights.pulse"] = `${pulsing} track lights pulsing on their own loop wrap`;

  const songRow = ([1, 2, 3, 4] as const).map((i) => leds[`side-led-${i}` as LedId]!.pattern);
  if (songRow.some((p) => p === "solid" || p === "blink"))
    out["lights.songRow"] = `solid = song ${(state.song % 4) + 1} · ${
      state.bankJumpArmed ? `blink = bank ${state.bank + 1}` : "no bank blink"
    }`;

  out["songs.transfer"] =
    "documentation row — WAVs in + out happen on the hardware transfer page; no browser equivalent until the audio engine lands";

  return out;
}

