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

/**
 * Finger travel from the grab point to full velocity, in SVG user units.
 * The grab point itself is always neutral: grabbing either half must not make
 * the tape jump before the musician moves their finger.
 */
export const ROCKER_DRAG_RANGE = 70;

/** Visible rocker travel at full deflection, in SVG user units. */
export const ROCKER_VISUAL_TRAVEL = 24;

/**
 * Pointer Y delta from the initial grab → normalized displacement in [-1,+1].
 * Moving above the grab point is positive; moving below it is negative.
 */
export function rockerDisplacement(
  userY: number,
  grabY: number,
  range = ROCKER_DRAG_RANGE,
): number {
  if (!Number.isFinite(userY) || !Number.isFinite(grabY) || range <= 0) return 0;
  const d = (grabY - userY) / range;
  return d > 1 ? 1 : d < -1 ? -1 : d;
}

/** Normalized displacement → signed master velocity (master frames / frame). */
export function displacementToVelocity(d: number, max = SCRATCH_TUNING.maxAbsVelocity): number {
  if (!Number.isFinite(d)) return 0;
  const clamped = d > 1 ? 1 : d < -1 ? -1 : d;
  return clampVelocity(clamped * max, max);
}

/** Visible Y offset. Positive tape velocity moves the rocker upward. */
export function rockerVisualY(d: number, travel = ROCKER_VISUAL_TRAVEL): number {
  const clamped = d > 1 ? 1 : d < -1 ? -1 : d;
  return -clamped * travel;
}

/** The CSS transform written directly to the rocker group during a drag. */
export function rockerTransform(d: number): string {
  return `translateY(${rockerVisualY(d).toFixed(3)}px)`;
}
