import type { Control, TrackIndex } from "@/device/geometry";

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

export type TrackContent = "empty" | "armed" | "recording" | "loaded" | "muted";

export interface TrackSlice {
  content: TrackContent;
  volume: number;
  stem: "vocals" | "drums" | "bass" | "instruments";
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
  /** Phase 2 only: no transport engine yet, this is a label for diagnostics. */
  note: string;
}

export const STEM_ROLES = ["vocals", "drums", "bass", "instruments"] as const;

export function initialSurfaceState(): SurfaceState {
  return {
    power: "on",
    playing: false,
    functionHeld: false,
    pressed: [],
    tracks: [
      { content: "loaded", volume: 0.78, stem: "vocals" },
      { content: "loaded", volume: 0.72, stem: "drums" },
      { content: "loaded", volume: 0.65, stem: "bass" },
      { content: "loaded", volume: 0.7, stem: "instruments" },
    ],
    activeTrack: 0,
    masterVolume: 0.7,
    rocker: "center",
    lastGesture: null,
    note: "phase 2 — interaction surface only, no audio engine",
  };
}

/**
 * LED arbitration. The Tape Looper v2.6 meanings are the base layer
 * (dark = empty, faint = muted content, pulse = playing); anything added on top
 * declares a higher priority and says so in `reason`, so diagnostics can always
 * explain which state won and why. Brightness and animation only — no new
 * colours are assumed to be possible on the hardware.
 */
export function deriveLeds(state: SurfaceState): LedFrame {
  const frame = {} as LedFrame;

  state.tracks.forEach((track, i) => {
    const id = `track-led-${i + 1}` as LedId;
    const control = `track-button-${i + 1}` as Control;
    if (state.pressed.includes(control)) {
      frame[id] = { pattern: "solid", reason: "button held (input feedback)", priority: 95 };
    } else if (track.content === "recording") {
      frame[id] = { pattern: "solid", reason: "recording", priority: 80 };
    } else if (track.content === "armed") {
      frame[id] = { pattern: "breathe", reason: "armed, waiting for first sound", priority: 70 };
    } else if (i === state.activeTrack && state.functionHeld) {
      frame[id] = { pattern: "solid", reason: "active track (stock track select)", priority: 40 };
    } else if (track.content === "empty") {
      frame[id] = { pattern: "dark", reason: "empty (v2.6 base)", priority: 0 };
    } else if (track.content === "muted") {
      frame[id] = { pattern: "faint", reason: "muted content (v2.6 base)", priority: 10 };
    } else if (state.playing) {
      frame[id] = { pattern: "pulse", reason: "playing — pulses on its own loop wrap (v2.6 base)", priority: 20 };
    } else {
      frame[id] = { pattern: "faint", reason: "loaded, stopped", priority: 10 };
    }
  });

  // Side LED column: song row. Solid = song, blink = bank (v2.6).
  for (let i = 0; i < 4; i++) {
    const id = `side-led-${i + 1}` as LedId;
    frame[id] =
      i === 0
        ? { pattern: "solid", reason: "song row: solid = song (v2.6)", priority: 20 }
        : { pattern: "dark", reason: "no song in this slot", priority: 0 };
  }

  frame["play-indicator"] = state.playing
    ? { pattern: "solid", reason: "transport running", priority: 30 }
    : { pattern: "faint", reason: "transport stopped", priority: 10 };

  frame["function-led-1"] = state.functionHeld
    ? { pattern: "solid", reason: "function held", priority: 60 }
    : { pattern: "dark", reason: "function idle", priority: 0 };

  frame["function-led-2"] = state.pressed.some((c) => c.startsWith("rocker"))
    ? { pattern: "blink", reason: "rocker engaged", priority: 50 }
    : { pattern: "dark", reason: "rocker centred", priority: 0 };

  return frame;
}
