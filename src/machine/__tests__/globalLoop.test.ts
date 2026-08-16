/**
 * Global one-bar loop acceptance (replaces the retired chop remap suite).
 *
 * Hold PLAY is transport-state dependent: playing = global loop, stopped = cue.
 * PLAY + rocker moves the loop window, FUNCTION + Volume sets its division, and
 * a FUNCTION tap during the hold latches it so PLAY may be released. The
 * arbiter still claims PLAY on the rocker deflection, so the transport can
 * never leak out of the gesture.
 */

import { describe, expect, it } from "vitest";
import {
  applyGesture,
  applyPerfIntent,
  initialSurfaceState,
  pressControl,
  releaseControl,
  TRACK_SELECT_ARM_MS,
  type SurfaceState,
} from "@/machine/surface";
import { ChordArbiter } from "@/machine/chordArbiter";
import type { Gesture } from "@/input/gestures";
import type { Control } from "@/device/geometry";

let clock = 0;
const tap = (control: Control, count = 1): Gesture => ({ type: "tap", control, count, t: (clock += 1000) });
const holdStart = (control: Control): Gesture => ({
  type: "holdStart",
  control,
  level: "hold",
  duration: 500,
  t: (clock += 1000),
});
const holdEnd = (control: Control): Gesture => ({
  type: "holdEnd",
  control,
  level: "hold",
  duration: 900,
  t: (clock += 1000),
});
const types = (s: SurfaceState) => s.commands.map((c) => c.type);

/** Transport running, PLAY physically down. */
function running() {
  let s = applyGesture(initialSurfaceState(), tap("play"));
  expect(s.playing).toBe(true);
  s = pressControl(s, "play");
  return s;
}

describe("Hold PLAY · global one-bar loop", () => {
  it("captures the global loop while playing and releases it with the button", () => {
    let s = applyGesture(running(), holdStart("play"));
    expect(s.globalLoop.active).toBe(true);
    expect(types(s)).toContain("loop.global.start");
    s = applyGesture(s, holdEnd("play"));
    expect(s.globalLoop.active).toBe(false);
    expect(types(s)).toContain("loop.global.release");
  });

  it("cues at frame 0 instead when the transport is stopped", () => {
    const s = applyGesture(pressControl(initialSurfaceState(), "play"), holdStart("play"));
    expect(s.globalLoop.active).toBe(false);
    expect(types(s)).toContain("transport.cue");
    expect(types(s)).not.toContain("loop.global.start");
  });

  it("latches on a FUNCTION tap during the hold and survives the PLAY release", () => {
    let s = applyGesture(running(), holdStart("play"));
    s = applyGesture(s, tap("function"));
    expect(s.globalLoop.latched).toBe(true);
    s = applyGesture(s, holdEnd("play"));
    expect(s.globalLoop.active).toBe(true);
    expect(types(s)).not.toContain("loop.global.release");
    // The next bare PLAY tap releases the latched loop; the song keeps running.
    s = releaseControl(s, "play");
    s = applyGesture(s, tap("play"));
    expect(s.globalLoop.active).toBe(false);
    expect(s.globalLoop.latched).toBe(false);
    expect(s.playing).toBe(true);
    expect(types(s)).toContain("loop.global.release");
  });

  it("PLAY + rocker moves the loop window and emits no transport or rate command", () => {
    let s = applyGesture(running(), holdStart("play"));
    const before = s.commands.length;
    s = applyGesture(s, tap("rocker-fwd"));
    s = applyGesture(s, tap("rocker-rwd"));
    const after = s.commands.slice(before).map((c) => c.type);
    expect(after.filter((x) => x === "loop.global.move")).toHaveLength(2);
    expect(after).not.toContain("rate.set");
    expect(after.filter((x) => x.startsWith("transport."))).toEqual([]);
    expect(s.speed).toBe(1);
  });

  it("FUNCTION + Volume steps the division 1 → 2 → 4 → 8 and clamps", () => {
    // Addendum §1: FUNCTION + Volume is contextual — it only owns the loop
    // division while a global loop is captured or latched.
    let s = applyGesture(running(), holdStart("play"));
    expect(s.globalLoop.active).toBe(true);
    s = pressControl(s, "function");
    s = applyGesture(s, tap("volume-plus"));
    expect(s.globalLoop.division).toBe(2);
    s = applyGesture(s, tap("volume-plus"));
    s = applyGesture(s, tap("volume-plus"));
    expect(s.globalLoop.division).toBe(8);
    s = applyGesture(s, tap("volume-plus"));
    expect(s.globalLoop.division).toBe(8);
    s = applyGesture(s, tap("volume-minus"));
    expect(s.globalLoop.division).toBe(4);
    // It works with no loop captured: the division is remembered for the next
    // Hold PLAY, and only a running loop is told to resize.
    expect(types(s)).not.toContain("loop.global.resize");
  });

  it("arbiter claims PLAY on the rocker deflection so the tap cannot fire", () => {
    const a = new ChordArbiter(() => ({ activeStem: 0, fxOverlay: false, selectedBank: null }));
    a.handle({ id: 1, pointerId: 1, control: "play", phase: "down", t: 0 });
    a.handle({ id: 2, pointerId: 2, control: "rocker-fwd", phase: "down", t: 60 });
    expect(a.isClaimed("play")).toBe(true);
    a.handle({ id: 3, pointerId: 2, control: "rocker-fwd", phase: "up", t: 120 });
    a.handle({ id: 4, pointerId: 1, control: "play", phase: "up", t: 200 });
    expect(a.log.some((e: { suppressed: Control[] }) => e.suppressed.includes("play"))).toBe(true);
  });
});

describe("Rocker while stopped is the song skip", () => {
  it("skips songs and cues at 0:00 instead of changing varispeed", () => {
    const s = applyGesture(initialSurfaceState(), tap("rocker-fwd"));
    expect(s.song).toBe(1);
    expect(s.speed).toBe(1);
    expect(types(s)).toContain("transport.cue");
    expect(types(s)).not.toContain("rate.set");
  });

  it("keeps varispeed on the bare rocker while the transport runs", () => {
    let s = applyGesture(initialSurfaceState(), tap("play"));
    s = applyGesture(s, tap("rocker-fwd"));
    expect(types(s)).toContain("rate.set");
    s = applyGesture(s, holdStart("rocker-fwd"));
    expect(s.speedGlide).toBe(true);
  });
});

describe("FUNCTION tap · armed active-track selection", () => {
  it("emits ONLY stem.select — no mute, loop, audition or FX action", () => {
    let s = applyGesture(initialSurfaceState(), tap("function"));
    expect(s.trackSelectArmedAt).not.toBeNull();
    const before = s.commands.length;
    s = applyGesture(s, tap("track-button-3"));
    const emitted = s.commands.slice(before).map((c) => c.type);
    expect(emitted).toEqual(["stem.select"]);
    expect(s.activeTrack).toBe(2);
    expect(s.tracks[2]!.content).toBe(initialSurfaceState().tracks[2]!.content);
    expect(s.trackSelectArmedAt).toBeNull();
  });

  it("expires: a Track tap after the arm window is a normal mute", () => {
    let s = applyGesture(initialSurfaceState(), tap("function"));
    clock += TRACK_SELECT_ARM_MS + 500;
    const before = s.commands.length;
    s = applyGesture(s, tap("track-button-1"));
    const emitted = s.commands.slice(before).map((c) => c.type);
    expect(emitted).not.toContain("stem.select");
    expect(s.trackSelectArmedAt).toBeNull();
  });
});

describe("FUNCTION + PLAY ×1 is half speed", () => {
  it("toggles 0.5× and back, and ×2 still snaps to 1.0×", () => {
    let s = applyGesture(running(), { type: "tap", control: "play", count: 1, t: (clock += 1000), qualified: true });
    expect(s.speed).toBeCloseTo(0.5, 6);
    s = applyGesture(s, { type: "tap", control: "play", count: 1, t: (clock += 1000), qualified: true });
    expect(s.speed).toBeCloseTo(1, 6);
  });
});

describe("FX overlay survives an active-stem change", () => {
  it("keeps fxOverlay open when the active stem is switched", () => {
    let s = applyPerfIntent(initialSurfaceState(), { type: "fx.overlay", on: true, scope: "stem" });
    s = applyPerfIntent(s, { type: "stem.select", dir: 1 });
    expect(s.perf.fxOverlay).toBe(true);
    expect(s.activeTrack).toBe(1);
  });
});
