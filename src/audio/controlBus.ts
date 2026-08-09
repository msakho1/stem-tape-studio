/**
 * Continuous control bus.
 *
 * The fader's visual path already writes the SVG cap directly in a rAF and only
 * dispatches `faderCommit` on pointer-up. Audio must NOT wait for pointer-up, so
 * previews travel on this bus instead:
 *
 *   pointer drag -> rAF-coalesced preview -> control bus -> GainNode AudioParam
 *   pointer up   -> faderCommit -> reducer -> persistence
 *
 * The bus carries continuous values only. It never mutates reducer state, and
 * SVG components never see an AudioNode — they call the bus.
 */

export type ContinuousChannel = "fader" | "window" | "laneScrub" | "headScrub" | "headLevel";

/** Gesture lifecycle. `move` events are rAF-coalesced; the rest are discrete. */
export type ContinuousPhase = "start" | "move" | "end" | "cancel";

export interface ContinuousEvent {
  channel: ContinuousChannel;
  index: number;
  value: number;
  /** false while dragging, true for the committed value on pointer-up. */
  committed: boolean;
  /** Present for every pointer-driven control; identifies the gesture. */
  pointerId?: number;
  phase?: ContinuousPhase;
  /** performance.now() of the originating pointer event. */
  timestamp?: number;
  /**
   * Shared id for every fader flushed in the same animation frame. The engine
   * schedules one batch onto ONE future audio frame, so simultaneous fingers
   * never stagger across frames.
   */
  batchFrame?: number;
}



type Handler = (e: ContinuousEvent) => void;

class ControlBus {
  private handlers = new Set<Handler>();
  /** Last value the engine actually heard, per channel+index. */
  readonly lastPreview = new Map<string, number>();
  /** Last value the reducer committed, per channel+index. */
  readonly lastCommitted = new Map<string, number>();

  subscribe(fn: Handler) {
    this.handlers.add(fn);
    return () => this.handlers.delete(fn);
  }

  send(e: ContinuousEvent) {
    const key = `${e.channel}:${e.index}`;
    this.lastPreview.set(key, e.value);
    if (e.committed) this.lastCommitted.set(key, e.value);
    for (const h of this.handlers) h(e);
  }

  /**
   * Documented pointer-cancel rule: reconcile audio back to the last COMMITTED
   * value (ramped, never stepped). A cancelled drag is not a commit.
   */
  reconcile(channel: ContinuousChannel, index: number, fallback: number) {
    const key = `${channel}:${index}`;
    const value = this.lastCommitted.get(key) ?? fallback;
    this.send({ channel, index, value, committed: true });
    return value;
  }
}

export const controlBus = new ControlBus();
