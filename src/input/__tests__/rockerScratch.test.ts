import { describe, expect, it } from "vitest";
import { SCRATCH_TUNING } from "@/audio/masterScratch";
import {
  ROCKER_CENTER_Y,
  ROCKER_DRAG_RANGE,
  displacementToVelocity,
  rockerDisplacement,
  rockerTiltDeg,
  rockerTransform,
} from "@/input/rockerScratch";

describe("S3 rocker displacement", () => {
  it("centre is the tape held still", () => {
    expect(rockerDisplacement(ROCKER_CENTER_Y)).toBe(0);
    expect(displacementToVelocity(rockerDisplacement(ROCKER_CENTER_Y))).toBe(0);
  });

  it("above centre pushes forward, below centre pulls backward", () => {
    expect(rockerDisplacement(ROCKER_CENTER_Y - ROCKER_DRAG_RANGE / 2)).toBeCloseTo(0.5, 12);
    expect(rockerDisplacement(ROCKER_CENTER_Y + ROCKER_DRAG_RANGE / 2)).toBeCloseTo(-0.5, 12);
  });

  it("clamps beyond the finite physical travel", () => {
    expect(rockerDisplacement(ROCKER_CENTER_Y - 10 * ROCKER_DRAG_RANGE)).toBe(1);
    expect(rockerDisplacement(ROCKER_CENTER_Y + 10 * ROCKER_DRAG_RANGE)).toBe(-1);
    expect(rockerDisplacement(Number.NaN)).toBe(0);
  });

  it("displacement is monotone through zero", () => {
    let prev = -Infinity;
    for (let y = ROCKER_CENTER_Y + ROCKER_DRAG_RANGE; y >= ROCKER_CENTER_Y - ROCKER_DRAG_RANGE; y -= 1) {
      const d = rockerDisplacement(y);
      expect(d).toBeGreaterThan(prev);
      prev = d;
    }
  });
});

describe("S3 displacement → signed master velocity", () => {
  it("full deflection is exactly the centralized Vmax, both signs", () => {
    expect(displacementToVelocity(1)).toBe(SCRATCH_TUNING.maxAbsVelocity);
    expect(displacementToVelocity(-1)).toBe(-SCRATCH_TUNING.maxAbsVelocity);
  });

  it("is linear in displacement", () => {
    for (const d of [-0.9, -0.4, 0, 0.25, 0.75]) {
      expect(displacementToVelocity(d)).toBeCloseTo(d * SCRATCH_TUNING.maxAbsVelocity, 12);
    }
  });

  it("never exceeds the clamp even for out-of-range input", () => {
    expect(displacementToVelocity(9)).toBe(SCRATCH_TUNING.maxAbsVelocity);
    expect(displacementToVelocity(-9)).toBe(-SCRATCH_TUNING.maxAbsVelocity);
    expect(displacementToVelocity(Number.NaN)).toBe(0);
  });

  it("crossing centre reverses sign continuously (no jump at zero)", () => {
    const above = displacementToVelocity(rockerDisplacement(ROCKER_CENTER_Y - 1));
    const below = displacementToVelocity(rockerDisplacement(ROCKER_CENTER_Y + 1));
    expect(above).toBeGreaterThan(0);
    expect(below).toBeLessThan(0);
    expect(Math.abs(above - below)).toBeLessThan(0.2);
  });
});

describe("S3 rocker visual", () => {
  it("tilts forward above centre and back below, resting flat at centre", () => {
    expect(rockerTiltDeg(0)).toBe(-0);
    expect(rockerTiltDeg(1)).toBeLessThan(0);
    expect(rockerTiltDeg(-1)).toBeGreaterThan(0);
    expect(rockerTransform(0.5)).toMatch(/^rotate\(-3\.200deg\)$/);
  });
});
