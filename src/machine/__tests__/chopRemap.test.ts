/**
 * P4 — chop remap acceptance (`rocker.chop.play`).
 *
 * Proves that PLAY + rocker produces an ordered `loop.chop` stream for all four
 * stems and that the transport is fully suppressed: the arbiter claims PLAY on
 * the rocker deflection, so no transport.play / transport.stop / transport.cue
 * can leak out of the gesture. Also pins P5: switching the active stem must not
 * close the FX overlay.
 */

import { describe, expect, it } from "vitest";
import { applyGesture, applyPerfIntent, initialSurfaceState, pressControl, releaseControl } from "@/machine/surface";
import { ChordArbiter } from "@/machine/chordArbiter";
import { STEM_TAPE_ROW_BY_ID } from "@/machine/stemTapeV1Map";
import type { Gesture } from "@/input/gestures";
import type { Control } from "@/device/geometry";

let clock = 0;
const tap = (control: "rocker-fwd" | "rocker-rwd", count = 1): Gesture => ({
  type: "tap",
  control,
  count,
  t: (clock += 1000),
});
const hold = (control: "rocker-fwd" | "rocker-rwd"): Gesture => ({
  type: "holdStart",
  control,
  level: "hold",
  duration: 500,
  t: (clock += 1000),
});

function withPlayHeld() {
  return pressControl(initialSurfaceState(), "play");
}

describe("P4 · PLAY + rocker = chop", () => {
  it("doubles the chop division and emits one loop.chop per stem", () => {
    let s = withPlayHeld();
    s = applyGesture(s, tap("rocker-fwd"));
    expect(s.chopDiv).toBe(2);
    const chops = s.commands.filter((c) => c.type === "loop.chop");
    expect(chops).toHaveLength(4);
    expect(chops.map((c) => c.payload["track"])).toEqual([0, 1, 2, 3]);
    expect(chops.every((c) => c.payload["div"] === 2)).toBe(true);
    // Ordered stream: sequence numbers strictly increase.
    const seqs = chops.map((c) => c.id);
    expect([...seqs].sort((a, b) => a - b)).toEqual(seqs);
  });

  it("halves on the reverse rocker and clamps at 1/1", () => {
    let s = withPlayHeld();
    s = applyGesture(s, tap("rocker-rwd"));
    expect(s.chopDiv).toBe(1);
    s = applyGesture(s, tap("rocker-fwd"));
    s = applyGesture(s, tap("rocker-fwd"));
    expect(s.chopDiv).toBe(4);
    s = applyGesture(s, tap("rocker-rwd"));
    expect(s.chopDiv).toBe(2);
  });

  it("resets to 1/1 on a rocker double-tap while PLAY is held", () => {
    let s = withPlayHeld();
    s = applyGesture(s, tap("rocker-fwd"));
    s = applyGesture(s, tap("rocker-fwd"));
    expect(s.chopDiv).toBe(4);
    s = applyGesture(s, tap("rocker-fwd", 2));
    expect(s.chopDiv).toBe(1);
    expect(s.chopWindowOffset).toBe(0);
  });

  it("starts a chop glide on hold, never a speed glide", () => {
    const s = applyGesture(withPlayHeld(), hold("rocker-fwd"));
    expect(s.chopGlide).toBe(true);
    expect(s.speedGlide).toBe(false);
  });

  it("emits no transport or rate command from the chop gesture", () => {
    let s = withPlayHeld();
    s = applyGesture(s, tap("rocker-fwd"));
    s = applyGesture(s, tap("rocker-rwd"));
    s = applyGesture(s, hold("rocker-fwd"));
    const types = s.commands.map((c) => c.type);
    expect(types.filter((t) => t.startsWith("transport."))).toEqual([]);
    expect(types).not.toContain("rate.set");
    expect(s.speed).toBe(1);
  });

  it("leaves bare-rocker varispeed untouched when PLAY is not held", () => {
    let s = applyGesture(initialSurfaceState(), tap("rocker-fwd"));
    expect(s.chopDiv).toBe(1);
    expect(s.commands.map((c) => c.type)).toContain("rate.set");
    s = applyGesture(s, hold("rocker-fwd"));
    expect(s.speedGlide).toBe(true);
    expect(s.chopGlide).toBe(false);
  });

  it("arbiter claims PLAY on the rocker deflection so the tap cannot fire", () => {
    const a = new ChordArbiter(() => ({ activeStem: 0, fxOverlay: false, selectedBank: null }));
    a.handle({ control: "play", phase: "down", t: 0 });
    a.handle({ control: "rocker-fwd", phase: "down", t: 60 });
    expect(a.isClaimed("play")).toBe(true);
    a.handle({ control: "rocker-fwd", phase: "up", t: 120 });
    a.handle({ control: "play", phase: "up", t: 200 });
    // The claim was recorded before dispatch — the log names the suppression.
    expect(a.log.some((e: { suppressed: Control[] }) => e.suppressed.includes("play"))).toBe(true);
  });

  it("registry row supersedes the v2.6 rocker.chop row", () => {
    const row = STEM_TAPE_ROW_BY_ID["rocker.chop.play"];
    expect(row).toBeDefined();
    expect(row?.supersedes).toContain("rocker.chop");
    expect(row?.suppresses).toEqual(expect.arrayContaining(["transport.play", "transport.stop"]));
    // FUNCTION + rocker keeps the shuttle; the two rows never share controls.
    expect(STEM_TAPE_ROW_BY_ID["rocker.scrub"]?.controls).toContain("function");
    expect(row?.controls).toContain("play");
  });
});

describe("P5 · FX overlay survives an active-stem change", () => {
  it("keeps fxOverlay open when the active stem is switched", () => {
    let s = applyPerfIntent(initialSurfaceState(), { type: "fx.overlay", on: true });
    expect(s.perf.fxOverlay).toBe(true);
    s = applyPerfIntent(s, { type: "stem.select", dir: 1 });
    expect(s.perf.fxOverlay).toBe(true);
    expect(s.activeTrack).toBe(1);
    s = applyPerfIntent(s, { type: "stem.select", dir: 1 });
    expect(s.perf.fxOverlay).toBe(true);
    expect(s.activeTrack).toBe(2);
    expect(s.commands.filter((c) => c.type === "fx.overlay")).toHaveLength(1);
  });
});

describe("PLAY release after a chop is inert", () => {
  it("releasing PLAY after the chop leaves the transport where it was", () => {
    let s = withPlayHeld();
    s = applyGesture(s, tap("rocker-fwd"));
    s = releaseControl(s, "play");
    expect(s.commands.map((c) => c.type).filter((t) => t.startsWith("transport."))).toEqual([]);
  });
});
