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
import {
  SCRATCH_TUNING,
  blendScratchScrub,
  clampVelocity,
  decayedScratch,
  displacementToScrubVelocity,
  handVelocityToTapeVelocity,
} from "@/audio/masterScratch";


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
 * Hybrid scratch/scrub controller.
 *
 * One gesture, two blended components (see `masterScratch.ts`):
 *  - MOTION: an impulse taken from hand speed, decaying with `scratchDecayMs`.
 *  - POSITION: a sustained scrub from how far the rocker is held from the grab
 *    point, through a squared curve.
 *
 * `sample()` on every pointermove, `poll()` on a timer so the decay and the
 * sustained hold keep being commanded while the finger is motionless.
 */
export class ScratchScrubController {
  private grabY: number;
  private lastY: number;
  private lastT: number;
  /** Latest scratch impulse and when it was taken. */
  private impulse = 0;
  private impulseT: number;
  /** Held displacement in [-1,+1] — visual travel AND the scrub component. */
  displacement = 0;
  /** Last blended velocity produced. */
  velocity = 0;

  constructor(grabY: number, t: number) {
    this.grabY = grabY;
    this.lastY = grabY;
    this.lastT = t;
    this.impulseT = t;
  }

  private blend(t: number): number {
    const motion = decayedScratch(this.impulse, t - this.impulseT);
    this.velocity = blendScratchScrub(motion, displacementToScrubVelocity(this.displacement));
    return this.velocity;
  }

  /** New pointer position → blended signed master velocity. */
  sample(userY: number, t: number): number {
    if (!Number.isFinite(userY) || !Number.isFinite(t)) return this.velocity;
    const dt = t - this.lastT;
    if (dt <= 0) return this.velocity;
    const dy = userY - this.lastY;
    this.lastY = userY;
    this.lastT = t;
    this.displacement = rockerDisplacement(userY, this.grabY);
    const next = handVelocityToTapeVelocity(dy, dt);
    if (next !== 0) {
      this.impulse = next;
      this.impulseT = t;
    }
    return this.blend(t);
  }

  /**
   * Time-only update: the scratch transient decays and the held position takes
   * over. A hand held away from centre keeps scrubbing; near centre it settles
   * to zero.
   */
  poll(t: number): number {
    if (!Number.isFinite(t)) return this.velocity;
    return this.blend(t);
  }

  /** Milliseconds since the last accepted sample. */
  idleFor(t: number): number {
    return t - this.lastT;
  }
}

