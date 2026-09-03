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
 * SCRATCH IS THE DEFAULT; SCRUB IS EARNED
 * ---------------------------------------
 * FUNCTION + rocker enters SCRATCH and stays there. In SCRATCH, velocity comes
 * from HAND MOTION ONLY — a stationary finger stops the record even while the
 * rocker is physically displaced. Sustained displacement contributes NOTHING
 * until the musician has deliberately held the rocker on ONE side of centre,
 * without reversing or re-crossing, for `scrubQualifyMs`. Only then does SCRUB
 * fade in over `scrubEnterFadeMs`, so the transition is smooth, not a jump.
 */
export const SCRATCH_TUNING = {
  /** Absolute engine ceiling on |velocity| (× musical rate). */
  maxAbsVelocity: 3.5,
  /** Ceiling on the SCRATCH (hand-motion) velocity. Turntable, not shuttle. */
  scratchMaxVelocity: 1.0,
  /** Ceiling on the SUSTAINED held-position (scrub) velocity. */
  scrubMaxVelocity: 2.0,
  /** Ceiling on the combined command once scrub has been earned. */
  combinedMaxVelocity: 2.5,
  /** Exponential decay time constant of the scratch impulse, ms. */
  scratchDecayMs: 55,
  /**
   * Uninterrupted same-side directional hold required before SCRUB may begin.
   * Reset by crossing centre, reversing, returning to neutral, or release.
   */
  scrubQualifyMs: 4000,
  /** Smooth fade-in of the sustained scrub component once qualified, ms. */
  scrubEnterFadeMs: 500,
  /** |displacement| at or below this counts as neutral/centre. */
  scrubNeutralBand: 0.12,
  /** Held displacement below this |d| produces no scrub even in SCRUB phase. */
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
   * Hand speed, in SVG user units per second, that maps to FULL scratch
   * velocity. Tuned so an ordinary performance stroke uses the whole expressive
   * range instead of crawling along the bottom of the curve.
   */
  handUnitsPerSecondAtMaxScratch: 520,
  /** Hand speed below this is treated as a held-still hand (dead band). */
  handDeadbandUnitsPerSecond: 6,
  /**
   * Slope of the response at very low hand speed. Small = fine resolution for a
   * slow deliberate drag; the remaining gain comes from the mid-speed curve.
   */
  scratchLowSpeedGain: 0.42,
  /**
   * Mid-speed curvature. >1 makes the middle of the range accelerate hard, so a
   * flick is clearly brighter than a drag without lifting the ceiling.
   */
  scratchCurveExponent: 2.4,
  /**
   * Normalised output above which a SOFT KNEE compresses rather than clips, so
   * the top of the range stays musical instead of slamming into a wall.
   */
  scratchKnee: 0.72,
  /**
   * Maximum change of commanded scratch velocity per millisecond. This is the
   * DIRECTION-CHANGE ramp: a reversal is forced to travel audibly through zero,
   * yet 1.0 → -1.0 still completes in ~57 ms, so the attack stays sharp.
   */
  scratchSlewPerMs: 0.035,
  /**
   * Legacy: the old "held still ⇒ zero" timeout. Superseded by the phase model.
   */
  handStopTimeoutMs: 60,
  /** Longest Δt trusted for a hand-velocity sample (throttled frames). */
  handMaxSampleMs: 120,
} as const;

/**
 * Normalised hand speed (0..∞) → normalised scratch magnitude (0..1).
 *
 * S-curve, not a plain power law:
 *   - low speed  : linear term `scratchLowSpeedGain` keeps fine resolution and
 *                  stops the centre going dead;
 *   - mid speed  : the `scratchCurveExponent` term accelerates hard, which is
 *                  where the "whiek" of a flick comes from;
 *   - near max   : a soft knee compresses instead of clipping.
 */
export function shapeHandSpeed(
  mag: number,
  tuning: {
    scratchLowSpeedGain: number;
    scratchCurveExponent: number;
    scratchKnee: number;
  } = SCRATCH_TUNING,
): number {
  if (!Number.isFinite(mag) || mag <= 0) return 0;
  const m = Math.min(1.6, mag);
  const g = tuning.scratchLowSpeedGain;
  const raw = g * m + (1 - g) * Math.pow(m, tuning.scratchCurveExponent);
  const knee = tuning.scratchKnee;
  if (raw <= knee) return Math.min(1, raw);
  const over = (raw - knee) / Math.max(1e-6, 1 - knee);
  return Math.min(1, knee + (1 - knee) * Math.tanh(over));
}

/**
 * Pointer motion → signed SCRATCH velocity (the impulse, before decay).
 *
 * `dyUnits` is SVG-user-unit travel (positive = downward on screen) and `dtMs`
 * the interval it took. Upward motion pushes the tape forward, so the sign is
 * inverted. Hand speed is normalised then S-curved, so a slow drag, a medium
 * stroke and a quick flick are three audibly different velocities.
 */
export function handVelocityToTapeVelocity(
  dyUnits: number,
  dtMs: number,
  tuning: {
    handUnitsPerSecondAtMaxScratch: number;
    handDeadbandUnitsPerSecond: number;
    handMaxSampleMs: number;
    scratchLowSpeedGain: number;
    scratchCurveExponent: number;
    scratchKnee: number;
    scratchMaxVelocity: number;
  } = SCRATCH_TUNING,
): number {
  if (!Number.isFinite(dyUnits) || !Number.isFinite(dtMs)) return 0;
  const dt = Math.min(Math.max(dtMs, 1), tuning.handMaxSampleMs);
  const unitsPerSecond = (-dyUnits * 1000) / dt;
  if (Math.abs(unitsPerSecond) < tuning.handDeadbandUnitsPerSecond) return 0;
  const x = unitsPerSecond / tuning.handUnitsPerSecondAtMaxScratch;
  const shaped = shapeHandSpeed(Math.abs(x), tuning) * tuning.scratchMaxVelocity;
  return clampVelocity(x < 0 ? -shaped : shaped, tuning.scratchMaxVelocity);
}

/**
 * Direction-change / anti-jump ramp. Limits how fast the COMMANDED velocity may
 * move, in velocity units per millisecond, so a reversal is heard as
 * "falling pitch → near stop → rising reverse pitch" rather than a hard flip.
 */
export function slewVelocity(
  previous: number,
  target: number,
  dtMs: number,
  perMs = SCRATCH_TUNING.scratchSlewPerMs,
): number {
  if (!Number.isFinite(target)) return previous;
  if (!Number.isFinite(previous)) return target;
  if (!Number.isFinite(dtMs) || dtMs <= 0 || perMs <= 0) return target;
  const step = perMs * dtMs;
  const delta = target - previous;
  if (Math.abs(delta) <= step) return target;
  return previous + Math.sign(delta) * step;
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
  const decayed = impulse * Math.exp(-ageMs / tauMs);
  // Snap the tail to exactly zero: a spent transient must not leave a residual
  // creep under a hand held at the centre.
  return Math.abs(decayed) < 1e-3 ? 0 : decayed;
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
