/**
 * Correction 6 — one authoritative tape-timeline event stream.
 *
 * Every consumer that has to stay glued to the tape (the take mixer / SOS
 * overdub layers, diagnostics, future features) subscribes HERE instead of the
 * engine remembering to call `followRate()` from each transport branch. A new
 * transport feature that forgets to emit is visible in the event log; a feature
 * that emits is automatically followed by every consumer.
 *
 * Events are ordered and monotonically numbered, exactly like the command
 * stream, so a consumer can prove it saw the same sequence the engine emitted.
 */

export type TapeTimelineEvent =
  | { seq: number; t: number; type: "RateChange"; rate: number; musicalRate: number; rampFrames: number; cause: string }
  | { seq: number; t: number; type: "GlideChange"; from: number; to: number; tau: number; cause: string }
  | { seq: number; t: number; type: "LoopWrap"; track: number; positionS: number }
  | { seq: number; t: number; type: "WindowChange"; track: number; startS: number; lengthS: number }
  | { seq: number; t: number; type: "ChopChange"; track: number; div: number }
  | { seq: number; t: number; type: "LinkChange"; mask: string }
  | { seq: number; t: number; type: "DirectionChange"; reversed: boolean };

export type TapeTimelineEventType = TapeTimelineEvent["type"];

export const TIMELINE_EVENT_LOG_LIMIT = 200;

type Sub = (ev: TapeTimelineEvent) => void;

/** Distributive omit — a plain Omit over the union collapses the variants. */
export type TapeTimelineEventInput = TapeTimelineEvent extends infer E
  ? E extends TapeTimelineEvent
    ? Omit<E, "seq" | "t"> & { t?: number }
    : never
  : never;

export class TapeTimelineBus {
  private subs = new Set<Sub>();
  private seq = 0;
  /** Recent events, newest last — evidence for the SOS regression tests. */
  log: TapeTimelineEvent[] = [];

  subscribe(fn: Sub): () => void {
    this.subs.add(fn);
    return () => this.subs.delete(fn);
  }

  emit(ev: TapeTimelineEventInput): TapeTimelineEvent {
    const full = {
      ...ev,
      seq: ++this.seq,
      t: ev.t ?? (typeof performance !== "undefined" ? performance.now() : Date.now()),
    } as TapeTimelineEvent;
    this.log.push(full);
    if (this.log.length > TIMELINE_EVENT_LOG_LIMIT) this.log.shift();
    for (const s of this.subs) s(full);
    return full;
  }

  count(type: TapeTimelineEventType): number {
    return this.log.filter((e) => e.type === type).length;
  }
}
