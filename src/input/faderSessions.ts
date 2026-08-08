/**
 * Workstream 1 — independent per-pointer fader drag sessions.
 *
 * The previous build kept ONE `dragRef` singleton, so a second finger stole
 * ownership of the first fader and every `pointerup` ended "whatever drag was
 * current". This module replaces that with a pure, framework-free session map:
 * one session per pointerId, one owner per fader, and a single coalesced flush
 * that carries every pending fader on ONE animation frame / ONE audio frame.
 *
 * It holds no DOM and no AudioNode, so it is directly unit-testable.
 */

import type { ContinuousChannel } from "@/audio/controlBus";

export type FaderIndex = 0 | 1 | 2 | 3;

/** How a session was created — diagnostics must not conflate these. */
export type FaderInputSource = "touch" | "mouse" | "mouse-group" | "keyboard";

export interface FaderDragSession {
  pointerId: number;
  faderIndex: FaderIndex;
  /** viewBox Y at pointer-down. */
  startUserY: number;
  /** Fader value at pointer-down (the destination value, not the pointer's). */
  startValue: number;
  /**
   * value(pointer) - value(fader) at grab time. Pickup semantics: the cap never
   * jumps to the finger, and a rebase re-derives this offset instead of
   * snapping.
   */
  grabOffset: number;
  lastPreviewValue: number;
  channel: ContinuousChannel;
  source: FaderInputSource;
  /** Set once the pointer has travelled past the movement threshold. */
  moved: boolean;
  startedAt: number;
}

export interface PendingPreview {
  faderIndex: FaderIndex;
  value: number;
  channel: ContinuousChannel;
  pointerId: number;
}

/** viewBox units a pointer must travel before a press becomes a drag. */
export const MOVE_THRESHOLD_UNITS = 3;

export interface FlushBatch {
  /** Monotonic batch id — every preview in one rAF shares it. */
  batchFrame: number;
  previews: PendingPreview[];
}

export class FaderSessionManager {
  private pointerToDrag = new Map<number, FaderDragSession>();
  private faderToPointer = new Map<FaderIndex, number>();
  private pending = new Map<FaderIndex, PendingPreview>();
  private batchSeq = 0;

  /** Values the reducer/engine last committed, per fader. */
  readonly committed: number[] = [0, 0, 0, 0];

  get activePointerCount(): number {
    return this.pointerToDrag.size;
  }

  sessions(): FaderDragSession[] {
    return [...this.pointerToDrag.values()];
  }

  sessionForPointer(pointerId: number): FaderDragSession | undefined {
    return this.pointerToDrag.get(pointerId);
  }

  sessionForFader(index: FaderIndex): FaderDragSession | undefined {
    const p = this.faderToPointer.get(index);
    return p == null ? undefined : this.pointerToDrag.get(p);
  }

  owner(index: FaderIndex): number | null {
    return this.faderToPointer.get(index) ?? null;
  }

  /**
   * Claim a fader for a pointer. Returns null when the fader already has an
   * owner — the REJECTION MUST NOT touch the existing session in any way.
   */
  begin(args: {
    pointerId: number;
    faderIndex: FaderIndex;
    userY: number;
    pointerValue: number;
    currentValue: number;
    channel: ContinuousChannel;
    source: FaderInputSource;
    t: number;
  }): FaderDragSession | null {
    if (this.faderToPointer.has(args.faderIndex)) return null;
    if (this.pointerToDrag.has(args.pointerId)) return null;
    const session: FaderDragSession = {
      pointerId: args.pointerId,
      faderIndex: args.faderIndex,
      startUserY: args.userY,
      startValue: args.currentValue,
      grabOffset: args.pointerValue - args.currentValue,
      lastPreviewValue: args.currentValue,
      channel: args.channel,
      source: args.source,
      moved: false,
      startedAt: args.t,
    };
    this.pointerToDrag.set(args.pointerId, session);
    this.faderToPointer.set(args.faderIndex, args.pointerId);
    return session;
  }

  /**
   * Pointer movement. Returns the new value, or null when this pointer owns no
   * fader. Pickup semantics: value = pointerValue - grabOffset.
   */
  move(pointerId: number, userY: number, pointerValue: number, clamp: (v: number) => number): number | null {
    const s = this.pointerToDrag.get(pointerId);
    if (!s) return null;
    if (!s.moved && Math.abs(userY - s.startUserY) >= MOVE_THRESHOLD_UNITS) s.moved = true;
    if (!s.moved) return null;
    const value = clamp(pointerValue - s.grabOffset);
    s.lastPreviewValue = value;
    this.pending.set(s.faderIndex, {
      faderIndex: s.faderIndex,
      value,
      channel: s.channel,
      pointerId,
    });
    return value;
  }

  /**
   * True rebase (binding correction): the semantic target changed while the
   * finger is still down. The session keeps the pointer where it is and
   * re-derives grabOffset against the NEW channel's current destination value,
   * so nothing jumps and the old channel stops receiving previews.
   */
  rebase(pointerId: number, channel: ContinuousChannel, pointerValue: number, destinationValue: number): FaderDragSession | null {
    const s = this.pointerToDrag.get(pointerId);
    if (!s) return null;
    if (s.channel === channel) return s;
    s.channel = channel;
    s.grabOffset = pointerValue - destinationValue;
    s.startValue = destinationValue;
    s.lastPreviewValue = destinationValue;
    this.pending.delete(s.faderIndex);
    return s;
  }

  /** Commit exactly one session. Returns it so the caller can dispatch. */
  end(pointerId: number): FaderDragSession | null {
    const s = this.pointerToDrag.get(pointerId);
    if (!s) return null;
    this.pointerToDrag.delete(pointerId);
    this.faderToPointer.delete(s.faderIndex);
    this.pending.delete(s.faderIndex);
    this.committed[s.faderIndex] = s.lastPreviewValue;
    return s;
  }

  /** Cancel exactly one session. Never touches the other live faders. */
  cancel(pointerId: number): FaderDragSession | null {
    const s = this.pointerToDrag.get(pointerId);
    if (!s) return null;
    this.pointerToDrag.delete(pointerId);
    this.faderToPointer.delete(s.faderIndex);
    this.pending.delete(s.faderIndex);
    return s;
  }

  /** Queue a preview that did not come from a pointer move (keyboard/group). */
  queue(preview: PendingPreview) {
    this.pending.set(preview.faderIndex, preview);
  }

  /** Drain every pending fader onto ONE shared batch frame. */
  flush(): FlushBatch | null {
    if (this.pending.size === 0) return null;
    const previews = [...this.pending.values()];
    this.pending.clear();
    return { batchFrame: ++this.batchSeq, previews };
  }

  hasPending(): boolean {
    return this.pending.size > 0;
  }

  /** Diagnostics view: every live pointer and the fader it owns. */
  diagnostics() {
    return {
      activePointerCount: this.pointerToDrag.size,
      pointers: this.sessions().map((s) => ({
        pointerId: s.pointerId,
        fader: s.faderIndex + 1,
        channel: s.channel,
        source: s.source,
        moved: s.moved,
        pending: this.pending.get(s.faderIndex)?.value ?? null,
        committed: this.committed[s.faderIndex] ?? null,
        last: s.lastPreviewValue,
      })),
      lastBatchFrame: this.batchSeq,
    };
  }
}
