import { describe, expect, it } from "vitest";
import { SCRATCH_TUNING } from "@/audio/masterScratch";
import {
  ROCKER_DRAG_RANGE,
  ROCKER_VISUAL_TRAVEL,
  displacementToVelocity,
  rockerDisplacement,
  rockerTransform,
  rockerVisualY,
} from "@/input/rockerScratch";

describe("S3 rocker displacement", () => {
  const grabY = 225;

  it("the exact grab point is the tape held still", () => {
    expect(rockerDisplacement(grabY, grabY)).toBe(0);
    expect(displacementToVelocity(rockerDisplacement(grabY, grabY))).toBe(0);
  });

  it("movement above the grab pushes forward; movement below pulls backward", () => {
    expect(rockerDisplacement(grabY - ROCKER_DRAG_RANGE / 2, grabY)).toBeCloseTo(0.5, 12);
    expect(rockerDisplacement(grabY + ROCKER_DRAG_RANGE / 2, grabY)).toBeCloseTo(-0.5, 12);
  });

  it("grabbing either half starts neutral", () => {
    expect(rockerDisplacement(205, 205)).toBe(0);
    expect(rockerDisplacement(245, 245)).toBe(0);
  });

  it("clamps beyond the finite physical travel", () => {
    expect(rockerDisplacement(grabY - 10 * ROCKER_DRAG_RANGE, grabY)).toBe(1);
    expect(rockerDisplacement(grabY + 10 * ROCKER_DRAG_RANGE, grabY)).toBe(-1);
    expect(rockerDisplacement(Number.NaN, grabY)).toBe(0);
  });

  it("displacement is monotone through zero", () => {
    let prev = -Infinity;
    for (let y = grabY + ROCKER_DRAG_RANGE; y >= grabY - ROCKER_DRAG_RANGE; y -= 1) {
      const d = rockerDisplacement(y, grabY);
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
    const above = displacementToVelocity(rockerDisplacement(224, 225));
    const below = displacementToVelocity(rockerDisplacement(226, 225));
    expect(above).toBeGreaterThan(0);
    expect(below).toBeLessThan(0);
    expect(Math.abs(above - below)).toBeLessThan(0.2);
  });
});

describe("S3 rocker visual", () => {
  it("travels with the finger and rests at the grabbed position", () => {
    expect(rockerVisualY(0)).toBe(-0);
    expect(rockerVisualY(1)).toBe(-ROCKER_VISUAL_TRAVEL);
    expect(rockerVisualY(-1)).toBe(ROCKER_VISUAL_TRAVEL);
    expect(rockerTransform(0.5)).toBe("translateY(-12.000px)");
  });
});
