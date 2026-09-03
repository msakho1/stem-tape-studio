/**
 * S2 — signed master head.
 *
 * WHY ONE POSITION, NOT FOUR VELOCITIES
 * -------------------------------------
 * Sending "the same velocity at the same applyAtContextFrame" to four
 * processors still leaves four accumulators. This module defines the opposite
 * contract:
 *
 *   MASTER.position  — one authoritative position in MASTER FRAMES, i.e.
 *                      frames of song time at the AudioContext sample rate.
 *   MASTER.velocity  — one signed value, master frames per output frame.
 *
 * Every processor integrates that ONE program (same engage frame, same signed
 * velocity, same ramp length, same loop geometry, same order of operations, so
 * bit-identical IEEE-754 results), and each lane's read pointer is a pure
 * mapping of it:
 *
 *   sourceFrame_lane = masterFrame * rateScale_lane,  rateScale = srcSR / ctxSR
 *
 * No lane owns a master-scratch accumulator, so no lane can drift from another
 * no matter how long the gesture runs. `rateScale` is a per-lane CONSTANT, so
 * lanes decoded at a foreign sample rate stay coherent too — the master domain
 * is the shared normalization, not an assumption that all stems match.
 *
 * (In this app `decodeAudioData` resamples every stem to the context rate, so
 * rateScale is 1.0 in practice; the mapping is kept general rather than relied
 * upon, per the invariant requirement.)
 */

/**
 * S3 tuning surface. These are STARTING VALUES to be tuned by feel, not
 * product requirements — every gesture constant lives here so tuning is a
 * one-file change.
 *
 * HYBRID PHYSICAL ROCKER: SCRATCH + SCRUB, ONE GESTURE
 * ----------------------------------------------------
 * Two components are summed, never switched between:
 *
 *   MOTION  (scratch)  transient, from hand SPEED (Δy/Δt), decays with
 *                      `scratchDecayMs` so a flick is an impulse, not a latch.
 *   POSITION (scrub)   sustained, from how far the rocker is HELD from the
 *                      grab point, through a squared curve so the centre has
 *                      fine control and the extremes shuttle.
 *
 *   v = clamp(motion·exp(-Δt/τ) + scrub(d), ±combinedMaxVelocity)
 *
 * A held-still hand therefore does NOT force zero: it settles onto the scrub
 * velocity its position implies, and only reads zero near the centre.
 */
export const SCRATCH_TUNING = {
  /** Absolute engine ceiling on |velocity| (× musical rate). */
  maxAbsVelocity: 3.5,
  /** Ceiling on the TRANSIENT hand-motion (scratch) component. */
  scratchMaxVelocity: 1.75,
  /** Ceiling on the SUSTAINED held-position (scrub) component. */
  scrubMaxVelocity: 2.0,
  /** Ceiling on the blended command. */
  combinedMaxVelocity: 2.5,
  /** Exponential decay time constant of the scratch impulse, ms. */
  scratchDecayMs: 110,
  /** Held displacement below this |d| is the centre: no scrub. */
  scrubDeadband: 0.08,
  /** Curve exponent of the held-position response (2 = squared). */
  scrubCurveExponent: 2,
  /** One-pole gesture response, ms. */
  responseMs: 12,
  /** Release glide back to the musical rate, ms. */
  releaseMs: 60,
  /** No gesture input for this long ⇒ velocity neutralises. */
  neutralTimeoutMs: 90,
  /** Ramp applied to each commanded velocity step, ms (click suppression). */
  velocityRampMs: 6,
  /**
   * Hand speed, in SVG user units per second, that maps to 1.0× tape velocity.
   * Deliberately far coarser than literal finger speed: an ordinary swipe is
   * hundreds of units/second and must NOT read as multiples of tape speed.
   */
  handUnitsPerSecondAtUnitRate: 420,
  /** Hand speed below this is treated as a held-still hand (dead band). */
  handDeadbandUnitsPerSecond: 30,
  /**
   * Legacy: the old "held still ⇒ zero" timeout. The hybrid controller does not
   * use it; a still hand now settles onto its held scrub velocity.
   */
  handStopTimeoutMs: 60,
  /** Longest Δt trusted for a hand-velocity sample (throttled frames). */
  handMaxSampleMs: 120,
} as const;

/**
 * Pointer motion → signed TRANSIENT scratch velocity (the impulse, before decay).
 *
 * `dyUnits` is SVG-user-unit travel (positive = downward on screen) and `dtMs`
 * the interval it took. Upward motion pushes the tape forward, so the sign is
 * inverted. Below the dead band the hand counts as stopped.
 */
export function handVelocityToTapeVelocity(
  dyUnits: number,
  dtMs: number,
  tuning: {
    handUnitsPerSecondAtUnitRate: number;
    handDeadbandUnitsPerSecond: number;
    handMaxSampleMs: number;
    scratchMaxVelocity: number;
  } = SCRATCH_TUNING,
): number {
  if (!Number.isFinite(dyUnits) || !Number.isFinite(dtMs)) return 0;
  const dt = Math.min(Math.max(dtMs, 1), tuning.handMaxSampleMs);
  const unitsPerSecond = (-dyUnits * 1000) / dt;
  if (Math.abs(unitsPerSecond) < tuning.handDeadbandUnitsPerSecond) return 0;
  return clampVelocity(unitsPerSecond / tuning.handUnitsPerSecondAtUnitRate, tuning.scratchMaxVelocity);
}

/**
 * Held rocker displacement d ∈ [-1,+1] → SUSTAINED scrub velocity.
 * Nonlinear: fine control near the centre, shuttle at the extremes.
 */
export function displacementToScrubVelocity(
  d: number,
  tuning: {
    scrubDeadband: number;
    scrubCurveExponent: number;
    scrubMaxVelocity: number;
  } = SCRATCH_TUNING,
): number {
  if (!Number.isFinite(d)) return 0;
  const clamped = d > 1 ? 1 : d < -1 ? -1 : d;
  const mag = Math.abs(clamped);
  if (mag <= tuning.scrubDeadband) return 0;
  const norm = (mag - tuning.scrubDeadband) / (1 - tuning.scrubDeadband);
  const shaped = Math.pow(norm, tuning.scrubCurveExponent) * tuning.scrubMaxVelocity;
  return clamped < 0 ? -shaped : shaped;
}

/** Exponential decay of the scratch impulse over `ageMs`. */
export function decayedScratch(impulse: number, ageMs: number, tauMs = SCRATCH_TUNING.scratchDecayMs): number {
  if (!Number.isFinite(impulse) || !Number.isFinite(ageMs) || ageMs <= 0) return impulse || 0;
  if (tauMs <= 0) return 0;
  return impulse * Math.exp(-ageMs / tauMs);
}

/**
 * The blend the rocker commands: transient scratch on top of sustained scrub.
 * No mode switch exists — one is simply decaying while the other persists.
 */
export function blendScratchScrub(
  motion: number,
  scrub: number,
  max = SCRATCH_TUNING.combinedMaxVelocity,
): number {
  return clampVelocity((Number.isFinite(motion) ? motion : 0) + (Number.isFinite(scrub) ? scrub : 0), max);
}

export function clampVelocity(v: number, max: number = SCRATCH_TUNING.maxAbsVelocity): number {
  if (!Number.isFinite(v)) return 0;
  return v > max ? max : v < -max ? -max : v;
}


export function msToFrames(ms: number, sampleRate: number): number {
  return Math.max(0, Math.round((ms / 1000) * sampleRate));
}

/** Master frames ⇒ this lane's source frames. Pure mapping, never integrated. */
export function masterToSourceFrame(masterFrame: number, rateScale: number): number {
  return masterFrame * rateScale;
}

/** Inverse mapping, used only to report a lane's position in master terms. */
export function sourceToMasterFrame(sourceFrame: number, rateScale: number): number {
  return rateScale === 0 ? sourceFrame : sourceFrame / rateScale;
}

/** Legacy compatibility: derive the old magnitude+direction pair from a signed velocity. */
export function derivedDirection(velocity: number): 1 | -1 {
  return velocity < 0 ? -1 : 1;
}

export function derivedRate(velocity: number): number {
  return Math.abs(velocity);
}

/**
 * Reference integration of the master program — the exact arithmetic the
 * processor performs, exposed so tests and the main thread can predict the
 * master position without guessing.
 */
export interface MasterProgramState {
  pos: number;
  vel: number;
  target: number;
  rampLeft: number;
  step: number;
  loopEnabled: boolean;
  loopStart: number;
  loopEnd: number;
  songFrames: number;
}

export function advanceMaster(m: MasterProgramState, frames: number): MasterProgramState {
  const s = { ...m };
  for (let i = 0; i < frames; i++) {
    if (s.rampLeft > 0) {
      s.vel += s.step;
      s.rampLeft--;
      if (s.rampLeft === 0) s.vel = s.target;
    } else {
      s.vel = s.target;
    }
    s.pos += s.vel;
    const len = s.loopEnd - s.loopStart;
    if (s.loopEnabled && len > 2) {
      while (s.pos >= s.loopEnd) s.pos -= len;
      while (s.pos < s.loopStart) s.pos += len;
    } else if (s.pos < 0) {
      s.pos = 0;
      s.vel = 0;
      s.target = 0;
      s.rampLeft = 0;
    } else if (s.pos > s.songFrames) {
      s.pos = s.songFrames;
      s.vel = 0;
      s.target = 0;
      s.rampLeft = 0;
    }
  }
  return s;
}
