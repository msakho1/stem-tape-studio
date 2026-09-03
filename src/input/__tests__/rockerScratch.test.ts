import { describe, expect, it } from "vitest";
import { SCRATCH_TUNING, handVelocityToTapeVelocity } from "@/audio/masterScratch";
import {
  HandVelocityTracker,
  ROCKER_DRAG_RANGE,
  ROCKER_VISUAL_TRAVEL,
  displacementToVelocity,
  rockerDisplacement,
  rockerTransform,
  rockerVisualY,
} from "@/input/rockerScratch";

const T = SCRATCH_TUNING;
/** SVG units travelled in `ms` at `rate`× tape speed. */
const travelFor = (rate: number, ms: number) => (rate * T.handUnitsPerSecondAtUnitRate * ms) / 1000;

describe("S3 — audio velocity comes from HAND SPEED, not position", () => {
  it("maps upward hand speed to forward tape and downward to reverse", () => {
    expect(handVelocityToTapeVelocity(-travelFor(1, 20), 20)).toBeCloseTo(1, 9);
    expect(handVelocityToTapeVelocity(travelFor(1, 20), 20)).toBeCloseTo(-1, 9);
  });

  it("is proportional to hand speed: faster hand, faster tape", () => {
    const slow = handVelocityToTapeVelocity(-travelFor(0.5, 16), 16);
    const fast = handVelocityToTapeVelocity(-travelFor(2, 16), 16);
    expect(slow).toBeCloseTo(0.5, 9);
    expect(fast).toBeCloseTo(2, 9);
    expect(fast).toBeGreaterThan(slow);
  });

  it("a stationary hand is a stationary record, at ANY position", () => {
    expect(handVelocityToTapeVelocity(0, 16)).toBe(0);
    // Below the dead band = held still, not a crawl.
    expect(handVelocityToTapeVelocity(-0.05, 100)).toBe(0);
  });

  it("clamps at the centralized ceiling in both directions", () => {
    expect(handVelocityToTapeVelocity(-5000, 8)).toBe(T.maxAbsVelocity);
    expect(handVelocityToTapeVelocity(5000, 8)).toBe(-T.maxAbsVelocity);
  });

  it("reverses through zero continuously as the hand reverses", () => {
    const seen = [24, 12, 4, 0, -4, -12, -24].map((dy) => handVelocityToTapeVelocity(dy, 16));
    expect(seen[0]!).toBeLessThan(0);
    expect(seen[3]!).toBe(0);
    expect(seen[6]!).toBeGreaterThan(0);
    for (let i = 1; i < seen.length; i++) expect(seen[i]!).toBeGreaterThanOrEqual(seen[i - 1]!);
  });

  it("rejects non-finite input", () => {
    expect(handVelocityToTapeVelocity(Number.NaN, 16)).toBe(0);
    expect(handVelocityToTapeVelocity(10, Number.NaN)).toBe(0);
  });
});

describe("S3 — held-still pointer decays the command to zero", () => {
  it("commands 0 once the hand stops, within the configured timeout", () => {
    const h = new HandVelocityTracker(225, 0);
    expect(h.sample(225 - travelFor(1, 16), 16)).toBeCloseTo(1, 9);
    // Still inside the timeout: the record keeps moving.
    expect(h.stopIfIdle(16 + T.handStopTimeoutMs - 1)).toBe(false);
    expect(h.velocity).toBeCloseTo(1, 9);
    // Past it: the record stops under the hand.
    expect(h.stopIfIdle(16 + T.handStopTimeoutMs)).toBe(true);
    expect(h.velocity).toBe(0);
    // Reported exactly once per pause.
    expect(h.stopIfIdle(10_000)).toBe(false);
  });

  it("the stop timeout is short enough to feel physical", () => {
    expect(T.handStopTimeoutMs).toBeLessThanOrEqual(80);
  });

  it("a fresh grab starts at zero before any movement", () => {
    const h = new HandVelocityTracker(225, 0);
    expect(h.velocity).toBe(0);
    expect(h.sample(225, 16)).toBe(0);
  });

  it("rapid back-and-forth motion tracks the hand's sign every sample", () => {
    const h = new HandVelocityTracker(225, 0);
    const out: number[] = [];
    let y = 225;
    let t = 0;
    for (let i = 0; i < 8; i++) {
      const dy = i % 2 === 0 ? -travelFor(2, 12) : travelFor(2, 12);
      y += dy;
      t += 12;
      out.push(h.sample(y, t));
    }
    expect(out.filter((v) => v > 0).length).toBe(4);
    expect(out.filter((v) => v < 0).length).toBe(4);
    for (const v of out) expect(Math.abs(v)).toBeCloseTo(2, 6);
  });
});

describe("S3 — visual rocker travel (decoupled from audio)", () => {
  it("the grab point is visually neutral in either half", () => {
    expect(rockerDisplacement(225, 225)).toBe(0);
    expect(rockerDisplacement(205, 205)).toBe(0);
    expect(rockerDisplacement(245, 245)).toBe(0);
  });

  it("follows the finger and clamps at the finite physical travel", () => {
    expect(rockerDisplacement(225 - ROCKER_DRAG_RANGE / 2, 225)).toBeCloseTo(0.5, 12);
    expect(rockerDisplacement(225 + ROCKER_DRAG_RANGE / 2, 225)).toBeCloseTo(-0.5, 12);
    expect(rockerDisplacement(225 - 10 * ROCKER_DRAG_RANGE, 225)).toBe(1);
    expect(rockerDisplacement(225 + 10 * ROCKER_DRAG_RANGE, 225)).toBe(-1);
    expect(rockerDisplacement(Number.NaN, 225)).toBe(0);
  });

  it("translates vertically with the gesture", () => {
    expect(rockerVisualY(0)).toBe(-0);
    expect(rockerVisualY(1)).toBe(-ROCKER_VISUAL_TRAVEL);
    expect(rockerVisualY(-1)).toBe(ROCKER_VISUAL_TRAVEL);
    expect(rockerTransform(0.5)).toBe("translateY(-12.000px)");
  });

  it("displacement no longer drives audio: it is only the legacy shuttle map", () => {
    expect(displacementToVelocity(1)).toBe(T.maxAbsVelocity);
    expect(displacementToVelocity(-1)).toBe(-T.maxAbsVelocity);
    expect(displacementToVelocity(Number.NaN)).toBe(0);
  });
});
