/**
 * Rate-glide math (Phase 5, binding correction #1).
 *
 * `AudioParam.setTargetAtTime()` is an EXPONENTIAL approach:
 *
 *     r(t) = r1 + (r0 - r1) · e^(-t/τ)
 *
 * It never mathematically reaches r1, so treating a glide as a linear ramp
 * desynchronises the derived playhead from the audio. The tape timeline
 * therefore integrates the exact curve given by the Web Audio specification:
 *
 *     d(Δt) = r1·Δt + (r0 - r1)·τ·(1 - e^(-Δt/τ))
 *
 * where d is media-seconds advanced over Δt context-seconds.
 *
 * The alternative, used when a glide MUST terminate exactly (seam scheduling
 * across a loop boundary), is a finite scheduled curve — `setValueCurveAtTime`
 * with a sampled exponential that ends precisely on r1 — see `glideCurve()`.
 */

/** Default time constant for a musical rate glide, in seconds. */
export const GLIDE_TAU = 0.06;
/** After 6τ the residual is < 0.25 %; we treat the glide as finished there. */
export const GLIDE_SETTLE_MULT = 6;

export interface GlideSegment {
  /** AudioContext time the glide started. */
  startAt: number;
  /** Rate at startAt. */
  from: number;
  /** Asymptotic target rate. */
  to: number;
  /** Time constant τ, seconds. */
  tau: number;
}

/** Instantaneous rate of an exponential approach at Δt seconds in. */
export function rateAt(seg: GlideSegment, dt: number): number {
  if (dt <= 0) return seg.from;
  return seg.to + (seg.from - seg.to) * Math.exp(-dt / seg.tau);
}

/**
 * Media-seconds advanced over Δt context-seconds — the exact integral of the
 * spec curve. This is the ONLY function allowed to convert glide time into
 * playhead distance.
 */
export function integratedDistance(seg: GlideSegment, dt: number): number {
  if (dt <= 0) return 0;
  const { from: r0, to: r1, tau } = seg;
  return r1 * dt + (r0 - r1) * tau * (1 - Math.exp(-dt / tau));
}

/**
 * Inverse of `integratedDistance`: how many context-seconds until `distance`
 * media-seconds have elapsed. Used to place a future loop seam on the same
 * integrated-rate timeline as the playhead (binding correction #5).
 *
 * d(Δt) is strictly increasing for positive rates, so a bracketed bisection is
 * both safe and exact to the requested tolerance. Returns null when the target
 * is unreachable (rate glides to ~0 and the distance is never covered).
 */
export function timeForDistance(seg: GlideSegment, distance: number, tolerance = 1e-9): number | null {
  if (distance <= 0) return 0;
  const minRate = Math.min(seg.from, seg.to);
  if (minRate <= 0) return null;
  // Bracket: the glide never runs slower than minRate, so this is an upper bound.
  let hi = distance / minRate + seg.tau;
  if (!Number.isFinite(hi)) return null;
  let lo = 0;
  for (let i = 0; i < 200 && hi - lo > tolerance; i++) {
    const mid = (lo + hi) / 2;
    if (integratedDistance(seg, mid) < distance) lo = mid;
    else hi = mid;
  }
  return (lo + hi) / 2;
}

/** True once the exponential has settled far enough to be called constant. */
export function settled(seg: GlideSegment, dt: number): boolean {
  return dt >= seg.tau * GLIDE_SETTLE_MULT;
}

/**
 * Finite scheduled curve that ENDS exactly on `to`. Sampled from the same
 * exponential so it sounds identical to setTargetAtTime, but terminates, which
 * keeps `rate.set` ack values and the integrated timeline exactly in agreement.
 */
export function glideCurve(from: number, to: number, tau = GLIDE_TAU, points = 64): Float32Array {
  const duration = tau * GLIDE_SETTLE_MULT;
  const curve = new Float32Array(points);
  for (let i = 0; i < points; i++) {
    const t = (i / (points - 1)) * duration;
    curve[i] = to + (from - to) * Math.exp(-t / tau);
  }
  curve[points - 1] = to; // finite curves must land on the target exactly
  return curve;
}

export function glideDurationS(tau = GLIDE_TAU): number {
  return tau * GLIDE_SETTLE_MULT;
}

/**
 * Rocker steps are LINEAR IN EFFECTIVE BPM, not compounding:
 *   speed' = speed + steps / baseBpm
 * so 120 → 122 BPM is exactly two steps at baseBpm = 120.
 */
export function bpmStepToSpeed(speed: number, steps: number, baseBpm: number): number {
  return speed + steps / baseBpm;
}

export function effectiveBpm(speed: number, baseBpm: number): number {
  return speed * baseBpm;
}
