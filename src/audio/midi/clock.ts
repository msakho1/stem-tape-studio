/**
 * performance.now() ↔ AudioContext calibration.
 *
 * One calibration pair `(perfNowMs0, ctxTime0)` is maintained and refreshed on
 * unlock, on visibility change, and on every native re-anchor. Cue frames are
 * later derived from `ctxTimeOf(ev)`, never from `engine.position()` at the
 * moment JavaScript happened to receive the event.
 *
 * Pure bookkeeping — this module never touches the engine or emits commands.
 */

import type { StemMidiEvent } from "./contract";

export type Calibration = { perfNowMs0: number; ctxTime0: number };

export class MidiClock {
  private cal: Calibration = { perfNowMs0: 0, ctxTime0: 0 };
  private anchored = false;

  /** Refresh the calibration pair. Both values must be sampled together. */
  anchor(perfNowMs: number, ctxTime: number): void {
    this.cal = { perfNowMs0: perfNowMs, ctxTime0: ctxTime };
    this.anchored = true;
  }

  isAnchored(): boolean {
    return this.anchored;
  }

  calibration(): Calibration {
    return { ...this.cal };
  }

  /** ctxTimeOf(ev) = ctxTime0 + (ev.timestampMs - perfNowMs0) / 1000 */
  ctxTimeOf(ev: Pick<StemMidiEvent, "timestampMs">): number {
    return this.cal.ctxTime0 + (ev.timestampMs - this.cal.perfNowMs0) / 1000;
  }

  /** How far in the past the event is, in seconds, relative to a context time. */
  latencyOf(ev: Pick<StemMidiEvent, "timestampMs">, ctxNow: number): number {
    return ctxNow - this.ctxTimeOf(ev);
  }
}

export const midiClock = new MidiClock();
