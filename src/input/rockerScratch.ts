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
  slewVelocity,
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
 * Scratch-first rocker controller.
 *
 * PHASE "scratch" (always the entry phase)
 *   velocity = decayed hand-motion impulse ONLY. A stationary finger stops the
 *   record even while the rocker is physically displaced. Sustained
 *   displacement contributes nothing.
 *
 * PHASE "scrub" (earned, never assumed)
 *   Entered only after an uninterrupted `scrubQualifyMs` hold on ONE side of
 *   centre with no reversal and no centre crossing. Displacement then fades in
 *   over `scrubEnterFadeMs` and sustains, with hand motion still layered on top.
 *
 * The qualification timer resets on: centre crossing, side change, return to
 * neutral, or a significant direction reversal of the hand.
 */
export type RockerPhase = "scratch" | "scrub";

export class ScratchScrubController {
  private grabY: number;
  private lastY: number;
  private lastT: number;
  /** Latest scratch impulse and when it was taken. */
  private impulse = 0;
  private impulseT: number;
  /** Which side of centre the hold currently qualifies on (0 = neutral). */
  private holdSide: -1 | 0 | 1 = 0;
  /** When the current uninterrupted same-side hold began. */
  private holdSinceT: number | null = null;
  /** When SCRUB was entered, for the smooth fade-in. */
  private scrubSinceT: number | null = null;
  /** Held displacement in [-1,+1] — visual travel, and scrub once qualified. */
  displacement = 0;
  /** Current phase. Internal only: there is no UI mode. */
  phase: RockerPhase = "scratch";
  /** Last velocity produced. */
  velocity = 0;
  /** When `velocity` was last produced, for the direction-change slew. */
  private velocityT: number;

  constructor(grabY: number, t: number) {
    this.grabY = grabY;
    this.lastY = grabY;
    this.lastT = t;
    this.impulseT = t;
    this.velocityT = t;
  }

  /** Milliseconds of uninterrupted qualifying hold so far. */
  holdMs(t: number): number {
    return this.holdSinceT == null ? 0 : t - this.holdSinceT;
  }

  /**
   * Public reset: FUNCTION lifted, or any other event that must revoke a
   * partially-earned hold. Returns the gesture to SCRATCH immediately.
   */
  revokeQualification(t: number): void {
    this.resetQualification(0, t);
  }

  /** Drop out of SCRUB and restart qualification from scratch. */
  private resetQualification(side: -1 | 0 | 1, t: number): void {
    this.holdSide = side;
    this.holdSinceT = side === 0 ? null : t;
    this.phase = "scratch";
    this.scrubSinceT = null;
  }

  /**
   * Emit a velocity through the direction-change ramp. The ramp is fast enough
   * to keep the reversal attack sharp, but it forces the signed head to travel
   * audibly through zero instead of flipping instantaneously.
   */
  private emit(target: number, t: number): number {
    const dt = t - this.velocityT;
    this.velocity = slewVelocity(this.velocity, target, dt > 0 ? dt : 0);
    this.velocityT = t;
    return this.velocity;
  }

  private compute(t: number): number {
    const motion = decayedScratch(this.impulse, t - this.impulseT);
    // Qualification is time-based, so it can complete while the finger is still.
    if (
      this.phase === "scratch" &&
      this.holdSinceT != null &&
      this.holdSide !== 0 &&
      t - this.holdSinceT >= SCRATCH_TUNING.scrubQualifyMs
    ) {
      this.phase = "scrub";
      this.scrubSinceT = t;
    }
    if (this.phase !== "scrub") {
      // SCRATCH: hand motion only. Displacement is deliberately ignored.
      return this.emit(clampVelocity(motion, SCRATCH_TUNING.scratchMaxVelocity), t);
    }
    const fade =
      this.scrubSinceT == null || SCRATCH_TUNING.scrubEnterFadeMs <= 0
        ? 1
        : Math.min(1, Math.max(0, (t - this.scrubSinceT) / SCRATCH_TUNING.scrubEnterFadeMs));
    const scrub = displacementToScrubVelocity(this.displacement) * fade;
    return this.emit(blendScratchScrub(motion, scrub), t);
  }

  /** New pointer position → signed master velocity for the current phase. */
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
      // A significant reversal of the HAND restarts qualification: back-and-forth
      // scratching can therefore continue forever without becoming scrub.
      if (this.impulse !== 0 && Math.sign(next) !== Math.sign(this.impulse)) {
        this.resetQualification(this.holdSide, t);
      }
      this.impulse = next;
      this.impulseT = t;
    }

    const band = SCRATCH_TUNING.scrubNeutralBand;
    const side: -1 | 0 | 1 = this.displacement > band ? 1 : this.displacement < -band ? -1 : 0;
    if (side !== this.holdSide) this.resetQualification(side, t);
    else if (side !== 0 && this.holdSinceT == null) this.holdSinceT = t;

    return this.compute(t);
  }

  /**
   * Time-only update: the scratch transient decays toward zero, and — only once
   * the deliberate hold has been earned — the sustained scrub fades in.
   */
  poll(t: number): number {
    if (!Number.isFinite(t)) return this.velocity;
    return this.compute(t);
  }

  /** Milliseconds since the last accepted sample. */
  idleFor(t: number): number {
    return t - this.lastT;
  }
}


