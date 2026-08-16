/**
 * Workstream 2 — tape transport inertia.
 *
 * The wind-up / wind-down is produced by MOVING THE TAPE SPEED, so pitch
 * follows naturally. No canned tape sample is layered over the music.
 *
 * The curve is a FINITE exponential that terminates exactly on the target rate
 * (an asymptotic setTargetAtTime would never arrive, and the derived playhead
 * would drift from the audio). Shape:
 *
 *     r(u) = r1 + (r0 - r1) · f(u),      u = t / D  in [0, 1]
 *     f(u) = (e^(-k·u) - e^(-k)) / (1 - e^(-k)),    f(0) = 1, f(1) = 0
 *
 * Media-seconds advanced over the first x of the ramp (closed form, the only
 * function allowed to convert inertia time into playhead distance):
 *
 *     d(x) = D · [ r1·x + (r0 - r1)·F(x) ]
 *     F(x) = ( (1 - e^(-k·x))/k - x·e^(-k) ) / (1 - e^(-k))
 */

export type InertiaPresetName = "tight" | "classic" | "slow";

export interface InertiaPreset {
  name: InertiaPresetName;
  label: string;
  /** Wind-up duration, seconds. */
  startS: number;
  /** Wind-down duration, seconds. */
  stopS: number;
}

export const INERTIA_PRESETS: Record<InertiaPresetName, InertiaPreset> = {
  tight: { name: "tight", label: "Tight", startS: 0.18, stopS: 0.3 },
  classic: { name: "classic", label: "Classic", startS: 0.3, stopS: 0.45 },
  slow: { name: "slow", label: "Slow", startS: 0.6, stopS: 0.9 },
};

/** Binding correction: Classic is the default (300 ms / 450 ms). */
export const DEFAULT_INERTIA_PRESET: InertiaPresetName = "classic";

/** Curvature. 4 gives an audibly tape-like knee without a long tail. */
export const INERTIA_K = 4;

/** Click-free final fade applied after a wind-down reaches ~0 rate. */
export const INERTIA_STOP_FADE_S = 0.008;

/** Rate treated as "stopped" — below this the tape is inaudible anyway. */
export const INERTIA_MIN_RATE = 1e-4;

export interface InertiaSegment {
  /** AudioContext time the ramp starts. */
  startAt: number;
  /** Rate at startAt (may be mid-ramp from a reversed transition). */
  from: number;
  /** Rate at startAt + durationS — reached EXACTLY. */
  to: number;
  durationS: number;
  k: number;
  kind: "windUp" | "windDown";
}

function shape(u: number, k: number): number {
  if (u <= 0) return 1;
  if (u >= 1) return 0;
  return (Math.exp(-k * u) - Math.exp(-k)) / (1 - Math.exp(-k));
}

function shapeIntegral(x: number, k: number): number {
  if (x <= 0) return 0;
  const xc = Math.min(1, x);
  return ((1 - Math.exp(-k * xc)) / k - xc * Math.exp(-k)) / (1 - Math.exp(-k));
}

/** Instantaneous rate dt seconds into the ramp. */
export function inertiaRateAt(seg: InertiaSegment, dt: number): number {
  if (dt <= 0) return seg.from;
  if (dt >= seg.durationS) return seg.to;
  return seg.to + (seg.from - seg.to) * shape(dt / seg.durationS, seg.k);
}

/** Media-seconds advanced over the first dt context-seconds of the ramp. */
export function inertiaDistance(seg: InertiaSegment, dt: number): number {
  if (dt <= 0) return 0;
  const D = seg.durationS;
  if (dt >= D) {
    const full = D * (seg.to * 1 + (seg.from - seg.to) * shapeIntegral(1, seg.k));
    return full + (dt - D) * seg.to;
  }
  const x = dt / D;
  return D * (seg.to * x + (seg.from - seg.to) * shapeIntegral(x, seg.k));
}

/** Sampled curve for `AudioParam.setValueCurveAtTime` on the node fallback. */
export function inertiaCurve(seg: InertiaSegment, points = 128): Float32Array {
  const out = new Float32Array(points);
  for (let i = 0; i < points; i++) {
    const u = i / (points - 1);
    out[i] = Math.max(INERTIA_MIN_RATE, inertiaRateAt(seg, u * seg.durationS));
  }
  out[points - 1] = Math.max(INERTIA_MIN_RATE, seg.to);
  return out;
}

/**
 * Build a transition. `currentRate` is the INSTANTANEOUS rate, so Play during a
 * wind-down (or Stop during a wind-up) reverses smoothly from where the tape
 * actually is instead of snapping. Duration is scaled by how far the rate still
 * has to travel, so a reversal near the target is short, not a full 450 ms.
 */
export function makeInertiaSegment(args: {
  startAt: number;
  currentRate: number;
  targetRate: number;
  preset: InertiaPreset;
  kind: "windUp" | "windDown";
}): InertiaSegment {
  const { startAt, currentRate, targetRate, preset, kind } = args;
  const nominal = kind === "windUp" ? preset.startS : preset.stopS;
  const span = Math.abs(targetRate - currentRate);
  const reference = Math.max(Math.abs(targetRate), Math.abs(currentRate), 1e-6);
  const scaled = nominal * Math.min(1, span / reference);
  return {
    startAt,
    from: currentRate,
    to: targetRate,
    durationS: Math.max(0.01, scaled),
    k: INERTIA_K,
    kind,
  };
}

/**
 * "cued" is a distinct STOPPED state: the tape is parked on song frame zero and
 * the next single Play tap launches every stem on one scheduled frame.
 */
export type TransportPhase = "stopped" | "cued" | "windingUp" | "playing" | "windingDown";

/** Exact cue launch: no wind-up, only a click-free 8 ms open. */
export const CUE_FADE_S = 0.008;

/**
 * Stock-SP-1 addendum §6 — the shuttle release is a real tape hand-off, not a
 * jump cut. The transport decelerates from the SIGNED shuttle rate to the
 * musical rate along the same finite exponential the wind-up/wind-down uses:
 *
 *   forward shuttle  +R → +musical            (monotone, never leaves forward)
 *   reverse shuttle  −R → 0 → +musical        (one continuous curve through 0)
 *
 * Duration scales with how far the rate has to travel, so a reverse release is
 * audibly longer than a forward one — exactly as reel inertia behaves.
 */
export function makeScrubReleaseSegment(args: {
  startAt: number;
  /** Signed shuttle rate at release (negative when shuttling backwards). */
  fromRate: number;
  /** Musical rate to land on (always positive). */
  toRate: number;
  preset: InertiaPreset;
}): InertiaSegment {
  const { startAt, fromRate, toRate, preset } = args;
  const span = Math.abs(toRate - fromRate);
  const reference = Math.max(Math.abs(fromRate), Math.abs(toRate), 1e-6);
  const scaled = preset.stopS * Math.min(2, span / reference);
  return {
    startAt,
    from: fromRate,
    to: toRate,
    durationS: Math.max(0.02, Math.min(1.2, scaled)),
    k: INERTIA_K,
    kind: "windUp",
  };
}

/**
 * Context time at which a sign-crossing segment passes through zero rate, or
 * null when it never changes direction. Solving the shape exactly is what lets
 * the reverse hand-off swap from reversed grains to forward playback at the one
 * frame where the tape is momentarily still — so the swap cannot click.
 */
export function inertiaZeroCrossing(seg: InertiaSegment): number | null {
  if (seg.from === 0 || seg.to === 0) return seg.from === 0 ? seg.startAt : null;
  if (seg.from > 0 === seg.to > 0) return null;
  const s = -seg.to / (seg.from - seg.to); // required shape value in (0,1)
  if (!(s > 0 && s < 1)) return null;
  const k = seg.k;
  const e = Math.exp(-k);
  const u = -Math.log(s * (1 - e) + e) / k;
  if (!(u > 0 && u < 1)) return null;
  return seg.startAt + u * seg.durationS;
}

/**
 * Four persistent shuttle speeds (stock-SP-1 addendum §3). 1.6× is the value
 * the shuttle already ran at and stays the default; the others bracket it.
 */
export const GLOBAL_SCRUB_SPEEDS = [1.25, 1.6, 2.5, 4] as const;
export const DEFAULT_SCRUB_SPEED_INDEX = 1;
export type ScrubSpeedIndex = 0 | 1 | 2 | 3;
