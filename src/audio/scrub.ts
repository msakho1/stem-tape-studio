/**
 * Head scrub model (Phase 6 §5) — pure math, no Web Audio, no DOM.
 *
 * A scrub is NOT `head.offset = faderValue`. It is a *travelling read pointer*:
 * the control's motion is converted into an unwrapped frame delta, the audio
 * engine chases that target with a bounded velocity, and the sound produced
 * while travelling is the tape scrub. Both the worklet kernel and the node
 * fallback consume the same numbers produced here, so the two paths cannot
 * disagree about direction, distance or final position.
 *
 * Semantics:
 *  - the delta is UNWRAPPED (10 % → 90 % is +80 % forward, never −20 %), so the
 *    audible direction always matches the finger;
 *  - the final position is ABSOLUTE and exact to one source frame;
 *  - velocity is clamped to a documented safe range so a flick cannot ask the
 *    kernel for an unbounded read step.
 */

export type ScrubEventType = "head.scrub.start" | "head.scrub.preview" | "head.scrub.end" | "head.scrub.cancel";

export interface ScrubEvent {
  type: ScrubEventType;
  headId: number;
  pointerId: number;
  normalizedPosition: number;
  targetSourceFrame: number;
  contextFrame: number;
  inputTimestamp: number;
}

/** Maximum read speed, in source frames per output frame (± this value). */
export const MAX_SCRUB_RATE = 32;
/** Below this |rate| the tape is effectively stationary and must fade out. */
export const SCRUB_SILENCE_RATE = 0.02;
/** Exponential lag: how long the audible pointer takes to reach the target. */
export const SCRUB_LAG_S = 0.03;
/** Scrub ↔ normal-playback crossfade. */
export const SCRUB_XFADE_S = 0.012;

export function clampScrubRate(rate: number): number {
  if (!Number.isFinite(rate)) return 0;
  return rate > MAX_SCRUB_RATE ? MAX_SCRUB_RATE : rate < -MAX_SCRUB_RATE ? -MAX_SCRUB_RATE : rate;
}

export function mod(x: number, n: number): number {
  return ((x % n) + n) % n;
}

export interface ScrubPreview {
  /** Signed, unwrapped travel requested by this movement, in source frames. */
  deltaFrames: number;
  /** Absolute (wrapped into the cycle) frame the head is being driven toward. */
  targetSourceFrame: number;
  /** Signed frames per second implied by this movement. */
  velocityFramesPerSecond: number;
  direction: 1 | -1 | 0;
}

/**
 * Per-head, per-gesture tracker. One instance lives for exactly one pointer
 * gesture; it converts normalized fader positions into unwrapped frame travel.
 */
export class ScrubTracker {
  previewCount = 0;
  lastNormalized: number;
  lastTimestamp: number;
  /** Unwrapped target, in source frames relative to the cycle start. */
  unwrapped: number;
  lastVelocity = 0;

  constructor(
    readonly headId: number,
    readonly pointerId: number,
    readonly cycleStartFrame: number,
    readonly cycleFrames: number,
    /** Head read position (absolute source frames) at gesture start. */
    readonly startSourceFrame: number,
    startNormalized: number,
    startTimestamp: number,
  ) {
    this.lastNormalized = startNormalized;
    this.lastTimestamp = startTimestamp;
    this.unwrapped = startSourceFrame - cycleStartFrame;
  }

  preview(normalized: number, timestamp: number): ScrubPreview {
    const dNorm = normalized - this.lastNormalized;
    const deltaFrames = dNorm * this.cycleFrames;
    const dt = Math.max(1e-4, (timestamp - this.lastTimestamp) / 1000);
    this.lastNormalized = normalized;
    this.lastTimestamp = timestamp;
    this.unwrapped += deltaFrames;
    this.previewCount++;
    const v = deltaFrames / dt;
    this.lastVelocity = v;
    return {
      deltaFrames,
      targetSourceFrame: this.cycleStartFrame + mod(this.unwrapped, this.cycleFrames),
      velocityFramesPerSecond: v,
      direction: deltaFrames > 0 ? 1 : deltaFrames < 0 ? -1 : 0,
    };
  }

  /** Absolute frame commanded by a release at `normalized` (exact, wrapped). */
  finalFrame(normalized: number): number {
    return this.cycleStartFrame + mod(normalized, 1) * this.cycleFrames;
  }
}

/** Rolling log of emitted scrub events — the diagnostic evidence trail. */
export class ScrubLog {
  readonly events: ScrubEvent[] = [];
  limit = 200;
  push(e: ScrubEvent) {
    this.events.unshift(e);
    if (this.events.length > this.limit) this.events.length = this.limit;
  }
  countFor(headId: number, type: ScrubEventType): number {
    return this.events.filter((e) => e.headId === headId && e.type === type).length;
  }
  clear() {
    this.events.length = 0;
  }
}
