/**
 * S3 — the rocker as a continuously draggable physical tape control.
 *
 * FUNCTION = hand on the tape. FUNCTION + grab/drag the rocker = push/pull the
 * ONE authoritative signed master head implemented in S2. This module is pure:
 * it converts a pointer position (SVG user units) into a normalized rocker
 * displacement and then into a signed master velocity. Nothing here knows about
 * React, the DOM or the audio engine, so the mapping is unit-testable and the
 * tuning stays centralized in `src/audio/masterScratch.ts`.
 *
 * Velocity constants are NOT defined here: `SCRATCH_TUNING.maxAbsVelocity` is
 * the single tuning surface (S2 requirement).
 */
import { SCRATCH_TUNING, clampVelocity } from "@/audio/masterScratch";

/** Resting visual centre of the rocker body, in viewBox user units. */
export const ROCKER_CENTER_Y = 225;

/**
 * Finite physical travel each way. 70 user units ≈ the rocker body plus a
 * little overshoot, so a full-scale scratch is a short thumb movement rather
 * than a page-length drag.
 */
export const ROCKER_DRAG_RANGE = 70;

/** Full-deflection tilt. Slightly beyond the ±3.2° static press tilt. */
export const ROCKER_MAX_TILT_DEG = 6.4;

/**
 * Pointer Y (user units) → normalized displacement in [-1, +1].
 * ABOVE centre is positive (tape pushed forward), below is negative.
 */
export function rockerDisplacement(
  userY: number,
  center = ROCKER_CENTER_Y,
  range = ROCKER_DRAG_RANGE,
): number {
  if (!Number.isFinite(userY) || range <= 0) return 0;
  const d = (center - userY) / range;
  return d > 1 ? 1 : d < -1 ? -1 : d;
}

/** Normalized displacement → signed master velocity (master frames / frame). */
export function displacementToVelocity(d: number, max = SCRATCH_TUNING.maxAbsVelocity): number {
  if (!Number.isFinite(d)) return 0;
  const clamped = d > 1 ? 1 : d < -1 ? -1 : d;
  return clampVelocity(clamped * max, max);
}

/** Visual tilt for a displacement. Positive displacement tilts forward (up). */
export function rockerTiltDeg(d: number, maxDeg = ROCKER_MAX_TILT_DEG): number {
  const clamped = d > 1 ? 1 : d < -1 ? -1 : d;
  return -clamped * maxDeg;
}

/** The CSS transform written straight to the rocker group during a drag. */
export function rockerTransform(d: number): string {
  return `rotate(${rockerTiltDeg(d).toFixed(3)}deg)`;
}
