import type { Control, TrackIndex } from "@/device/geometry";
import type { Gesture } from "@/input/gestures";
import { V26_ROW_BY_ID } from "@/machine/v26map";

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

export type TrackContent = "empty" | "armed" | "recording" | "loaded" | "muted" | "printing";

export interface TrackSlice {
  content: TrackContent;
  volume: number;
  stem: "vocals" | "drums" | "bass" | "instruments";
  /** Heads mode: per-head scrub position 0..1. */
  headPos: number;
  headReverse: boolean;
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
  lights: "full" | "dim";
  song: number;
  bank: number;
  bankJumpArmed: boolean;
  grid: { bpm: number | null; rejected: boolean; source: "none" | "tapped" | "beatmatched" | "rounded" };
  fnTapTimes: number[];
  fnTapCount: number;
  /** FUNCTION hold has crossed 450 ms and no other control has been touched. */
  fnHoldReached: boolean;
  /** FUNCTION was used as a modifier during this hold — so it must NOT power-toggle. */
  fnModifierUsed: boolean;
  /**
   * Taps fire optimistically at ×1 and revise upward. When ×2 arrives the ×1
   * effect has to be rolled back before the ×2 row runs, or a double-tap leaves
   * the ×1 side effect behind.
   */
  pendingUndo: { control: Control; tracks: SurfaceState["tracks"]; speed: number; chopDiv: number } | null;

  fired: FiredRow[];
  coverage: Record<string, number>;
  note: string;
}

export const STEM_ROLES = ["vocals", "drums", "bass", "instruments"] as const;

const FIRED_LIMIT = 60;
let firedSeq = 0;

function track(stem: TrackSlice["stem"], volume: number): TrackSlice {
  return { content: "loaded", volume, stem, headPos: 0, headReverse: false };
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
    lights: "full",
    song: 0,
    bank: 0,
    bankJumpArmed: false,
    grid: { bpm: null, rejected: false, source: "none" },
    fnTapTimes: [],
    fnTapCount: 0,
    fnHoldReached: false,
    fnModifierUsed: false,
    pendingUndo: null,

    fired: [],
    coverage: {},
    note: "phase 3 — Tape Looper v2.6 behavioural simulation, no audio engine",
  };
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

/** ---------------- v2.6 gesture → command dispatch ---------------- */

export function applyGesture(state: SurfaceState, g: Gesture): SurfaceState {
  let next: SurfaceState = { ...state };
  const fn = state.functionHeld;
  const t = g.t;

  // Power is a v2.6 row: FUNCTION hold. It is armed here and only commits on
  // FUNCTION release (see releaseControl) — otherwise every FN + X combo, which
  // necessarily holds FUNCTION past 450 ms, would power the unit down.
  if (g.type === "holdStart" && g.control === "function" && g.duration === 450) {
    return { ...next, fnHoldReached: true };
  }
  // While off, nothing but that row responds.
  if (state.power === "off") return next;

  switch (g.type) {
    case "tap": {
      const c = g.control;

      // Roll back the optimistic ×1 effect before running the ×2 / ×3 row.
      if (g.count > 1 && state.pendingUndo && state.pendingUndo.control === c) {
        next = {
          ...next,
          tracks: state.pendingUndo.tracks,
          speed: state.pendingUndo.speed,
          chopDiv: state.pendingUndo.chopDiv,
        };
      } else if (g.count === 1) {
        next = {
          ...next,
          pendingUndo: { control: c, tracks: state.tracks, speed: state.speed, chopDiv: state.chopDiv },
        };
      }


      if (c === "play") {
        if (fn && g.count === 3) {
          next = { ...next, headsMode: !state.headsMode };
          next = fire(next, "play.heads", `heads mode ${next.headsMode ? "on" : "off"}`, t);
          return fire(next, "heads.toggle", `heads ${next.headsMode ? "on" : "off"}`, t);
        }
        if (fn && g.count === 2) {
          next = { ...next, speed: 1 };
          return fire(next, "play.snap", "speed snapped to 1.000×", t);
        }
        if (!fn && g.count === 1) {
          next = { ...next, playing: !state.playing };
          return fire(next, "play.toggle", next.playing ? "play" : "stop", t);
        }
        return next;
      }

      if (c === "function") {
        const times = [...state.fnTapTimes, t].slice(-4);
        next = { ...next, fnTapTimes: times, fnTapCount: g.count };
        if (g.count === 4 && times.length === 4) {
          const gaps = times.slice(1).map((v, i) => v - times[i]!);
          const mean = gaps.reduce((a, b) => a + b, 0) / gaps.length;
          const bpm = 60000 / mean;
          const farOff = state.grid.bpm != null && Math.abs(bpm - state.grid.bpm) / state.grid.bpm > 0.25;
          if (farOff) {
            next = { ...next, grid: { ...state.grid, rejected: true } };
            return fire(next, "fn.gridReject", `tapped ${bpm.toFixed(1)} vs grid ${state.grid.bpm!.toFixed(1)} — all four blink, nothing moves`, t);
          }
          next = { ...next, grid: { bpm, rejected: false, source: "tapped" } };
          return fire(next, "fn.tempoGrid", `grid = ${bpm.toFixed(1)} BPM`, t);
        }
        if (state.grid.bpm != null && g.count >= 1) {
          next = { ...next, grid: { ...state.grid, source: "beatmatched", rejected: false } };
          return fire(next, "fn.beatmatch", `re-tap over loops · ${state.grid.bpm.toFixed(1)} BPM held`, t);
        }
        return next;
      }

      if (c.startsWith("track-button")) {
        const i = trackIndexOf(c);
        const slice = next.tracks[i]!;

        if (fn) {
          // FN + track = banks: jump · tap again = next song
          if (g.count === 1) {
            next = { ...next, bank: i, bankJumpArmed: true, activeTrack: i };
            return fire(next, "track.bank", `bank jump → bank ${i + 1}`, t);
          }
          const song = (state.bank * 4 + ((state.song % 4) + 1) % 4) % 16;
          next = { ...next, song, bankJumpArmed: true };
          return fire(next, "track.bank", `tap again → next song (${song + 1}/16)`, t);
        }
        if (g.count === 2) {
          next = { ...next, tracks: setTrack(next, i, { content: "empty" }), activeTrack: i };
          return fire(next, "track.delete", `track ${i + 1} deleted`, t);
        }
        next = { ...next, activeTrack: i };
        if (slice.content === "recording" || slice.content === "armed") {
          next = { ...next, tracks: setTrack(next, i, { content: "loaded" }) };
          return fire(next, "track.tap", `track ${i + 1} — stop the take`, t);
        }
        if (slice.content === "empty") return fire(next, "track.tap", `track ${i + 1} empty — nothing to stop or mute`, t);
        const muted = slice.content === "muted";
        next = { ...next, tracks: setTrack(next, i, { content: muted ? "loaded" : "muted" }) };
        return fire(next, "track.tap", `track ${i + 1} ${muted ? "unmute" : "mute"}`, t);
      }

      if (c === "rocker-fwd" || c === "rocker-rwd") {
        const dir = c === "rocker-fwd" ? 1 : -1;
        if (fn) {
          if (g.count === 2) {
            next = { ...next, chopDiv: 1 };
            return fire(next, "rocker.chopReset", "chop reset → 1", t);
          }
          const chopDiv = Math.min(16, Math.max(1, dir > 0 ? state.chopDiv * 2 : state.chopDiv / 2));
          next = { ...next, chopDiv };
          return fire(next, "rocker.chop", `chop ${dir > 0 ? "double" : "half"} → 1/${chopDiv}`, t);
        }
        if (g.count === 2) {
          const speed = state.speed * Math.pow(2, dir / 12);
          next = { ...next, speed };
          return fire(next, "rocker.semitone", `exact semitone ${dir > 0 ? "+1" : "−1"} → ${speed.toFixed(4)}×`, t);
        }
        const bpmBase = state.grid.bpm ?? 120;
        const speed = state.speed * ((bpmBase + dir) / bpmBase);
        next = { ...next, speed };
        return fire(next, "rocker.speed", `${dir > 0 ? "+" : "−"}1 BPM → ${speed.toFixed(4)}×`, t);
      }

      if (c === "volume-plus" || c === "volume-minus") {
        const dir = c === "volume-plus" ? 1 : -1;
        if (fn) {
          const chopWindowOffset = clamp01(state.chopWindowOffset + dir * 0.0625);
          next = { ...next, chopWindowOffset };
          return fire(next, "volume.chopWindow", `chop window → ${chopWindowOffset.toFixed(3)}`, t);
        }
        const masterVolume = clamp01(state.masterVolume + dir * 0.0625);
        next = { ...next, masterVolume };
        return fire(next, "volume.master", `master → ${masterVolume.toFixed(3)}`, t);
      }

      if (c.startsWith("fader-") && g.count === 2 && state.headsMode) {
        const i = trackIndexOf(c);
        const rev = !state.tracks[i]!.headReverse;
        next = { ...next, tracks: setTrack(next, i, { headReverse: rev }) };
        return fire(next, "heads.scrub", `head ${i + 1} double-tap → ${rev ? "reverse" : "forward"}`, t);
      }
      return next;
    }

    case "holdStart": {
      const c = g.control;
      if (c === "play") {
        if (g.duration === 5000) {
          next = { ...next, lights: state.lights === "full" ? "dim" : "full" };
          return fire(next, "play.lights", `lights ${next.lights}`, t);
        }
        if (!fn) return fire({ ...next, playing: true }, "play.restart", "restart from the top", t);
        return next;
      }
      if (c === "function" && g.duration === 450) {
        next = { ...next, power: "off", playing: false };
        return fire(next, "fn.power", "power off", t);
      }
      if (c.startsWith("track-button")) {
        const i = trackIndexOf(c);
        const slice = state.tracks[i]!;
        if (state.headsMode) {
          const printing = slice.content === "empty";
          next = { ...next, tracks: setTrack(next, i, { content: printing ? "printing" : slice.content }) };
          return fire(next, "heads.print", `track ${i + 1} ${printing ? "PRINT" : "tape (loaded)"}`, t);
        }
        next = { ...next, tracks: setTrack(next, i, { content: "armed" }), activeTrack: i };
        return fire(next, "track.record", `track ${i + 1} armed — records on your first sound`, t);
      }
      if ((c === "rocker-fwd" || c === "rocker-rwd") && g.duration === 450) {
        if (fn) return fire({ ...next, chopGlide: true }, "rocker.chop", "hold = glide (chop glides)", t);
        return fire({ ...next, speedGlide: true }, "rocker.speed", "hold = continuous speed glide", t);
      }
      if ((c === "volume-plus" || c === "volume-minus") && g.duration === 450 && fn) {
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

/** Fader commit routing per v2.6: FN layer, heads layer, otherwise track volume. */
export function applyFader(state: SurfaceState, index: number, value: number): SurfaceState {
  const t = performance.now();
  if (state.functionHeld) {
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
  if (state.headsMode) {
    return fire(
      { ...state, tracks: setTrack(state, index, { headPos: value }) },
      "fader.headScrub",
      `head ${index + 1} scrub → ${value.toFixed(3)}`,
      t,
    );
  }
  return fire(
    { ...state, tracks: setTrack(state, index, { volume: value }) },
    "fader.trackVolume",
    `track ${index + 1} volume → ${value.toFixed(3)}`,
    t,
  );
}

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
    } else if (state.pressed.includes(control)) {
      frame[id] = { pattern: "solid", reason: "button held (input feedback)", priority: 95 };
    } else if (track.content === "printing") {
      frame[id] = { pattern: "chase", reason: "PRINT (heads mode, empty track)", priority: 85 };
    } else if (track.content === "recording") {
      frame[id] = { pattern: "solid", reason: "recording", priority: 80 };
    } else if (track.content === "armed") {
      frame[id] = { pattern: "breathe", reason: "armed — record starts on your first sound (v2.6)", priority: 70 };
    } else if (track.content === "empty") {
      frame[id] = { pattern: "dark", reason: "dark = empty (v2.6)", priority: 0 };
    } else if (track.content === "muted") {
      frame[id] = { pattern: "faint", reason: "faint = muted content (v2.6)", priority: 10 };
    } else if (state.playing) {
      frame[id] = { pattern: "pulse", reason: "pulse = playing — pulses on its own loop wrap (v2.6)", priority: 20 };
    } else {
      frame[id] = { pattern: "faint", reason: "loaded, stopped", priority: 10 };
    }
  });

  // Song row: solid = song · blink = bank (v2.6).
  for (let i = 0; i < 4; i++) {
    const id = `side-led-${i + 1}` as LedId;
    if (off) frame[id] = { pattern: "dark", reason: "powered off", priority: 100 };
    else if (state.bankJumpArmed && i === state.bank)
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
    : state.functionHeld
      ? { pattern: "solid", reason: "function held", priority: 60 }
      : state.headsMode
        ? { pattern: "breathe", reason: "heads mode on", priority: 45 }
        : { pattern: "dark", reason: "function idle", priority: 0 };

  frame["function-led-2"] = off
    ? { pattern: "dark", reason: "powered off", priority: 100 }
    : state.pressed.some((c) => c.startsWith("rocker"))
      ? { pattern: "blink", reason: "rocker engaged", priority: 50 }
      : state.grid.bpm != null
        ? { pattern: "pulse", reason: `tempo grid ${state.grid.bpm.toFixed(1)} BPM (metronome)`, priority: 35 }
        : { pattern: "dark", reason: "rocker centred, no grid", priority: 0 };

  // Lights dim/full is a global brightness layer, not a pattern change.
  return frame;
}
