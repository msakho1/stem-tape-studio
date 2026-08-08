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
