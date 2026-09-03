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
/** SVG units travelled in `ms` at a hand speed of `ups` units/second. */
const travel = (ups: number, ms: number) => (ups * ms) / 1000;
/** Units travelled in `ms` for a hand speed that yields `rate`× scratch velocity. */
const travelFor = (rate: number, ms: number) =>
  travel(Math.pow(rate / T.scratchMaxVelocity, 1 / T.scratchCurveExponent) * T.handUnitsPerSecondAtMaxScratch, ms);

describe("S3 — scratch velocity: heavy, curved, hard to saturate", () => {
  it("maps upward hand speed to forward tape and downward to reverse", () => {
    expect(handVelocityToTapeVelocity(-travelFor(0.5, 20), 20)).toBeCloseTo(0.5, 9);
    expect(handVelocityToTapeVelocity(travelFor(0.5, 20), 20)).toBeCloseTo(-0.5, 9);
  });

  it("is nonlinear: ordinary finger motion stays slow and does not saturate", () => {
    // A brisk 300 units/second swipe is ordinary on a touch screen.
    const ordinary = handVelocityToTapeVelocity(-travel(300, 16), 16);
    expect(ordinary).toBeGreaterThan(0);
    expect(ordinary).toBeLessThan(0.25 * T.scratchMaxVelocity);
    const half = handVelocityToTapeVelocity(-travel(450, 16), 16);
    expect(half).toBeLessThan(0.5 * T.scratchMaxVelocity); // curved, not linear
    expect(half).toBeGreaterThan(ordinary);
  });

  it("never exceeds scratchMaxAbsVelocity, and that ceiling is turntable-slow", () => {
    expect(T.scratchMaxVelocity).toBeLessThanOrEqual(1.0);
    expect(handVelocityToTapeVelocity(-5000, 8)).toBe(T.scratchMaxVelocity);
    expect(handVelocityToTapeVelocity(5000, 8)).toBe(-T.scratchMaxVelocity);
  });

  it("a stationary hand is a stationary record, at ANY position", () => {
    expect(handVelocityToTapeVelocity(0, 16)).toBe(0);
    expect(handVelocityToTapeVelocity(-0.05, 100)).toBe(0);
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

describe("S3 — scratch is the default; scrub is earned by a 4 s directional hold", () => {
  const grab = 225;
  const held = (d: number) => grab - d * ROCKER_DRAG_RANGE;

  /** Pull to `d` and then hold motionless, polling, until `untilMs`. */
  function pullAndHold(d: number, untilMs: number) {
    const c = new ScratchScrubController(grab, 0);
    c.sample(held(d), 60);
    let v = c.velocity;
    for (let t = 76; t <= untilMs; t += 16) v = c.poll(t);
    return { c, v };
  }

  it("stationary away from centre for 1 s ⇒ velocity 0, still SCRATCH", () => {
    const { c, v } = pullAndHold(0.7, 1000);
    expect(c.phase).toBe("scratch");
    expect(v).toBe(0);
  });

  it("stationary away from centre for 3.9 s ⇒ velocity 0, still SCRATCH", () => {
    const { c, v } = pullAndHold(0.7, 3900);
    expect(c.phase).toBe("scratch");
    expect(v).toBe(0);
    expect(c.holdMs(3900)).toBeGreaterThan(3800);
  });

  it("the same direction at 4.0 s enters SCRUB smoothly, no jump", () => {
    const { c } = pullAndHold(0.7, 3990);
    expect(c.phase).toBe("scratch");
    const atEntry = c.poll(4100);
    expect(c.phase).toBe("scrub");
    // Faded in, not snapped to the full sustained value.
    expect(atEntry).toBeGreaterThan(0);
    expect(atEntry).toBeLessThan(displacementToScrubVelocity(0.7));
    const settled = c.poll(4060 + T.scrubEnterFadeMs + 100);
    expect(settled).toBeCloseTo(displacementToScrubVelocity(0.7), 6);
    expect(Math.abs(settled)).toBeLessThanOrEqual(T.scrubMaxVelocity);
  });

  it("holding below centre for 4 s sustains REVERSE scrub", () => {
    const { c } = pullAndHold(-0.7, 4600);
    expect(c.phase).toBe("scrub");
    const settled = c.poll(4600 + T.scrubEnterFadeMs);
    expect(settled).toBeLessThan(0);
    expect(Math.abs(settled)).toBeLessThanOrEqual(T.scrubMaxVelocity);
  });

  it("crossing centre at 3.9 s resets the qualification timer", () => {
    const c = new ScratchScrubController(grab, 0);
    c.sample(held(0.7), 60);
    for (let t = 76; t <= 3900; t += 16) c.poll(t);
    c.sample(held(-0.7), 3916); // crossed centre
    expect(c.holdMs(3916)).toBe(0);
    for (let t = 3932; t <= 3932 + 3000; t += 16) c.poll(t);
    expect(c.phase).toBe("scratch");
  });

  it("a direction reversal at 3.9 s resets the qualification timer", () => {
    const c = new ScratchScrubController(grab, 0);
    c.sample(held(0.9), 60);
    for (let t = 76; t <= 3900; t += 16) c.poll(t);
    // Same side of centre, but the hand yanked back: qualification restarts.
    c.sample(held(0.5), 3916);
    expect(c.holdMs(3916)).toBe(0);
    for (let t = 3932; t <= 3932 + 3000; t += 16) c.poll(t);
    expect(c.phase).toBe("scratch");
  });

  it("returning to neutral resets qualification", () => {
    const c = new ScratchScrubController(grab, 0);
    c.sample(held(0.6), 60);
    for (let t = 76; t <= 2000; t += 16) c.poll(t);
    c.sample(grab, 2016);
    expect(c.holdMs(2016)).toBe(0);
    expect(c.phase).toBe("scratch");
  });

  it("10+ s of back-and-forth scratching NEVER becomes scrub", () => {
    const c = new ScratchScrubController(grab, 0);
    let t = 0;
    let up = true;
    for (let cycle = 0; cycle < 30; cycle++) {
      const span = up ? 400 : 350;
      const from = up ? -0.5 : 0.5;
      const to = up ? 0.5 : -0.5;
      for (let k = 1; k <= 10; k++) {
        t += span / 10;
        c.sample(held(from + ((to - from) * k) / 10), t);
        expect(c.phase).toBe("scratch");
        expect(Math.abs(c.velocity)).toBeLessThanOrEqual(T.scratchMaxVelocity);
      }
      up = !up;
    }
    expect(t).toBeGreaterThan(10_000);
    expect(c.phase).toBe("scratch");
  });

  it("stopping mid-scratch settles to zero even while displaced", () => {
    const c = new ScratchScrubController(grab, 0);
    c.sample(held(0.8), 40);
    expect(Math.abs(c.velocity)).toBeGreaterThan(0);
    expect(c.poll(40 + 10 * T.scratchDecayMs)).toBe(0);
  });

  it("held-position scrub is nonlinear and bounded by scrubMaxAbsVelocity", () => {
    expect(displacementToScrubVelocity(0)).toBe(0);
    expect(displacementToScrubVelocity(1)).toBeCloseTo(T.scrubMaxVelocity, 12);
    expect(displacementToScrubVelocity(-1)).toBeCloseTo(-T.scrubMaxVelocity, 12);
    const quarter = displacementToScrubVelocity(0.25);
    expect(quarter).toBeLessThan(0.25 * T.scrubMaxVelocity);
    expect(displacementToScrubVelocity(0.5) / quarter).toBeGreaterThan(2);
  });

  it("a fresh grab starts at zero in SCRATCH", () => {
    const c = new ScratchScrubController(grab, 0);
    expect(c.phase).toBe("scratch");
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
