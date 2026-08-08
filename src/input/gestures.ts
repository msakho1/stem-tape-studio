import type { Control } from "@/device/geometry";

/** Raw, uninterpreted input. One entry per pointer/keyboard transition. */
export interface RawInputEvent {
  id: number;
  control: Control;
  phase: "down" | "up" | "cancel";
  pointerId: number | "keyboard";
  t: number;
  x?: number | undefined;
  y?: number | undefined;
}

export type Gesture =
  | { type: "tap"; control: Control; count: number; t: number }
  | { type: "holdStart"; control: Control; duration: number; t: number }
  | { type: "holdEnd"; control: Control; duration: number; t: number }
  | { type: "tapThenHold"; control: Control; t: number }
  | { type: "chordStart"; controls: Control[]; t: number }
  | { type: "chordRelease"; controls: Control[]; releaseSpreadMs: number; t: number }
  | { type: "cancel"; control: Control; t: number };

export interface GestureTimings {
  /** Pointer-down to holdStart. */
  holdMs: number;
  /** Documented Tape Looper v2.6 long hold (dim / full lights). */
  longHoldMs: number;
  /** Max gap between taps for a multi-tap sequence. */
  multiTapGapMs: number;
  /** Max gap between a tap's release and a following press for tap-then-hold. */
  tapThenHoldGapMs: number;
  /** Two controls pressed within this window count as a chord. */
  chordWindowMs: number;
  /** Chord members released within this window count as "released together". */
  chordReleaseSpreadMs: number;
}

/**
 * Defaults, NOT measurements. Every value here is a guess until it is measured
 * on a physical SP-1 (see the verification checklist). They live in one place
 * precisely so the Mapping Lab can retune them.
 */
export const DEFAULT_TIMINGS: GestureTimings = {
  holdMs: 450,
  longHoldMs: 5000,
  multiTapGapMs: 300,
  tapThenHoldGapMs: 300,
  chordWindowMs: 120,
  chordReleaseSpreadMs: 120,
};

interface PressRecord {
  control: Control;
  pointerId: number | "keyboard";
  downAt: number;
  holdFired: boolean;
  longHoldFired: boolean;
  holdTimer: ReturnType<typeof setTimeout> | null;
  longHoldTimer: ReturnType<typeof setTimeout> | null;
  moved: boolean;
}

interface TapRecord {
  count: number;
  lastReleaseAt: number;
  timer: ReturnType<typeof setTimeout> | null;
}

export type GestureListener = (g: Gesture) => void;
export type RawListener = (e: RawInputEvent) => void;

/**
 * Continuous controls. Faders are drag controls: they must never enter
 * button-hold recognition (no holdStart / holdEnd / tapThenHold) and never join
 * a chord. Pointer-down starts fader interaction immediately.
 * A fader still emits `tap` when it was pressed and released without moving,
 * which is what the v2.6 "double-tap = reverse" row is built on.
 */
export function isContinuousControl(control: Control): boolean {
  return control.startsWith("fader-");
}


/**
 * Raw pointer runtime + gesture interpreter.
 *
 * Deliberately framework-free: no React, no DOM queries. Components feed it
 * normalised control events; it emits semantic gestures.
 *
 * Multi-tap without sluggishness: a tap fires OPTIMISTICALLY on release with
 * count = 1, and a following tap inside the window emits the same control again
 * with count = 2, 3, 4... Consumers treat a higher count as a revision of the
 * lower one rather than waiting out the window, so a single musical tap is
 * never delayed.
 */
export class GestureEngine {
  private presses = new Map<Control, PressRecord>();
  private taps = new Map<Control, TapRecord>();
  private gestureListeners = new Set<GestureListener>();
  private rawListeners = new Set<RawListener>();
  private chordActive: Control[] | null = null;
  private chordReleaseStartedAt: number | null = null;
  private chordReleased: Control[] = [];
  private chordTimer: ReturnType<typeof setTimeout> | null = null;
  private seq = 0;

  timings: GestureTimings = { ...DEFAULT_TIMINGS };

  onGesture(fn: GestureListener): () => void {
    this.gestureListeners.add(fn);
    return () => this.gestureListeners.delete(fn);
  }

  onRaw(fn: RawListener): () => void {
    this.rawListeners.add(fn);
    return () => this.rawListeners.delete(fn);
  }

  get heldControls(): Control[] {
    return [...this.presses.keys()];
  }

  isHeld(control: Control): boolean {
    return this.presses.has(control);
  }

  private emit(g: Gesture) {
    this.gestureListeners.forEach((fn) => fn(g));
  }

  private emitRaw(e: RawInputEvent) {
    this.rawListeners.forEach((fn) => fn(e));
  }

  press(control: Control, pointerId: number | "keyboard", t = performance.now(), x?: number, y?: number) {
    if (this.presses.has(control)) return; // ignore auto-repeat / duplicate
    const rec: PressRecord = {
      control,
      pointerId,
      downAt: t,
      holdFired: false,
      longHoldFired: false,
      holdTimer: null,
      longHoldTimer: null,
      moved: false,
    };

    rec.holdTimer = setTimeout(() => {
      rec.holdFired = true;
      const prevTap = this.taps.get(control);
      const isTapThenHold =
        prevTap != null &&
        prevTap.count > 0 &&
        rec.downAt - prevTap.lastReleaseAt <= this.timings.tapThenHoldGapMs;
      if (isTapThenHold) {
        this.emit({ type: "tapThenHold", control, t: performance.now() });
        this.clearTaps(control);
      }
      this.emit({ type: "holdStart", control, duration: this.timings.holdMs, t: performance.now() });
    }, this.timings.holdMs);

    rec.longHoldTimer = setTimeout(() => {
      rec.longHoldFired = true;
      this.emit({ type: "holdStart", control, duration: this.timings.longHoldMs, t: performance.now() });
    }, this.timings.longHoldMs);

    this.presses.set(control, rec);
    this.emitRaw({ id: ++this.seq, control, phase: "down", pointerId, t, x, y });
    this.evaluateChordStart(t);
  }

  markMoved(control: Control) {
    const rec = this.presses.get(control);
    if (rec) rec.moved = true;
  }

  release(control: Control, pointerId: number | "keyboard", t = performance.now()) {
    const rec = this.presses.get(control);
    if (!rec) return;
    this.clearTimers(rec);
    this.presses.delete(control);
    this.emitRaw({ id: ++this.seq, control, phase: "up", pointerId, t });

    const duration = t - rec.downAt;

    if (this.chordActive?.includes(control)) {
      this.registerChordRelease(control, t);
    }

    if (rec.holdFired) {
      this.emit({ type: "holdEnd", control, duration, t });
      this.clearTaps(control);
      return;
    }

    // Optimistic tap: fire now, revise upward if more taps arrive.
    const prev = this.taps.get(control);
    const within = prev != null && t - prev.lastReleaseAt <= this.timings.multiTapGapMs;
    const count = within ? prev!.count + 1 : 1;
    if (prev?.timer) clearTimeout(prev.timer);
    const timer = setTimeout(() => this.taps.delete(control), this.timings.multiTapGapMs);
    this.taps.set(control, { count, lastReleaseAt: t, timer });
    this.emit({ type: "tap", control, count, t });
  }

  /** Pointer cancellation / lost capture. Never leaves a stuck control. */
  cancel(control: Control, pointerId: number | "keyboard", t = performance.now()) {
    const rec = this.presses.get(control);
    if (!rec) return;
    this.clearTimers(rec);
    this.presses.delete(control);
    this.clearTaps(control);
    if (this.chordActive?.includes(control)) this.registerChordRelease(control, t);
    this.emitRaw({ id: ++this.seq, control, phase: "cancel", pointerId, t });
    this.emit({ type: "cancel", control, t });
  }

  /** Force-release everything. Used on blur / visibilitychange / unmount. */
  releaseAll(t = performance.now()) {
    for (const control of [...this.presses.keys()]) {
      this.cancel(control, this.presses.get(control)!.pointerId, t);
    }
  }

  private clearTimers(rec: PressRecord) {
    if (rec.holdTimer) clearTimeout(rec.holdTimer);
    if (rec.longHoldTimer) clearTimeout(rec.longHoldTimer);
  }

  private clearTaps(control: Control) {
    const prev = this.taps.get(control);
    if (prev?.timer) clearTimeout(prev.timer);
    this.taps.delete(control);
  }

  private evaluateChordStart(t: number) {
    if (this.presses.size < 2 || this.chordActive) return;
    const members = [...this.presses.values()]
      .filter((p) => t - p.downAt <= this.timings.chordWindowMs || p.downAt <= t)
      .map((p) => p.control);
    if (members.length < 2) return;
    this.chordActive = members;
    this.chordReleased = [];
    this.chordReleaseStartedAt = null;
    this.emit({ type: "chordStart", controls: members, t });
  }

  private registerChordRelease(control: Control, t: number) {
    if (!this.chordActive) return;
    if (!this.chordReleased.includes(control)) this.chordReleased.push(control);
    if (this.chordReleaseStartedAt == null) this.chordReleaseStartedAt = t;

    if (this.chordTimer) clearTimeout(this.chordTimer);
    const finish = () => {
      if (!this.chordActive) return;
      const spread = t - (this.chordReleaseStartedAt ?? t);
      this.emit({
        type: "chordRelease",
        controls: this.chordActive,
        releaseSpreadMs: Math.round(Math.max(spread, t - (this.chordReleaseStartedAt ?? t))),
        t: performance.now(),
      });
      this.chordActive = null;
      this.chordReleased = [];
      this.chordReleaseStartedAt = null;
      this.chordTimer = null;
    };

    const allReleased = this.chordActive.every((c) => !this.presses.has(c));
    if (allReleased) {
      finish();
    } else {
      this.chordTimer = setTimeout(finish, this.timings.chordReleaseSpreadMs);
    }
  }

  dispose() {
    this.releaseAll();
    if (this.chordTimer) clearTimeout(this.chordTimer);
    this.gestureListeners.clear();
    this.rawListeners.clear();
  }
}

export function describeGesture(g: Gesture): string {
  switch (g.type) {
    case "tap":
      return `tap ×${g.count} · ${g.control}`;
    case "holdStart":
      return `hold start (${g.duration}ms) · ${g.control}`;
    case "holdEnd":
      return `hold end (${Math.round(g.duration)}ms) · ${g.control}`;
    case "tapThenHold":
      return `tap-then-hold · ${g.control}`;
    case "chordStart":
      return `chord start · ${g.controls.join(" + ")}`;
    case "chordRelease":
      return `chord release (spread ${g.releaseSpreadMs}ms) · ${g.controls.join(" + ")}`;
    case "cancel":
      return `cancel · ${g.control}`;
  }
}
