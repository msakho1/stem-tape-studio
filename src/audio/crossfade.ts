/**
 * Crossfade curves (Phase 5, binding correction #2).
 *
 * Which curve is correct depends entirely on the CORRELATION of the two signals
 * being blended:
 *
 *  - sourceA ↔ sourceB at a loop seam: two different points of the tape, so the
 *    signals are effectively UNCORRELATED. Summed power adds, so equal-power
 *    (sin/cos, gains² sum to 1) holds perceived loudness constant.
 *
 *  - dry ↔ filtered: the SAME signal through a biquad, so the two paths are
 *    strongly CORRELATED. Amplitudes add, not powers. Equal-power here produces
 *    an audible +3 dB bump at the midpoint. Complementary gains (summing to 1)
 *    are the correct default, with a measured correction available for filter
 *    settings whose phase response decorrelates the paths.
 */

export interface FadePair {
  a: number;
  b: number;
}

/** Uncorrelated sources: gains² sum to 1. Use for sourceA ↔ sourceB seams. */
export function equalPower(t: number): FadePair {
  const x = Math.min(1, Math.max(0, t));
  return { a: Math.cos((x * Math.PI) / 2), b: Math.sin((x * Math.PI) / 2) };
}

/** Correlated paths: gains sum to 1. Use for dry ↔ filtered. */
export function complementary(t: number): FadePair {
  const x = Math.min(1, Math.max(0, t));
  return { a: 1 - x, b: x };
}

/**
 * Measured curve for dry ↔ filter.
 *
 * `correlation` is the measured coherence between the two paths at the current
 * filter setting: 1 = perfectly correlated (complementary is exact), 0 = fully
 * decorrelated (equal-power is exact). Anything between interpolates, so a
 * measured bump can be dialled out without changing call sites.
 */
export function measuredDryWet(t: number, correlation = 1): FadePair {
  const c = Math.min(1, Math.max(0, correlation));
  const lin = complementary(t);
  const pow = equalPower(t);
  return { a: lin.a * c + pow.a * (1 - c), b: lin.b * c + pow.b * (1 - c) };
}

/**
 * Peak summed amplitude of a curve across the fade, for a correlated pair
 * (amplitudes add). Used by the level-bump test: complementary must never
 * exceed 1, equal-power provably does (√2 at the midpoint).
 */
export function peakCorrelatedSum(curve: (t: number) => FadePair, steps = 201): number {
  let peak = 0;
  for (let i = 0; i < steps; i++) {
    const { a, b } = curve(i / (steps - 1));
    peak = Math.max(peak, a + b);
  }
  return peak;
}

/** Sample a fade into a Float32Array for setValueCurveAtTime. */
export function sampleCurve(curve: (t: number) => FadePair, pick: "a" | "b", points = 64): Float32Array {
  const out = new Float32Array(points);
  for (let i = 0; i < points; i++) out[i] = curve(i / (points - 1))[pick];
  return out;
}

/** Default seam crossfade length, seconds (5–20 ms window). */
export const SEAM_FADE_S = 0.012;
/** Dry ↔ filter transition length, seconds. */
export const FILTER_FADE_S = 0.03;
