/**
 * Addendum §0 + Heads Mode (Vocal-only) focused regression.
 *
 * Three separate contracts are pinned here, because they were previously
 * entangled in one PLAY/FUNCTION code path:
 *
 *   §0  a bare PLAY tap releases a LATCHED GLOBAL LOOP and nothing else;
 *       a bare FUNCTION tap is the only thing that unlatches the shuttle.
 *   §3  the four Heads sit exactly a quarter of the active cycle apart.
 *   §6  supporting Heads are bounded under the focused Head and the summed
 *       energy never exceeds unity.
 *   §5  head/stem faders are soft-takeover: the physical position is inert
 *       until it picks the stored value up.
 */

import { describe, expect, it } from "vitest";
import {
  applyFader,
  applyGesture,
  applyGlobalScrub,
  applyHeadsFeedback,
  initialSurfaceState,
  pressControl,
  releaseControl,
  PICKUP_WINDOW,
  type SurfaceState,
} from "@/machine/surface";
import type { Gesture } from "@/input/gestures";
import { headMixGains, quarterCycleHeadFrames, SUPPORT_HEAD_CAP } from "@/audio/heads";

let clock = 0;
const tap = (control: string, count = 1): Gesture =>
  ({ type: "tap", control, count, t: (clock += 1000) }) as Gesture;

/** PLAY held long enough to open a global loop, then latched with FUNCTION. */
function latchedGlobalLoop(): SurfaceState {
  let s = pressControl(initialSurfaceState(), "play");
  s = applyGesture(s, { type: "holdStart", control: "play", level: "hold", duration: 500, t: (clock += 1000) } as Gesture);
  expect(s.globalLoop.active).toBe(true);
  s = applyGesture(s, tap("function"));
  expect(s.globalLoop.latched).toBe(true);
  return releaseControl(s, "play");
}

describe("§0 · scrub versus loop release ownership", () => {
  it("a bare PLAY tap releases the latched global loop and leaves the shuttle alone", () => {
    let s = latchedGlobalLoop();
    s = applyGlobalScrub(s, 1);
    s = applyGesture(s, tap("function")); // latch the shuttle too
    expect(s.scrubLatched).toBe(true);

    s = applyGesture(s, tap("play"));
    expect(s.globalLoop.latched).toBe(false);
    expect(s.globalLoop.active).toBe(false);
    // PLAY did NOT touch the shuttle.
    expect(s.scrubLatched).toBe(true);
    expect(s.globalScrub).toBe(1);
  });

  it("a bare FUNCTION tap releases the latched shuttle only", () => {
    let s = applyGlobalScrub(initialSurfaceState(), -1);
    s = applyGesture(s, tap("function"));
    expect(s.scrubLatched).toBe(true);

    s = applyGesture(s, tap("function"));
    expect(s.scrubLatched).toBe(false);
    expect(s.globalScrub).toBe(0);
  });

  it("FUNCTION joined by another control is not a bare tap and never unlatches the shuttle", () => {
    let s = applyGlobalScrub(initialSurfaceState(), 1);
    s = applyGesture(s, tap("function"));
    expect(s.scrubLatched).toBe(true);

    s = pressControl(s, "volume-plus");
    s = applyGesture(s, tap("function"));
    expect(s.scrubLatched).toBe(true);
  });
});

describe("§3 · exact quarter-cycle Head placement", () => {
  it("places the heads at 0 / 25 / 50 / 75 percent of the active cycle", () => {
    const start = 1000;
    const len = 4000;
    expect(quarterCycleHeadFrames(start, start, len)).toEqual([1000, 2000, 3000, 4000]);
    // Mid-cycle: head 1 is exactly where the vocal is playing, the rest wrap.
    expect(quarterCycleHeadFrames(3500, start, len)).toEqual([3500, 4500, 1500, 2500]);
  });
});

describe("§6 · bounded multi-Head mixing", () => {
  it("caps supporting heads under the focused head", () => {
    const g = headMixGains(
      [
        { level: 1, muted: false },
        { level: 1, muted: false },
        { level: 1, muted: false },
        { level: 1, muted: true },
      ],
      0,
    );
    expect(g[0]!).toBeGreaterThan(g[1]!);
    expect(g[1]!).toBeLessThanOrEqual(SUPPORT_HEAD_CAP + 1e-9);
    expect(g[3]!).toBe(0);
    const energy = Math.sqrt(g.reduce((a, x) => a + x * x, 0));
    expect(energy).toBeLessThanOrEqual(1 + 1e-9);
  });

  it("entering with only head 1 audible sounds exactly like the dry vocal", () => {
    const g = headMixGains(
      [
        { level: 1, muted: false },
        { level: 0.8, muted: true },
        { level: 0.8, muted: true },
        { level: 0.8, muted: true },
      ],
      0,
    );
    expect(g).toEqual([1, 0, 0, 0]);
  });
});

describe("§5 · fader soft takeover", () => {
  it("ignores the fader until it picks the stored head level up", () => {
    let s = applyHeadsFeedback(initialSurfaceState(), { active: true });
    const stored = s.tracks[0]!.headLevel;
    const n = s.commands.length;

    // Far away: inert, and nothing is emitted.
    s = applyFader(s, 0, 0.1, "headLevel");
    expect(s.tracks[0]!.headLevel).toBe(stored);
    expect(s.commands.length).toBe(n);

    // Within the pickup window: the fader takes over from here on.
    s = applyFader(s, 0, stored - PICKUP_WINDOW / 2, "headLevel");
    expect(s.tracks[0]!.headLevel).toBeCloseTo(stored - PICKUP_WINDOW / 2, 6);
    expect(s.tracks[0]!.headPickup).toBe(false);
    s = applyFader(s, 0, 0.1, "headLevel");
    expect(s.tracks[0]!.headLevel).toBeCloseTo(0.1, 6);
  });
});
