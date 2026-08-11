/**
 * Heads mode = FOUR readers over ONE stem, at FOUR distinct song positions.
 */
import { describe, expect, it } from "vitest";
import { distributeHeads } from "../headLanes";

const distinct = (xs: number[]) => new Set(xs.map((x) => x.toFixed(3))).size === xs.length;

describe("heads initial distribution", () => {
  it("spreads four distinct positions around the current transport position", () => {
    const p = distributeHeads(240, 90);
    expect(p).toHaveLength(4);
    expect(distinct(p)).toBe(true);
    expect(p[2]).toBeCloseTo(90, 6);
    const spacing = Math.min(Math.max(240 * 0.1, 8), 30);
    expect(p[1]).toBeCloseTo(90 - spacing, 6);
    expect(p[3]).toBeCloseTo(90 + spacing, 6);
  });

  it("never collapses heads at the very start of the song", () => {
    const p = distributeHeads(240, 0);
    expect(distinct(p)).toBe(true);
    expect(Math.min(...p)).toBeGreaterThanOrEqual(0);
  });

  it("never collapses heads at the very end of the song", () => {
    const dur = 240;
    const p = distributeHeads(dur, dur);
    expect(distinct(p)).toBe(true);
    expect(Math.max(...p)).toBeLessThanOrEqual(dur);
  });

  it("falls back to 20/40/60/80 % on very short sources", () => {
    const p = distributeHeads(4, 1);
    expect(p.map((x) => +x.toFixed(3))).toEqual([0.8, 1.6, 2.4, 3.2]);
    expect(distinct(p)).toBe(true);
  });

  it("stays in bounds and distinct across a sweep of positions", () => {
    for (let dur = 10; dur <= 600; dur += 37) {
      for (let cur = 0; cur <= dur; cur += Math.max(1, dur / 7)) {
        const p = distributeHeads(dur, cur);
        expect(distinct(p)).toBe(true);
        expect(Math.min(...p)).toBeGreaterThanOrEqual(0);
        expect(Math.max(...p)).toBeLessThanOrEqual(dur + 1e-9);
      }
    }
  });
});
