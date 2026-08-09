/**
 * Corrective production task — Track performance controls and the universal
 * lane layer.
 *
 * These tests exercise the REAL runtime objects (GestureEngine + surface
 * reducer + command stream), not a description of them:
 *
 *  1. Deferred Track arbitration: a double-tap NEVER emits an intermediate ×1,
 *     so no mute is heard before the loop capture. A single tap still resolves
 *     inside the approved 180–220 ms band.
 *  2. Crossing the hold threshold cancels the pending decision entirely — a
 *     momentary audition is not also a mute.
 *  3. Momentary audition is non-destructive: mute and latched-solo state is
 *     byte-identical before and after the hold.
 *  4. Universal lane commands: FUNCTION + double-tap = lane.reverse,
 *     bare double-tap = loop.capture / loop.release,
 *     FUNCTION + Track held + Volume ± = loop.resize.
 *  5. `heads.reverse` no longer exists anywhere in the command stream.
 */

import { describe, expect, it, vi } from "vitest";
import { DEFAULT_TIMINGS, GestureEngine, isDeferredControl, type Gesture } from "@/input/gestures";
import { applyGesture, initialSurfaceState, pressControl, releaseControl, type SurfaceState } from "@/machine/surface";

const T = "track-button-1" as const;

function collect(engine: GestureEngine): Gesture[] {
  const out: Gesture[] = [];
  engine.onGesture((g) => out.push(g));
  return out;
}

function types(s: SurfaceState, from = 0) {
  return s.commands.slice(from).map((c) => c.type);
}

describe("deferred Track arbitration", () => {
  it("emits exactly one count=2 tap for a double-tap — no optimistic ×1", () => {
    vi.useFakeTimers();
    const e = new GestureEngine();
    const seen = collect(e);
    e.press(T, 1, 0);
    e.release(T, 1, 40);
    vi.advanceTimersByTime(80); // inside the decision window
    e.press(T, 1, 120);
    e.release(T, 1, 160);
    vi.advanceTimersByTime(500);
    const taps = seen.filter((g) => g.type === "tap");
    expect(taps).toHaveLength(1);
    expect(taps[0]).toMatchObject({ type: "tap", count: 2 });
    vi.useRealTimers();
  });

  it("confirms a single tap after the decision window, which sits in 180–220 ms", () => {
    expect(DEFAULT_TIMINGS.trackDecisionMs).toBeGreaterThanOrEqual(180);
    expect(DEFAULT_TIMINGS.trackDecisionMs).toBeLessThanOrEqual(220);
    vi.useFakeTimers();
    const e = new GestureEngine();
    const seen = collect(e);
    e.press(T, 1, 0);
    e.release(T, 1, 30);
    expect(seen.filter((g) => g.type === "tap")).toHaveLength(0);
    vi.advanceTimersByTime(DEFAULT_TIMINGS.trackDecisionMs + 1);
    expect(seen.filter((g) => g.type === "tap")).toMatchObject([{ count: 1 }]);
    vi.useRealTimers();
  });

  it("only Track buttons are deferred — Play stays optimistic", () => {
    expect(isDeferredControl("track-button-3")).toBe(true);
    expect(isDeferredControl("play")).toBe(false);
    vi.useFakeTimers();
    const e = new GestureEngine();
    const seen = collect(e);
    e.press("play", 1, 0);
    e.release("play", 1, 20);
    expect(seen.filter((g) => g.type === "tap")).toMatchObject([{ count: 1 }]);
    vi.useRealTimers();
  });

  it("a hold cancels the pending decision — audition is not also a mute", () => {
    vi.useFakeTimers();
    const e = new GestureEngine();
    const seen = collect(e);
    e.press(T, 1, 0);
    vi.advanceTimersByTime(DEFAULT_TIMINGS.holdMs + 5);
    e.release(T, 1, DEFAULT_TIMINGS.holdMs + 10);
    vi.advanceTimersByTime(1000);
    expect(seen.filter((g) => g.type === "tap")).toHaveLength(0);
    expect(seen.filter((g) => g.type === "holdStart")).toHaveLength(1);
    vi.useRealTimers();
  });
});

describe("momentary audition is non-destructive", () => {
  it("emits a mask on hold and an empty mask on release, leaving mix state untouched", () => {
    let s = initialSurfaceState();
    const before = JSON.stringify({ tracks: s.tracks });
    const mark = s.commands.length;
    s = applyGesture(s, { type: "holdStart", control: "track-button-2", level: "hold", duration: 450, t: 100 });
    s = applyGesture(s, { type: "holdEnd", control: "track-button-2", level: "hold", duration: 800, t: 900 });
    const cmds = s.commands.slice(mark).filter((c) => c.type === "lane.audition");
    expect(cmds.map((c) => c.payload["mask"])).toEqual(["0100", ""]);
    expect(JSON.stringify({ tracks: s.tracks })).toBe(before);
  });
});

describe("universal lane layer", () => {
  it("bare double-tap captures, then releases, the lane loop — never deletes", () => {
    let s = initialSurfaceState();
    let mark = s.commands.length;
    s = applyGesture(s, { type: "tap", control: "track-button-1", count: 2, t: 10 });
    expect(types(s, mark)).toEqual(["loop.capture"]);
    expect(s.tracks[0]!.laneLoop.active).toBe(true);
    mark = s.commands.length;
    s = applyGesture(s, { type: "tap", control: "track-button-1", count: 2, t: 20 });
    expect(types(s, mark)).toEqual(["loop.release"]);
    expect(s.tracks[0]!.content).not.toBe("empty");
  });

  it("FUNCTION + double-tap is lane.reverse in every layer", () => {
    let s = pressControl(initialSurfaceState(), "function");
    const mark = s.commands.length;
    s = applyGesture(s, { type: "tap", control: "track-button-3", count: 2, t: 30 });
    expect(types(s, mark)).toEqual(["lane.reverse"]);
    expect(s.tracks[2]!.laneReverse).toBe(true);

    let h = pressControl({ ...initialSurfaceState(), headsMode: true }, "function");
    const hmark = h.commands.length;
    h = applyGesture(h, { type: "tap", control: "track-button-3", count: 2, t: 40 });
    expect(types(h, hmark)).toEqual(["lane.reverse"]);
  });

  it("PLAY + Track latches solo and reports the mask", () => {
    let s = pressControl(initialSurfaceState(), "play");
    const mark = s.commands.length;
    s = applyGesture(s, { type: "tap", control: "track-button-2", count: 1, t: 50 });
    expect(types(s, mark)).toEqual(["stem.solo"]);
    expect(s.commands.at(-1)!.payload["mask"]).toBe("0100");
    expect(s.tracks[1]!.soloLatched).toBe(true);
  });

  it("FUNCTION + Track held + Volume ± resizes the lane loop in bars", () => {
    let s = pressControl(initialSurfaceState(), "function");
    s = pressControl(s, "track-button-1");
    const mark = s.commands.length;
    s = applyGesture(s, { type: "tap", control: "volume-plus", count: 1, t: 60 });
    expect(types(s, mark)).toEqual(["loop.resize"]);
    expect(s.tracks[0]!.laneLoop.bars).toBe(2);
    s = applyGesture(s, { type: "tap", control: "volume-minus", count: 1, t: 70 });
    s = applyGesture(s, { type: "tap", control: "volume-minus", count: 1, t: 80 });
    expect(s.tracks[0]!.laneLoop.bars).toBe(0.5);
    s = releaseControl(s, "track-button-1");
    s = releaseControl(s, "function");
    expect(s.pressed).not.toContain("function");
  });

  it("no heads.reverse command can be produced any more", () => {
    let s = { ...initialSurfaceState(), headsMode: true };
    s = pressControl(s, "function");
    s = applyGesture(s, { type: "tap", control: "track-button-1", count: 2, t: 90 });
    expect(s.commands.some((c) => (c.type as string) === "heads.reverse")).toBe(false);
  });
});
