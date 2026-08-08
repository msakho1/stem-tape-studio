import { describe, expect, it } from "vitest";
import { bpmStepToSpeed, effectiveBpm, glideCurve, GLIDE_TAU, integratedDistance, rateAt, timeForDistance } from "../glide";
import { complementary, equalPower, peakCorrelatedSum } from "../crossfade";
import { resolveLoop, TapeTimeline, DEFAULT_WINDOW } from "../tape";
import { estimateMigration, handoffAccepted } from "../workletBudget";
import { antiClickFor } from "../antiClick";
import { MiB, type MemoryBudget } from "../memory";

/** Binding correction #7: never assert floating-point equality. */
const TOL = 1e-9;

describe("glide math", () => {
  it("integrates the spec curve, not a linear ramp", () => {
    const seg = { startAt: 0, from: 1, to: 1.5, tau: 0.06 };
    const dt = 0.02;
    const exact = 1.5 * dt + (1 - 1.5) * 0.06 * (1 - Math.exp(-dt / 0.06));
    expect(integratedDistance(seg, dt)).toBeCloseTo(exact, 12);
    // A linear ramp would over-advance; the exponential lags behind it.
    const linear = ((1 + 1.5) / 2) * dt;
    expect(integratedDistance(seg, dt)).toBeLessThan(linear);
  });

  it("never mathematically reaches the target", () => {
    const seg = { startAt: 0, from: 1, to: 2, tau: 0.06 };
    expect(rateAt(seg, 10)).toBeGreaterThan(2 - 1e-6 ? 1.999999 : 0);
    expect(rateAt(seg, 1e6)).not.toBe(2);
    expect(Math.abs(rateAt(seg, 1e6) - 2)).toBeLessThan(1e-12);
  });

  it("finite scheduled curve lands exactly on the target", () => {
    const c = glideCurve(1, 2, GLIDE_TAU, 64);
    expect(c[63]).toBe(2);
  });

  it("inverts distance → time on the same curve", () => {
    const seg = { startAt: 0, from: 0.8, to: 1.3, tau: 0.05 };
    const t = timeForDistance(seg, 0.4)!;
    expect(integratedDistance(seg, t)).toBeCloseTo(0.4, 9);
  });
});

describe("rocker steps are linear in effective BPM", () => {
  it("120 → 122 within tolerance, not strict equality", () => {
    const base = 120;
    const speed = bpmStepToSpeed(1, 2, base);
    const bpm = effectiveBpm(speed, base);
    expect(Math.abs(bpm - 122)).toBeLessThan(1e-9);
    expect(bpm).toBeCloseTo(122, 9);
  });

  it("does not compound", () => {
    let s = 1;
    for (let i = 0; i < 10; i++) s = bpmStepToSpeed(s, 1, 120);
    expect(Math.abs(effectiveBpm(s, 120) - 130)).toBeLessThan(1e-9);
  });
});

describe("crossfade curves", () => {
  it("equal-power bumps a correlated pair, complementary does not", () => {
    expect(peakCorrelatedSum(equalPower)).toBeGreaterThan(1.4);
    expect(peakCorrelatedSum(complementary)).toBeLessThanOrEqual(1 + TOL);
  });

  it("equal-power holds power constant for uncorrelated seams", () => {
    for (let i = 0; i <= 20; i++) {
      const { a, b } = equalPower(i / 20);
      expect(Math.abs(a * a + b * b - 1)).toBeLessThan(1e-12);
    }
  });
});

describe("tape timeline", () => {
  it("derives position across a glide using the integral", () => {
    const tl = new TapeTimeline(1);
    tl.anchor(0, 0);
    tl.glideTo(0, 2, 0.06);
    const pos = tl.positionAt(0.03);
    const expected = 2 * 0.03 + (1 - 2) * 0.06 * (1 - Math.exp(-0.03 / 0.06));
    expect(Math.abs(pos - expected)).toBeLessThan(1e-9);
  });

  it("recomputes a future seam on the same integrated timeline", () => {
    const tl = new TapeTimeline(1);
    tl.anchor(0, 0);
    const before = tl.timeAtPosition(0, 1)!;
    expect(Math.abs(before - 1)).toBeLessThan(1e-9);
    tl.glideTo(0, 2, 0.06);
    const after = tl.timeAtPosition(0, 1)!;
    expect(after).toBeLessThan(before); // faster tape reaches the seam sooner
    expect(Math.abs(tl.positionAt(after) - 1)).toBeLessThan(1e-6);
  });

  it("chops subdivide the active window", () => {
    const b = resolveLoop({ ...DEFAULT_WINDOW, start: 0.25, end: 0.75, chopDiv: 4, chopIndex: 2 }, 8);
    expect(Math.abs(b.length - 1)).toBeLessThan(1e-9); // 0.5 * 8 / 4
    expect(Math.abs(b.start - 3)).toBeLessThan(1e-9); // (0.25 + 2*0.125) * 8
  });
});

describe("worklet migration gate", () => {
  const budget: MemoryBudget = {
    warnBytes: 192 * MiB,
    standardBlockBytes: 384 * MiB,
    highMemoryBlockBytes: 512 * MiB,
    platform: "ios",
  };

  it("gates on the whole project duplicated, not twice one track", () => {
    const tracks = [50 * MiB, 50 * MiB, 50 * MiB, 49 * MiB];
    const est = estimateMigration(tracks, budget, true);
    expect(est.projectBytes).toBe(199 * MiB);
    expect(est.worstCasePeakBytes).toBe(398 * MiB);
    expect(est.allowed).toBe(true); // 398 < 512 ceiling, High Memory on
    const strict = estimateMigration(tracks, budget, false);
    expect(strict.allowed).toBe(false); // 398 > 384 standard limit
  });

  it("refuses handoff on phase drift even after a successful render", () => {
    expect(handoffAccepted(48_000, 48_000, true).ok).toBe(true);
    expect(handoffAccepted(48_000, 48_009, true).ok).toBe(false);
    expect(handoffAccepted(48_000, 48_000, false).ok).toBe(false);
  });
});

describe("anti-click matrix", () => {
  it("loop-mode transitions use a dual-source crossfade", () => {
    expect(antiClickFor("loop.enable")?.antiClick).toBe("dual-source-crossfade");
    expect(antiClickFor("loop.length")?.antiClick).toBe("dual-source-crossfade");
    expect(antiClickFor("chop.div")?.antiClick).toBe("dual-source-crossfade");
  });

  it("dry ↔ filter uses complementary gains", () => {
    expect(antiClickFor("filter.mode")?.antiClick).toBe("complementary-dry-wet");
  });
});
