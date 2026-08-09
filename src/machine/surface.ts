import { COMMAND_LOG_LIMIT, makeCommand, type AudioCommand, type AudioCommandType } from "@/audio/commands";
import type { Control, TrackIndex } from "@/device/geometry";
import { TEMPO_TAP_IDLE_MS, type Gesture } from "@/input/gestures";
import { V26_ROW_BY_ID } from "@/machine/v26map";
import type { PerfIntent } from "@/machine/chordArbiter";
import {
  FX_FAMILIES,
  clearLatches,
  cycleBankAlgorithm,
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
import { BANKS, algorithmDef } from "@/machine/fx12";


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
    soloLatched: false,
    laneReverse: false,
    laneLoop: { active: false, bars: 1 },
  };
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
  if (dir === null) {
    if (state.globalScrub === 0) return state;
    return emit(
      { ...state, globalScrub: 0, lastGesture: "global shuttle release" },
      "transport.scrub.end",
      {},
      { rowId: "transport.scrub.global", t },
    );
  }
  if (state.globalScrub === dir) return state;
  return emit(
    { ...state, globalScrub: dir, lastGesture: `global shuttle ${dir > 0 ? "forward" : "backward"}` },
    "transport.scrub.start",
    { direction: dir },
    { rowId: "transport.scrub.global", t },
  );
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
  const fn = state.functionHeld;
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
          const on = !next.headsMode;
          // The completed triple-tap has already rolled back the ×1 transport
          // toggle and the ×2 1.0× snap above, so no lower-precedence Play
          // command survives into heads entry/exit.
          const tracks = [...next.tracks] as SurfaceState["tracks"];
          for (let i = 0; i < 4; i++)
            tracks[i] = { ...tracks[i]!, headPos: i * 0.25, headReverse: false, headMuted: false, headLevel: 0.8 };
          next = { ...next, headsMode: on, tracks, headsSource: on ? next.headsSource : null };
          next = emit(next, on ? "heads.enter" : "heads.exit", {}, { rowId: "play.heads", t });
          next = fire(next, "play.heads", `heads mode ${on ? "on" : "off"}`, t);
          if (on) next = fire(next, "heads.replay", "heads 1·2·3·4 read the source at 0 · 0.25 · 0.50 · 0.75 of the audible cycle", t);
          return next;
        }

        if (fn && g.count === 2) {
          next = { ...next, speed: 1 };
          return fire(emit(next, "rate.set", { rate: 1 }, { rowId: "play.snap", t }), "play.snap", "speed snapped to 1.000×", t);
        }
        if (!fn && g.count === 1) {
          next = { ...next, playing: !next.playing };
          next = emit(next, next.playing ? "transport.play" : "transport.stop", {}, { rowId: "play.toggle", t });
          return fire(next, "play.toggle", next.playing ? "play" : "stop", t);
        }
        return next;
      }

      if (c === "function") {
        // Tempo tapping is NOT the engine's multi-tap: at 120 BPM the gap is
        // 500 ms, far outside the 300 ms multi-tap window, so `count` is always
        // 1 here. We count taps ourselves with an INACTIVITY timeout between
        // consecutive taps (TEMPO_TAP_IDLE_MS) — not a fixed first-to-fourth
        // window, so e.g. 104 BPM (576.9 ms gaps, 1730.8 ms total) is accepted.
        const prev = next.fnTapTimes;
        const inRhythm = prev.length > 0 && t - prev[prev.length - 1]! <= TEMPO_TAP_IDLE_MS;
        const times = (inRhythm ? [...prev, t] : [t]).slice(-4);
        const count = times.length;
        next = { ...next, fnTapTimes: times, fnTapCount: count };
        if (count === 4) {
          const gaps = times.slice(1).map((v, i) => v - times[i]!);
          const mean = gaps.reduce((a, b) => a + b, 0) / gaps.length;
          const bpm = 60000 / mean;
          const farOff = next.grid.bpm != null && Math.abs(bpm - next.grid.bpm) / next.grid.bpm > 0.25;
          if (farOff) {
            next = { ...next, grid: { ...next.grid, rejected: true } };
            return fire(
              next,
              "fn.gridReject",
              `tapped ${bpm.toFixed(1)} vs grid ${next.grid.bpm!.toFixed(1)} — all four blink, nothing moves`,
              t,
            );
          }
          next = { ...next, grid: { bpm, rejected: false, source: "tapped" } };
          return fire(next, "fn.tempoGrid", `grid = ${bpm.toFixed(1)} BPM`, t);
        }
        const held = next.grid.bpm;
        if (held != null) {
          next = { ...next, grid: { ...next.grid, source: "beatmatched", rejected: false } };
          return fire(next, "fn.beatmatch", `re-tap over loops · ${held.toFixed(1)} BPM held`, t);
        }

        return next;
      }



      if (c.startsWith("track-button")) {
        const i = trackIndexOf(c);
        const slice = next.tracks[i]!;

        // Heads mappings claim Track gestures before recording, mute/delete and
        // bank navigation (§3.3). The FX overlay still wins for FX buttons.
        if (next.headsMode && !next.perf.fxOverlay) {
          if (g.count === 2) {
            const rev = !slice.headReverse;
            next = { ...next, tracks: setTrack(next, i, { headReverse: rev }) };
            next = emit(next, "heads.reverse", { head: i, reverse: rev }, { rowId: "heads.reverse", t });
            return fire(next, "heads.reverse", `head ${i + 1} → ${rev ? "reverse" : "forward"}`, t);
          }
          const muted = !slice.headMuted;
          next = { ...next, tracks: setTrack(next, i, { headMuted: muted }) };
          next = emit(next, "heads.mute", { head: i, muted }, { rowId: "heads.mute", t });
          return fire(next, "heads.mute", `head ${i + 1} ${muted ? "muted" : "unmuted"}`, t);
        }

        if (fn) {
          // FN + track = banks: jump · tap again = next song
          if (g.count === 1) {
            next = { ...next, bank: i, bankJumpArmed: true, activeTrack: i };
            return fire(next, "track.bank", `bank jump → bank ${i + 1}`, t);
          }
          const song = (next.bank * 4 + (((next.song % 4) + 1) % 4)) % 16;
          next = { ...loadSong(next, song), bankJumpArmed: true };
          // P4 song load: stop the transport, hold the position, wait for Play.
          next = emit({ ...next, playing: false }, "song.load", { song }, { rowId: "track.bank", t });
          return fire(next, "track.bank", `tap again → next song (${song + 1}/16) — transport stopped, waiting for PLAY`, t);
        }

        next = { ...next, activeTrack: i };

        // ---- approved bare-Track state table (§2.1) -------------------------
        // Double-tap NEVER deletes. Delete has been removed from the surface
        // entirely; double-tap is reserved for `loop.capture` (Step 7).
        if (g.count === 2) {
          return fire(next, "track.tap", `track ${i + 1} double-tap → loop capture (engine lands in Step 7); delete removed from the surface`, t);
        }


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
          if (g.count === 2) {
            next = { ...next, chopDiv: 1, chopWindowOffset: 0, chopGlide: false };
            for (let i = 0; i < 4; i++) next = emit(next, "loop.chop", { track: i, div: 1, index: 0 }, { rowId: "rocker.chop.play", t });
            return fire(next, "rocker.chop.play", "chop reset → 1/1 (transport untouched)", t);
          }
          const chopDiv = Math.min(16, Math.max(1, dir > 0 ? next.chopDiv * 2 : next.chopDiv / 2));
          next = { ...next, chopDiv };
          for (let i = 0; i < 4; i++) next = emit(next, "loop.chop", { track: i, div: chopDiv, index: 0 }, { rowId: "rocker.chop.play", t });
          return fire(next, "rocker.chop.play", `chop ${dir > 0 ? "double" : "half"} → 1/${chopDiv} (transport untouched)`, t);
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
        if (fn) {
          const chopWindowOffset = clamp01(next.chopWindowOffset + dir * 0.0625);
          next = { ...next, chopWindowOffset };
          return fire(next, "volume.chopWindow", `chop window → ${chopWindowOffset.toFixed(3)}`, t);
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
        if (g.level === "hold" && !fn)
          // Correction 3: Hold Play is the UTILITY cue — 8 ms anti-click fade,
          // sources stopped, every stem parked on frame zero. Tap Play stays the
          // musical wind-up / wind-down control.
          return fire(
            emit({ ...next, playing: false }, "transport.cue", { frame: 0 }, { rowId: "play.cue", t }),
            "play.cue",
            "cued at frame 0 — next Play tap launches EXACT",
            t,
          );
        return next;
      }
      if (c.startsWith("track-button") && g.level === "hold") {
        const i = trackIndexOf(c);
        const slice = next.tracks[i]!;

        if (state.headsMode && !state.perf.fxOverlay) {
          // §3.3: a loaded track becomes the heads SOURCE. PRINT is removed.
          if (slice.content === "empty")
            return fire(next, "heads.source", `track ${i + 1} is empty — nothing to feed the heads`, t);
          next = { ...next, headsSource: i };
          next = emit(next, "heads.source", { track: i }, { rowId: "heads.source", t });
          return fire(next, "heads.source", `track ${i + 1} is now the heads source`, t);
        }

        return fire(next, "track.hold", `track ${i + 1} hold — live input recording has been removed`, t);
      }


      if ((c === "rocker-fwd" || c === "rocker-rwd") && g.level === "hold") {
        // Hold PLAY + rocker = chop glide. FN + rocker is the audible shuttle,
        // which owns its own hold and must not also start a glide.
        if (state.pressed.includes("play")) return fire({ ...next, chopGlide: true }, "rocker.chop.play", "hold PLAY + rocker = chop glide", t);
        if (fn) return next;
        return fire({ ...next, speedGlide: true }, "rocker.speed", "hold = continuous speed glide", t);
      }

      if ((c === "volume-plus" || c === "volume-minus") && g.level === "hold" && fn) {
        return fire({ ...next, chopGlide: true }, "volume.chopWindow", "hold = glide", t);
      }

      return next;
    }

    case "holdEnd": {
      if (g.control.startsWith("rocker") || g.control.startsWith("volume")) {
        return { ...next, speedGlide: false, chopGlide: false };
      }
      return next;
    }

    case "tapThenHold": {
      if (g.control === "function") {
        // This hold belongs to the grid rows, not to power.
        next = { ...next, fnModifierUsed: true };
        if (state.fnTapCount >= 4 && state.grid.bpm != null) {
          const bpm = Math.round(state.grid.bpm);
          next = { ...next, grid: { bpm, rejected: false, source: "rounded" } };
          return fire(next, "fn.roundBpm", `rounded to ${bpm} BPM`, t);
        }
        next = { ...next, grid: { bpm: null, rejected: false, source: "none" }, fnTapTimes: [] };
        return fire(next, "fn.clearGrid", "grid cleared", t);
      }

      return next;
    }

    case "chordRelease": {
      const set = g.controls;
      if (set.includes("function") && set.includes("play") && g.releaseSpreadMs <= 120) {
        next = { ...next, loopMode: state.loopMode === "fixed" ? "variable" : "fixed" };
        return fire(next, "play.loopMode", `released together → ${next.loopMode} loops`, t);
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

/** Heads verdicts from the engine (entry rejection, source selection). */
export function applyHeadsFeedback(
  state: SurfaceState,
  patch: { active?: boolean; source?: number | null; printedTrack?: number },
): SurfaceState {
  let next: SurfaceState = { ...state };
  if (patch.active !== undefined) next = { ...next, headsMode: patch.active };
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
  claimed?: "headScrub" | "headLevel" | "window" | "fader",
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
      next = emit({ ...next, perf: { ...perf, fxOverlay: intent.on } }, "fx.overlay", { on: intent.on }, { rowId: "fx.overlay.toggle", t });
      return fire(next, "fx.overlay.toggle", `FX overlay ${intent.on ? "open" : "closed"} — tape audio continues`, t);
    }
    case "system.pairing":
      return fire(next, "system.pairing", "Bluetooth pairing gesture (stock)", t);
    case "system.noop":
      return fire(next, "system.volumechord.ambiguous", intent.detail, t);
    case "fx.bank.select": {
      const p = selectBank(perf, intent.stem, intent.bank);
      next = emit({ ...next, perf: p }, "fx.bank.select", { track: intent.stem, bank: intent.bank }, { rowId: "fx.bank.select", t });
      return fire(next, "fx.bank.select", `stem ${intent.stem + 1} bank ${BANKS[intent.bank]!.id} selected`, t);
    }
    case "fx.momentary.start":
    case "fx.momentary.end": {
      const on = intent.type === "fx.momentary.start";
      const p = setBankMomentary(perf, intent.stem, intent.bank, on);
      const bank = p.tracks[intent.stem]!.fx12.banks[intent.bank]!;
      const algo = algorithmDef(intent.bank, bank.selectedAlgorithm);
      next = emit(
        { ...next, perf: p },
        intent.type,
        { track: intent.stem, bank: intent.bank, algorithm: bank.selectedAlgorithm, latched: bank.latched },
        { rowId: `fx.${BANKS[intent.bank]!.id}.momentary`, t },
      );
      return fire(
        next,
        `fx.${BANKS[intent.bank]!.id}.momentary`,
        `stem ${intent.stem + 1} ${algo.label} ${on ? "engaged" : "released"}`,
        t,
      );
    }
    case "fx.algorithm.cycle": {
      const p = cycleBankAlgorithm(perf, intent.stem, intent.bank, intent.dir);
      const bank = p.tracks[intent.stem]!.fx12.banks[intent.bank]!;
      const algo = algorithmDef(intent.bank, bank.selectedAlgorithm);
      next = emit(
        { ...next, perf: p },
        "fx.algorithm.cycle",
        { track: intent.stem, bank: intent.bank, algorithm: bank.selectedAlgorithm, latched: bank.latched },
        { rowId: `fx.${BANKS[intent.bank]!.id}.algorithm`, t },
      );
      return fire(next, `fx.${BANKS[intent.bank]!.id}.algorithm`, `stem ${intent.stem + 1} → ${algo.label}`, t);
    }
    case "fx.macro": {
      const p = nudgeBankMacro(perf, intent.stem, intent.bank, intent.dir);
      const bank = p.tracks[intent.stem]!.fx12.banks[intent.bank]!;
      const value = bank.algorithms[bank.selectedAlgorithm]!.macroAmount;
      next = emit(
        { ...next, perf: p },
        "fx.macro",
        { track: intent.stem, bank: intent.bank, algorithm: bank.selectedAlgorithm, value },
        { rowId: `fx.${BANKS[intent.bank]!.id}.macro`, t },
      );
      return fire(
        next,
        `fx.${BANKS[intent.bank]!.id}.macro`,
        `stem ${intent.stem + 1} ${algorithmDef(intent.bank, bank.selectedAlgorithm).label} macro → ${value.toFixed(2)}`,
        t,
      );
    }
    case "fx.latch": {
      const p = toggleBankLatch(perf, intent.stem, intent.bank);
      const bank = p.tracks[intent.stem]!.fx12.banks[intent.bank]!;
      next = emit(
        { ...next, perf: p },
        "fx.latch",
        { track: intent.stem, bank: intent.bank, on: bank.latched, algorithm: bank.selectedAlgorithm, latched: bank.latched },
        { rowId: `fx.${BANKS[intent.bank]!.id}.latch`, t },
      );
      return fire(
        next,
        `fx.${BANKS[intent.bank]!.id}.latch`,
        `stem ${intent.stem + 1} ${algorithmDef(intent.bank, bank.selectedAlgorithm).label} ${bank.latched ? "latched" : "unlatched"}`,
        t,
      );
    }
    case "fx.clearLatches": {
      const p = clearLatches(perf, intent.stem);
      next = emit({ ...next, perf: p }, "fx.clearLatches", { track: intent.stem }, { rowId: "fx.clearLatches", t });
      return fire(next, "fx.clearLatches", `stem ${intent.stem + 1} latches cleared`, t);
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
      const isSource = state.headsSource === i;
      const loaded = track.content !== "empty";
      const dir = track.headReverse ? "reversed chase" : "chase";
      if (isSource) frame[id] = { pattern: "solid", reason: `heads source — track ${i + 1} feeds all four heads`, priority: LED_PRIORITY.heads + 1 };
      else if (track.headMuted) frame[id] = { pattern: "faint", reason: `head ${i + 1} muted (still a head, not an empty slot)`, priority: LED_PRIORITY.heads - 2 };
      else if (loaded) frame[id] = { pattern: "chase", reason: `head ${i + 1} ${dir} over loaded content`, priority: LED_PRIORITY.heads };
      else frame[id] = { pattern: "faint", reason: `head ${i + 1} hollow — empty track, available as a PRINT target`, priority: LED_PRIORITY.heads - 1 };
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
      // Side LEDs 1–4 = Filter / Echo / Reverb / Beat Repeat for the ACTIVE stem.
      const family = FX_FAMILIES[i] as FxFamily;
      const slot = state.perf.tracks[state.perf.activeStem]!.fx[family];
      if (slot.rejected)
        frame[id] = { pattern: "blink", reason: `${family} rejected: ${slot.rejected}`, priority: LED_PRIORITY.error };
      else if (slot.arming)
        frame[id] = { pattern: "blink", reason: `${family} arming — buffering one full division`, priority: LED_PRIORITY.error - 1 };
      else if (slot.momentary)
        frame[id] = { pattern: "breathe", reason: `${family} momentary (held)`, priority: LED_PRIORITY.momentaryFx };
      else if (slot.latched)
        frame[id] = { pattern: "solid", reason: `${family} latched`, priority: LED_PRIORITY.latchedFx };
      else frame[id] = { pattern: "dark", reason: `${family} inactive`, priority: LED_PRIORITY.base };
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

