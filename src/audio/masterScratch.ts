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
 */
export const SCRATCH_TUNING = {
  /** Conservative first-pass ceiling on |velocity| (× musical rate). */
  maxAbsVelocity: 3.5,
  /** One-pole gesture response, ms. */
  responseMs: 12,
  /** Release glide back to the musical rate, ms. */
  releaseMs: 60,
  /** No gesture input for this long ⇒ velocity neutralises. */
  neutralTimeoutMs: 90,
  /** Ramp applied to each commanded velocity step, ms (click suppression). */
  velocityRampMs: 6,
} as const;

export function clampVelocity(v: number, max = SCRATCH_TUNING.maxAbsVelocity): number {
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
