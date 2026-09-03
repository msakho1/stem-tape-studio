import { describe, expect, it } from "vitest";
import {
  SCRATCH_TUNING,
  displacementToScrubVelocity,
  handVelocityToTapeVelocity,
} from "@/audio/masterScratch";
import {
  ScratchScrubController,
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
    const fast = handVelocityToTapeVelocity(-travelFor(1.5, 16), 16);
    expect(slow).toBeCloseTo(0.5, 9);
    expect(fast).toBeCloseTo(1.5, 9);
    expect(fast).toBeGreaterThan(slow);
  });

  it("a stationary hand is a stationary record, at ANY position", () => {
    expect(handVelocityToTapeVelocity(0, 16)).toBe(0);
    // Below the dead band = held still, not a crawl.
    expect(handVelocityToTapeVelocity(-0.05, 100)).toBe(0);
  });

  it("clamps at the centralized ceiling in both directions", () => {
    expect(handVelocityToTapeVelocity(-5000, 8)).toBe(T.scratchMaxVelocity);
    expect(handVelocityToTapeVelocity(5000, 8)).toBe(-T.scratchMaxVelocity);
  });

  it("reverses through zero continuously as the hand reverses", () => {
    const seen = [96, 48, 16, 0, -16, -48, -96].map((dy) => handVelocityToTapeVelocity(dy, 16));
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

describe("S3 — hybrid: scratch transient blends into sustained scrub", () => {
  const grab = 225;
  const held = (d: number) => grab - d * ROCKER_DRAG_RANGE;

  it("held-position scrub is nonlinear and fine near the centre", () => {
    expect(displacementToScrubVelocity(0)).toBe(0);
    expect(displacementToScrubVelocity(T.scrubDeadband)).toBe(0);
    expect(displacementToScrubVelocity(1)).toBeCloseTo(T.scrubMaxVelocity, 12);
    expect(displacementToScrubVelocity(-1)).toBeCloseTo(-T.scrubMaxVelocity, 12);
    const quarter = displacementToScrubVelocity(0.25);
    const half = displacementToScrubVelocity(0.5);
    expect(quarter).toBeGreaterThan(0);
    expect(quarter).toBeLessThan(0.25 * T.scrubMaxVelocity); // squared, not linear
    expect(half / quarter).toBeGreaterThan(2);
  });

  it("a flick decays into the sustained scrub the hold implies", () => {
    const c = new ScratchScrubController(grab, 0);
    const v0 = c.sample(held(0.6), 16); // quick pull upward
    expect(v0).toBeGreaterThan(displacementToScrubVelocity(0.6));
    // Finger now still, still held upward: the transient decays away.
    const settled = c.poll(16 + 8 * T.scratchDecayMs);
    expect(settled).toBeCloseTo(displacementToScrubVelocity(0.6), 9);
    expect(settled).toBeGreaterThan(0); // NOT zero — this is the scrub
  });

  it("held at centre, a motionless hand settles to a stopped record", () => {
    const c = new ScratchScrubController(grab, 0);
    c.sample(grab - 4, 16);
    expect(c.poll(16 + 8 * T.scratchDecayMs)).toBe(0);
  });

  it("held below centre sustains reverse scrub", () => {
    const c = new ScratchScrubController(grab, 0);
    c.sample(held(-0.7), 20);
    expect(c.poll(20 + 8 * T.scratchDecayMs)).toBeCloseTo(displacementToScrubVelocity(-0.7), 9);
  });

  it("rapid back-and-forth motion reverses sign continuously", () => {
    const c = new ScratchScrubController(grab, 0);
    const out: number[] = [];
    let y = grab;
    let t = 0;
    for (let i = 0; i < 8; i++) {
      y += i % 2 === 0 ? -travelFor(1, 12) : travelFor(1, 12);
      t += 12;
      out.push(c.sample(y, t));
    }
    expect(out.some((v) => v > 0)).toBe(true);
    expect(out.some((v) => v < 0)).toBe(true);
  });

  it("never reaches dog-whistle territory", () => {
    const c = new ScratchScrubController(grab, 0);
    let y = grab;
    let t = 0;
    for (let i = 0; i < 12; i++) {
      y -= 400; // absurd fling, well past the visual travel
      t += 8;
      expect(Math.abs(c.sample(y, t))).toBeLessThanOrEqual(T.combinedMaxVelocity);
    }
    expect(T.combinedMaxVelocity).toBeLessThanOrEqual(2.5);
    expect(T.scratchMaxVelocity).toBeLessThanOrEqual(1.75);
  });

  it("a fresh grab starts at zero before any movement", () => {
    const c = new ScratchScrubController(grab, 0);
    expect(c.velocity).toBe(0);
    expect(c.sample(grab, 16)).toBe(0);
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
