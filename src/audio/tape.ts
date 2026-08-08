/**
 * Tape timeline + loop/chop model (Phase 5A core).
 *
 * Pure math, no Web Audio objects, so it is directly unit-testable and shared
 * by the 5A node scheduler and the 5B worklet.
 *
 * Two invariants:
 *  1. The playhead is DERIVED from context time through the integrated rate
 *     curve — never ticked, never linearly approximated across a glide.
 *  2. Every future scheduled seam is computed on that SAME integrated timeline,
 *     so a varispeed or glide change invalidates and re-derives pending seams
 *     instead of letting a stale absolute time fire (binding correction #5).
 */

import { integratedDistance, rateAt, settled, timeForDistance, type GlideSegment } from "./glide";
import { inertiaDistance, inertiaRateAt, type InertiaSegment } from "./inertia";


export interface LoopWindow {
  /** Normalized 0..1 of buffer duration — portable across 44.1/48 kHz. */
  start: number;
  end: number;
  /** Chops subdivide the ACTIVE WINDOW: loopLength = windowWidth / chopDiv. */
  chopDiv: number;
  /** Which chop slice of the window is currently looping. */
  chopIndex: number;
  enabled: boolean;
  reverse: boolean;
}

export const DEFAULT_WINDOW: LoopWindow = {
  start: 0,
  end: 1,
  chopDiv: 1,
  chopIndex: 0,
  enabled: false,
  reverse: false,
};

export interface LoopBoundsSeconds {
  start: number;
  end: number;
  length: number;
}

/** Resolve the normalized window + chop into absolute seconds for a buffer. */
export function resolveLoop(win: LoopWindow, durationS: number): LoopBoundsSeconds {
  const a = Math.min(win.start, win.end);
  const b = Math.max(win.start, win.end);
  const width = Math.max(1e-6, b - a);
  const div = Math.max(1, Math.floor(win.chopDiv));
  const idx = ((win.chopIndex % div) + div) % div;
  const sliceW = width / div;
  const start = (a + idx * sliceW) * durationS;
  const length = sliceW * durationS;
  return { start, end: start + length, length };
}

/**
 * Rate timeline: a current constant rate, optionally overridden by an in-flight
 * exponential glide. Positions are integrated exactly.
 */
export class TapeTimeline {
  private anchorCtx = 0;
  private anchorPos = 0;
  private rate = 1;
  private glide: GlideSegment | null = null;
  /**
   * Workstream 2: a finite transport-inertia ramp. It takes precedence over the
   * glide segment while in flight — the playhead integrates the SAME curve the
   * audio is playing, so wind-up / wind-down never desynchronise the position.
   */
  private inertia: InertiaSegment | null = null;

  constructor(rate = 1) {
    this.rate = rate;
  }

  /** Hard re-anchor (transport start, seek, seam wrap). */
  anchor(ctxTime: number, position: number) {
    this.anchorPos = position;
    this.anchorCtx = ctxTime;
    if (this.inertia) {
      const r = this.rateAtTime(ctxTime);
      const remaining = Math.max(0.01, this.inertia.startAt + this.inertia.durationS - ctxTime);
      this.inertia = { ...this.inertia, startAt: ctxTime, from: r, durationS: remaining };
      return;
    }
    if (this.glide) {
      // Restart the glide from its instantaneous value so the integral stays continuous.
      const r = this.rateAtTime(ctxTime);
      this.glide = { startAt: ctxTime, from: r, to: this.glide.to, tau: this.glide.tau };
    }
  }

  currentRate(ctxTime: number): number {
    return this.rateAtTime(ctxTime);
  }

  targetRate(): number {
    if (this.inertia) return this.inertia.to;
    return this.glide ? this.glide.to : this.rate;
  }

  /** The rate the tape returns to once inertia finishes (the musical rate). */
  musicalRate(): number {
    return this.glide ? this.glide.to : this.rate;
  }

  inertiaSegment(): InertiaSegment | null {
    return this.inertia;
  }

  /** True while a wind-up / wind-down ramp is still in flight. */
  inertiaActive(ctxTime: number): boolean {
    return this.inertia != null && ctxTime < this.inertia.startAt + this.inertia.durationS;
  }

  /**
   * Start a finite inertia ramp at `ctxTime`. The glide segment is folded into
   * the anchor first so no distance is lost.
   */
  startInertia(ctxTime: number, seg: InertiaSegment) {
    this.anchorPos = this.positionAt(ctxTime);
    this.anchorCtx = ctxTime;
    this.glide = null;
    this.inertia = { ...seg, startAt: ctxTime };
  }

  /** Ramp finished (or was superseded): settle on a constant rate. */
  endInertia(ctxTime: number, rate: number) {
    this.anchorPos = this.positionAt(ctxTime);
    this.anchorCtx = ctxTime;
    this.inertia = null;
    this.rate = rate;
  }

  private rateAtTime(ctxTime: number): number {
    if (this.inertia) {
      const dt = ctxTime - this.inertia.startAt;
      if (dt < this.inertia.durationS) return inertiaRateAt(this.inertia, dt);
      return this.inertia.to;
    }
    if (!this.glide) return this.rate;
    const dt = ctxTime - this.glide.startAt;
    if (settled(this.glide, dt)) return this.glide.to;
    return rateAt(this.glide, dt);
  }


  /**
   * Begin a glide to `to`. Re-anchors at `ctxTime` so everything scheduled
   * afterwards is derived from the new curve.
   */
  glideTo(ctxTime: number, to: number, tau: number) {
    const from = this.rateAtTime(ctxTime);
    this.anchorPos = this.positionAt(ctxTime);
    this.anchorCtx = ctxTime;
    if (tau <= 0) {
      this.rate = to;
      this.glide = null;
      return;
    }
    this.rate = to;
    this.glide = { startAt: ctxTime, from, to, tau };
  }

  /** Instant rate change with no glide. */
  setRate(ctxTime: number, rate: number) {
    this.anchorPos = this.positionAt(ctxTime);
    this.anchorCtx = ctxTime;
    this.rate = rate;
    this.glide = null;
  }

  /** Media position at a context time, integrating any in-flight glide. */
  positionAt(ctxTime: number): number {
    const dt = ctxTime - this.anchorCtx;
    if (dt <= 0) return this.anchorPos;
    if (!this.glide) return this.anchorPos + dt * this.rate;
    const offset = this.anchorCtx - this.glide.startAt;
    const total = integratedDistance(this.glide, offset + dt) - integratedDistance(this.glide, offset);
    return this.anchorPos + total;
  }

  /**
   * Context time at which the playhead will reach `position`, on the SAME
   * integrated curve. Returns null when unreachable (rate glides to 0).
   */
  timeAtPosition(nowCtx: number, position: number): number | null {
    const here = this.positionAt(nowCtx);
    const distance = position - here;
    if (distance <= 0) return nowCtx;
    if (!this.glide) return this.rate > 0 ? nowCtx + distance / this.rate : null;
    const offset = nowCtx - this.glide.startAt;
    const base = integratedDistance(this.glide, offset);
    const shifted: GlideSegment = { ...this.glide };
    const solve = (target: number): number | null => {
      // Solve d(offset + x) - d(offset) = target for x, by bisection on the
      // absolute curve (strictly increasing for positive rates).
      const abs = timeForDistance(shifted, base + target);
      return abs == null ? null : abs - offset;
    };
    const x = solve(distance);
    return x == null ? null : nowCtx + x;
  }
}

/**
 * A pending seam: the context time at which the next crossfade must begin, plus
 * the timeline generation it was derived from. Any rate change bumps the
 * generation and forces a recalculation.
 */
export interface PendingSeam {
  atCtxTime: number;
  /** Media position the outgoing source will have reached. */
  atPosition: number;
  /** Media position the incoming source starts from. */
  toPosition: number;
  generation: number;
}

/** Next seam for a looping track, derived from the integrated timeline. */
export function nextSeam(
  timeline: TapeTimeline,
  nowCtx: number,
  bounds: LoopBoundsSeconds,
  generation: number,
  reverse = false,
): PendingSeam | null {
  const boundary = reverse ? bounds.start : bounds.end;
  const pos = timeline.positionAt(nowCtx);
  if (reverse) {
    // Reverse playback is modelled as a forward walk on a reversed buffer, so
    // the caller passes already-mirrored bounds; keep one code path.
    if (pos <= boundary) return null;
  }
  const at = timeline.timeAtPosition(nowCtx, boundary);
  if (at == null) return null;
  return {
    atCtxTime: at,
    atPosition: boundary,
    toPosition: reverse ? bounds.end : bounds.start,
    generation,
  };
}
