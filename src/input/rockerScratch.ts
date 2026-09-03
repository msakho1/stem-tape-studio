/**
 * S3 — the rocker as a physical tape control ("hand on the record").
 *
 * FUNCTION = put my hand on the tape. FUNCTION + grab/drag the rocker = push or
 * pull the ONE authoritative signed master head implemented in S2.
 *
 * TWO SEPARATE THINGS, DELIBERATELY DECOUPLED
 * -------------------------------------------
 *  - AUDIO velocity comes from HAND SPEED (Δy/Δt). A finger that is held still
 *    commands zero, wherever it is resting. That is a record under a hand, not
 *    a shuttle joystick whose off-centre position latches a speed.
 *  - VISUAL rocker travel comes from grab-relative DISPLACEMENT, purely so the
 *    control follows the finger on screen. It never feeds the audio path.
 *
 * This module is pure: no React, no DOM, no audio engine, so both mappings are
 * unit-testable. Velocity constants live in `src/audio/masterScratch.ts`.
 */
import { SCRATCH_TUNING, clampVelocity, handVelocityToTapeVelocity } from "@/audio/masterScratch";

/**
 * Finger travel from the grab point to full VISUAL deflection, in SVG user
 * units. Visual only — see the module note.
 */
export const ROCKER_DRAG_RANGE = 70;

/** Visible rocker travel at full deflection, in SVG user units. */
export const ROCKER_VISUAL_TRAVEL = 24;

/**
 * Grab-relative displacement in [-1,+1], used for the VISUAL rocker position.
 * The grab point itself is neutral, so grabbing either half never moves the
 * control before the musician does.
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

/**
 * Legacy shuttle mapping (displacement → sustained velocity). Retained only for
 * the no-stems fallback shuttle; the scratch path does NOT use it.
 */
export function displacementToVelocity(d: number, max = SCRATCH_TUNING.maxAbsVelocity): number {
  if (!Number.isFinite(d)) return 0;
  const clamped = d > 1 ? 1 : d < -1 ? -1 : d;
  return clampVelocity(clamped * max, max);
}

/** Visible Y offset. Positive displacement moves the rocker upward. */
export function rockerVisualY(d: number, travel = ROCKER_VISUAL_TRAVEL): number {
  const clamped = d > 1 ? 1 : d < -1 ? -1 : d;
  return -clamped * travel;
}

/** The CSS transform written directly to the rocker group during a drag. */
export function rockerTransform(d: number): string {
  return `translateY(${rockerVisualY(d).toFixed(3)}px)`;
}

/**
 * Hand-velocity tracker.
 *
 * One sample per pointer event. `sample()` returns the signed master velocity
 * the hand is currently commanding; `stopIfIdle()` is what the caller polls so
 * a held-but-motionless finger settles the record at zero, because the browser
 * simply stops delivering pointermove when the finger stops.
 */
export class HandVelocityTracker {
  private lastY: number;
  private lastT: number;
  /** Last non-zero commanded velocity, kept only for diagnostics. */
  velocity = 0;

  constructor(grabY: number, t: number) {
    this.lastY = grabY;
    this.lastT = t;
  }

  /** New pointer position → signed master velocity. */
  sample(userY: number, t: number): number {
    if (!Number.isFinite(userY) || !Number.isFinite(t)) return this.velocity;
    const dy = userY - this.lastY;
    const dt = t - this.lastT;
    if (dt <= 0) return this.velocity;
    this.lastY = userY;
    this.lastT = t;
    this.velocity = handVelocityToTapeVelocity(dy, dt);
    return this.velocity;
  }

  /**
   * True when the hand has produced no movement for `timeoutMs`: the caller
   * must then command exactly 0. Returns false once it has already reported a
   * stop, so the stop is commanded once per pause, not every poll.
   */
  stopIfIdle(t: number, timeoutMs = SCRATCH_TUNING.handStopTimeoutMs): boolean {
    if (this.velocity === 0) return false;
    if (t - this.lastT < timeoutMs) return false;
    this.velocity = 0;
    return true;
  }

  /** Milliseconds since the last accepted sample. */
  idleFor(t: number): number {
    return t - this.lastT;
  }
}
