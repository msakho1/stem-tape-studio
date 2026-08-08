/**
 * Phase 5B worklet protocol — sequenced control messages over MessagePort.
 *
 * Control messages may cross the port. Audio blocks may NOT: PCM crosses once,
 * as an ownership transfer, and never again. Ports are closed on teardown so
 * their resources can be collected (Web Audio spec recommendation).
 */

export interface BufferMetadata {
  frames: number;
  channels: number;
  sampleRate: number;
  /** Source-native duration, for portable loop persistence cross-checks. */
  durationS: number;
}

export type WorkletMessage =
  | { type: "adopt"; seq: number; channels: ArrayBuffer[]; metadata: BufferMetadata }
  | { type: "prepareHandoff"; seq: number; sourceFrame: number }
  | { type: "start"; seq: number; applyAtContextFrame: number; sourceFrame: number }
  | { type: "stop"; seq: number; applyAtContextFrame: number }
  | { type: "restart"; seq: number; applyAtContextFrame: number }
  | { type: "setRate"; seq: number; rate: number; rampFrames: number; applyAtContextFrame: number }
  | { type: "setWindow"; seq: number; start: number; end: number; enabled: boolean; applyAtContextFrame: number }
  | { type: "setChop"; seq: number; division: number; index: number; applyAtContextFrame: number }
  | { type: "setLoopMode"; seq: number; mode: "fixed" | "variable"; applyAtContextFrame: number }
  | { type: "setDirection"; seq: number; direction: 1 | -1; applyAtContextFrame: number }
  | { type: "poll"; seq: number }
  | { type: "dispose"; seq: number }
  | { type: "__forceError"; seq: number; inFrames: number };

export interface WorkletAck {
  seq: number;
  status: "ready" | "applied" | "rejected" | "failed";
  appliedAtContextFrame?: number;
  resultingSourceFrame?: number;
  detail: string;
  trackId?: number;
  wrapCount?: number;
  renderGapFrames?: number;
  rate?: number;
  direction?: 1 | -1;
  playing?: boolean;
}

export const PROCESSOR_NAME = "tape-processor";
export const PROCESSOR_URL = "/tape-processor.js";

/** Lookahead used when scheduling a shared applyAtContextFrame, seconds. */
export const APPLY_LOOKAHEAD_S = 0.12;
/** Handoff acceptance: drift must not exceed two sample frames. */
export const DRIFT_TOLERANCE_FRAMES = 2;

export function sharedApplyFrame(ctx: BaseAudioContext, lookaheadS = APPLY_LOOKAHEAD_S): number {
  return Math.round((ctx.currentTime + lookaheadS) * ctx.sampleRate);
}

/** Pairwise drift (frames) between acknowledged per-track source frames. */
export function pairwiseDrift(sourceFrames: (number | null)[]): { pairs: string[]; maxDrift: number } {
  const pairs: string[] = [];
  let maxDrift = 0;
  for (let i = 0; i < sourceFrames.length; i++) {
    for (let j = i + 1; j < sourceFrames.length; j++) {
      const a = sourceFrames[i];
      const b = sourceFrames[j];
      if (a == null || b == null) continue;
      const d = Math.abs(a - b);
      maxDrift = Math.max(maxDrift, d);
      pairs.push(`T${i + 1}↔T${j + 1}: ${d} frames`);
    }
  }
  return { pairs, maxDrift };
}

/** Omit that distributes over the union (plain Omit collapses it). */
export type WorkletCommand = WorkletMessage extends infer M
  ? M extends { seq: number }
    ? Omit<M, "seq">
    : never
  : never;
