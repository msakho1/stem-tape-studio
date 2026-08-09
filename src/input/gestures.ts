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

/** Which hold threshold produced a holdStart / holdEnd. */
export type HoldLevel = "hold" | "power" | "long";

export type Gesture =
  | { type: "tap"; control: Control; count: number; t: number }
  | { type: "holdStart"; control: Control; level: HoldLevel; duration: number; t: number }
  | { type: "holdEnd"; control: Control; level: HoldLevel; duration: number; t: number }
  | { type: "tapThenHold"; control: Control; t: number }
  | { type: "chordStart"; controls: Control[]; t: number }
  | { type: "chordRelease"; controls: Control[]; releaseSpreadMs: number; t: number }
  | { type: "cancel"; control: Control; t: number };

export interface GestureTimings {
  /** Pointer-down to holdStart. */
  holdMs: number;
  /**
   * Power hold. v2.6 powers the unit with a FUNCTION hold, so it must NOT share
   * the general 450 ms button hold — every FN + X chord crosses that. Separate
   * and configurable so it can be retuned against the physical unit.
   */
  powerHoldMs: number;
  /** Documented Tape Looper v2.6 long hold (dim / full lights). */
  longHoldMs: number;
  /** Max gap between taps for a multi-tap sequence. */
  multiTapGapMs: number;
  /**
   * DEFERRED Track decision window. Track controls must NOT fire optimistically:
   * a first tap that mutes and then un-mutes when the second tap arrives is
   * audible. The first release opens this window; a timeout confirms one tap, a
   * second press claims the double-tap.
   */
  trackDecisionMs: number;
  /** Max gap between a tap's release and a following press for tap-then-hold. */
  tapThenHoldGapMs: number;
  /** Two controls pressed within this window count as a chord. */
  chordWindowMs: number;
  /** Chord members released within this window count as "released together". */
  chordReleaseSpreadMs: number;
}

/**
 * Tempo tapping (FN tap ×4 in rhythm) is an INACTIVITY timeout between
 * consecutive taps — it is NOT a fixed first-to-fourth window. The sequence is
 * kept alive as long as each gap is <= this value, so a 104 BPM tap-in
 * (576.9 ms gaps, 1730.8 ms first-to-fourth) is accepted; only a gap slower
 * than 40 BPM breaks the sequence and restarts the count at 1.
 */
export const TEMPO_TAP_IDLE_MS = 1500;

/**
 * Defaults, NOT measurements. Every value here is a guess until it is measured
 * on a physical SP-1 (see the verification checklist). They live in one place
 * precisely so the Mapping Lab can retune them.
 */
export const DEFAULT_TIMINGS: GestureTimings = {
  holdMs: 450,
  powerHoldMs: 1200,
  longHoldMs: 5000,
  multiTapGapMs: 300,
  // 200 ms sits inside the approved 180–220 ms band: short enough that a single
  // musical mute still feels immediate, long enough for a deliberate double-tap.
  trackDecisionMs: 200,
  tapThenHoldGapMs: 300,
  chordWindowMs: 120,
  chordReleaseSpreadMs: 120,
};

/**
 * Controls whose taps are DEFERRED instead of optimistic. Only the four Track
 * buttons: they are the only controls whose ×1 action is audible and
 * irreversible-sounding (mute / loop release).
 */
export function isDeferredControl(control: Control): boolean {
  return control.startsWith("track-button");
}



interface PressRecord {
  control: Control;
  pointerId: number | "keyboard";
  downAt: number;
  holdFired: boolean;
  powerHoldFired: boolean;
  longHoldFired: boolean;
  holdTimer: ReturnType<typeof setTimeout> | null;
  powerHoldTimer: ReturnType<typeof setTimeout> | null;
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
 * Two tap policies:
 *  - NON-deferred controls (Play, rocker, volume, FUNCTION, faders) keep the
 *    optimistic policy: `count = 1` fires on release and a following tap
 *    revises it upward, protected by the reducer's TxnSnapshot rollback.
 *  - DEFERRED controls (the four Track buttons) never fire optimistically. The
 *    first release opens a `trackDecisionMs` window; the timeout confirms one
 *    tap, a second press claims the double-tap and the second valid release
 *    confirms it. Crossing the hold threshold cancels the pending decision and
 *    the momentary audition owns the gesture instead.
 */
export class GestureEngine {
  private presses = new Map<Control, PressRecord>();
  private taps = new Map<Control, TapRecord>();
  /** Deferred Track decisions awaiting a timeout or a second press. */
  private pending = new Map<Control, { count: number; timer: ReturnType<typeof setTimeout>; firstReleaseAt: number }>();
  private gestureListeners = new Set<GestureListener>();
  private rawListeners = new Set<RawListener>();
  private chordActive: Control[] | null = null;
  private chordReleaseStartedAt: number | null = null;
  private chordReleased: Control[] = [];
  private chordTimer: ReturnType<typeof setTimeout> | null = null;
  private seq = 0;
  /** Measured latency, first release → emitted tap, per deferred control. */
  readonly decisionLatencyMs: number[] = [];

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
      powerHoldFired: false,
      longHoldFired: false,
      holdTimer: null,
      powerHoldTimer: null,
      longHoldTimer: null,
      moved: false,
    };

    if (!isContinuousControl(control)) {
      // Snapshot the preceding tap NOW: the multi-tap record self-expires after
      // multiTapGapMs, which is shorter than holdMs, so reading it inside the
      // hold timer always found null and "tap, then quick hold" never emitted.
      const prevTap = this.taps.get(control);
      const isTapThenHold =
        prevTap != null && prevTap.count > 0 && t - prevTap.lastReleaseAt <= this.timings.tapThenHoldGapMs;

      // Deferred second press: claim the double-tap now, stop the timeout that
      // would otherwise have confirmed a single tap. Nothing is emitted until
      // the second RELEASE, so an aborted second press cannot fake a double-tap.
      const claim = this.pending.get(control);
      if (claim) {
        clearTimeout(claim.timer);
        // count now tracks 1 → 2 → 3; the emit is still deferred to the
        // release that closes the sequence.
        this.pending.set(control, { ...claim, count: Math.min(3, claim.count + 1) });
      }

      rec.holdTimer = setTimeout(() => {
        rec.holdFired = true;
        // Crossing the hold threshold cancels any pending tap/double-tap: the
        // gesture becomes a momentary audition and emits nothing else.
        this.dropPending(control);
        if (isTapThenHold) {
          this.emit({ type: "tapThenHold", control, t: performance.now() });
          this.clearTaps(control);
        }

        this.emit({
          type: "holdStart",
          control,
          level: "hold",
          duration: this.timings.holdMs,
          t: performance.now(),
        });
      }, this.timings.holdMs);


      // Power gets its own threshold, only on FUNCTION (the v2.6 power row).
      if (control === "function") {
        rec.powerHoldTimer = setTimeout(() => {
          rec.powerHoldFired = true;
          this.emit({
            type: "holdStart",
            control,
            level: "power",
            duration: this.timings.powerHoldMs,
            t: performance.now(),
          });
        }, this.timings.powerHoldMs);
      }

      rec.longHoldTimer = setTimeout(() => {
        rec.longHoldFired = true;
        this.emit({
          type: "holdStart",
          control,
          level: "long",
          duration: this.timings.longHoldMs,
          t: performance.now(),
        });
      }, this.timings.longHoldMs);
    }

    this.presses.set(control, rec);
    this.emitRaw({ id: ++this.seq, control, phase: "down", pointerId, t, x, y });
    if (!isContinuousControl(control)) this.evaluateChordStart(t);

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

    if (rec.holdFired || rec.powerHoldFired) {
      const level: HoldLevel = rec.longHoldFired ? "long" : rec.powerHoldFired ? "power" : "hold";
      this.emit({ type: "holdEnd", control, level, duration, t });
      this.clearTaps(control);
      return;
    }


    // A fader that was actually dragged is a continuous edit, not a tap.
    if (isContinuousControl(control) && rec.moved) {
      this.clearTaps(control);
      return;
    }


    // ---- deferred Track arbitration -------------------------------------
    if (isDeferredControl(control)) {
      const claim = this.pending.get(control);
      const count = claim ? claim.count : 1;
      const firstReleaseAt = claim ? claim.firstReleaseAt : t;
      if (claim) clearTimeout(claim.timer);
      if (count >= 3) {
        // The third valid release confirms independent latched playback. One
        // gesture for the whole sequence: no ×1 and no ×2 was ever dispatched.
        this.pending.delete(control);
        this.decisionLatencyMs.push(t - firstReleaseAt);
        if (this.decisionLatencyMs.length > 50) this.decisionLatencyMs.shift();
        this.emit({ type: "tap", control, count: 3, t });
        return;
      }
      // ×1 waits to see whether a ×2 arrives; ×2 waits again for a ×3. Nothing
      // audible is committed until the window closes.
      const timer = setTimeout(() => {
        this.pending.delete(control);
        const at = performance.now();
        this.decisionLatencyMs.push(at - firstReleaseAt);
        if (this.decisionLatencyMs.length > 50) this.decisionLatencyMs.shift();
        this.emit({ type: "tap", control, count, t: at });
      }, this.timings.trackDecisionMs);
      this.pending.set(control, { count, timer, firstReleaseAt });
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
    this.dropPending(control);
    if (this.chordActive?.includes(control)) this.registerChordRelease(control, t);
    this.emitRaw({ id: ++this.seq, control, phase: "cancel", pointerId, t });
    this.emit({ type: "cancel", control, t });
  }

  /** Force-release everything. Used on blur / visibilitychange / unmount. */
  releaseAll(t = performance.now()) {
    for (const control of [...this.presses.keys()]) {
      this.cancel(control, this.presses.get(control)!.pointerId, t);
    }
    for (const control of [...this.pending.keys()]) this.dropPending(control);
  }

  /** Discard a pending Track decision without emitting anything. */
  private dropPending(control: Control) {
    const claim = this.pending.get(control);
    if (!claim) return;
    clearTimeout(claim.timer);
    this.pending.delete(control);
  }

  private clearTimers(rec: PressRecord) {
    if (rec.holdTimer) clearTimeout(rec.holdTimer);
    if (rec.powerHoldTimer) clearTimeout(rec.powerHoldTimer);
    if (rec.longHoldTimer) clearTimeout(rec.longHoldTimer);
  }


  private clearTaps(control: Control) {
    const prev = this.taps.get(control);
    if (prev?.timer) clearTimeout(prev.timer);
    this.taps.delete(control);
  }


  private evaluateChordStart(t: number) {
    if (this.chordActive) return;
    const members = [...this.presses.values()]
      .filter((p) => !isContinuousControl(p.control))
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
      return `hold start (${g.level}, ${g.duration}ms) · ${g.control}`;
    case "holdEnd":
      return `hold end (${g.level}, ${Math.round(g.duration)}ms) · ${g.control}`;

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
