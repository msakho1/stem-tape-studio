/**
 * Take model and tape-coordinate math (plan §D, §D1).
 *
 * Captured PCM is stored REAL-TIME (never resampled on write). Playback
 * position is reconstructed from event-defined timeline segments so glides,
 * varispeed and loop passes are exact instead of sampled approximations.
 *
 * Pure math + serializable types only — no Web Audio, no DOM: this module is
 * the unit-tested source of truth for both the worklet and the exporter.
 */

export type TakeState =
  | "recording"
  | "finalizing"
  | "ready"
  | "interrupted"
  | "failed";

export type TakeRateSpec =
  | { kind: "constant"; value: number }
  | { kind: "linear"; r0: number; r1: number; spanFrames: number }
  | { kind: "exponential"; r0: number; r1: number; tau: number };

export interface TakeTimelineSegment {
  contextStartFrame: number;
  contextEndFrame: number;
  /** Frame inside the take PCM at contextStartFrame (real-time, 1:1). */
  takeStartFrame: number;
  /** Tape coordinate (frames) at contextStartFrame. */
  tapeStartFrame: number;
  direction: 1 | -1;
  rate: TakeRateSpec;
  loopIteration: number;
}

export interface TakeChunkRef {
  index: number;
  blobKey: string;
  /** Frame offset of this chunk inside the take. */
  startFrame: number;
  frames: number;
  bytes: number;
}

/** One pass sublayer: a single traversal of the loop while recording. */
export interface TakePass {
  passIndex: number;
  startFrame: number;
  frames: number;
  tapeStartFrame: number;
  segments: TakeTimelineSegment[];
}

export interface TakeManifest {
  id: string;
  projectId: string;
  trackId: number;
  createdAt: number;
  state: TakeState;
  sampleRate: number;
  channels: number;
  /** Frames actually committed to storage (the only playable extent). */
  frames: number;
  chunks: TakeChunkRef[];
  passes: TakePass[];
  /** Input latency compensation applied at playback, in frames. */
  latencyCompFrames: number;
  /** Non-null when the take stopped for a reason other than a clean stop. */
  failureReason: string | null;
  enabled: boolean;
  gain: number;
  provenance: "user-private";
  /** True once every chunk is durably written AND frames === sum(chunk frames). */
  durable: boolean;
  label: string;
}

export const TAKE_SCHEMA_VERSION = 1;

export function emptyManifest(init: Partial<TakeManifest> & { id: string; projectId: string; trackId: number; sampleRate: number; channels: number }): TakeManifest {
  return {
    createdAt: Date.now(),
    state: "recording",
    frames: 0,
    chunks: [],
    passes: [],
    latencyCompFrames: 0,
    failureReason: null,
    enabled: true,
    gain: 1,
    provenance: "user-private",
    durable: false,
    label: `take ${new Date().toLocaleTimeString()}`,
    ...init,
  };
}

// ---------------------------------------------------------------- rate math

/**
 * Tape distance (frames) travelled inside a segment after `x` context frames.
 * The exponential form is the SAME analytic integral used by `glide.ts`:
 *   r(t) = r1 + (r0 − r1)·e^(−t/τ)
 *   ∫₀ˣ r = r1·x + (r0 − r1)·τ·(1 − e^(−x/τ))
 */
export function segmentDistance(rate: TakeRateSpec, x: number, sampleRate: number): number {
  if (x <= 0) return 0;
  switch (rate.kind) {
    case "constant":
      return rate.value * x;
    case "linear": {
      // r(t) = r0 + (r1 − r0)·t/span  ⇒  ∫₀ˣ r = r0·x + (r1 − r0)·x²/(2·span)
      const span = rate.spanFrames > 0 ? rate.spanFrames : 1;
      const u = Math.min(x, span);
      const tail = x > span ? (x - span) * rate.r1 : 0;
      return rate.r0 * u + ((rate.r1 - rate.r0) * u * u) / (2 * span) + tail;
    }
    case "exponential": {
      const tauFrames = rate.tau * sampleRate;
      if (tauFrames <= 0) return rate.r1 * x;
      return rate.r1 * x + (rate.r0 - rate.r1) * tauFrames * (1 - Math.exp(-x / tauFrames));
    }
  }
}

export function rateAtSegment(rate: TakeRateSpec, x: number, sampleRate: number): number {
  switch (rate.kind) {
    case "constant":
      return rate.value;
    case "linear": {
      const span = rate.spanFrames > 0 ? rate.spanFrames : 1;
      const u = Math.max(0, Math.min(x, span));
      return rate.r0 + (rate.r1 - rate.r0) * (u / span);
    }
    case "exponential": {
      const tauFrames = rate.tau * sampleRate;
      if (tauFrames <= 0) return rate.r1;
      return rate.r1 + (rate.r0 - rate.r1) * Math.exp(-x / tauFrames);
    }
  }
}

/** Tape coordinate at a context frame, integrating the segment list exactly. */
export function tapeFrameAt(segments: TakeTimelineSegment[], contextFrame: number, sampleRate: number): number | null {
  for (const s of segments) {
    if (contextFrame < s.contextStartFrame) continue;
    if (contextFrame > s.contextEndFrame) continue;
    const x = contextFrame - s.contextStartFrame;
    return s.tapeStartFrame + s.direction * segmentDistance(s.rate, x, sampleRate);
  }
  return null;
}

/** Take (PCM) frame at a context frame — real-time capture, so 1:1 per segment. */
export function takeFrameAt(segments: TakeTimelineSegment[], contextFrame: number): number | null {
  for (const s of segments) {
    if (contextFrame < s.contextStartFrame || contextFrame > s.contextEndFrame) continue;
    return s.takeStartFrame + (contextFrame - s.contextStartFrame);
  }
  return null;
}

/**
 * Inverse mapping: which take frame sits at a tape coordinate, inside one pass.
 * Monotonic per segment, so bisection is exact to within `tolerance` frames.
 */
export function takeFrameForTape(
  segments: TakeTimelineSegment[],
  tapeFrame: number,
  sampleRate: number,
  tolerance = 1e-3,
): number | null {
  for (const s of segments) {
    const span = s.contextEndFrame - s.contextStartFrame;
    const endTape = s.tapeStartFrame + s.direction * segmentDistance(s.rate, span, sampleRate);
    const lo = Math.min(s.tapeStartFrame, endTape);
    const hi = Math.max(s.tapeStartFrame, endTape);
    if (tapeFrame < lo || tapeFrame > hi) continue;
    let a = 0;
    let b = span;
    for (let i = 0; i < 80 && b - a > tolerance; i++) {
      const mid = (a + b) / 2;
      const t = s.tapeStartFrame + s.direction * segmentDistance(s.rate, mid, sampleRate);
      const ahead = s.direction > 0 ? t < tapeFrame : t > tapeFrame;
      if (ahead) a = mid;
      else b = mid;
    }
    return s.takeStartFrame + (a + b) / 2;
  }
  return null;
}

/** Total tape distance covered by a segment list (frames). */
export function segmentsTapeLength(segments: TakeTimelineSegment[], sampleRate: number): number {
  let d = 0;
  for (const s of segments) d += segmentDistance(s.rate, s.contextEndFrame - s.contextStartFrame, sampleRate);
  return d;
}

/** A manifest is playable only when every frame it claims is durably stored. */
export function verifyDurability(m: TakeManifest): { ok: boolean; detail: string } {
  const sum = m.chunks.reduce((n, c) => n + c.frames, 0);
  if (m.chunks.length === 0) return { ok: false, detail: "no chunks committed" };
  for (let i = 0; i < m.chunks.length; i++) {
    const expected = i === 0 ? 0 : m.chunks[i - 1]!.startFrame + m.chunks[i - 1]!.frames;
    if (m.chunks[i]!.startFrame !== expected)
      return { ok: false, detail: `chunk ${i} starts at ${m.chunks[i]!.startFrame}, expected ${expected} (gap or overlap)` };
  }
  if (sum !== m.frames) return { ok: false, detail: `manifest claims ${m.frames} frames, chunks hold ${sum}` };
  return { ok: true, detail: `${m.chunks.length} contiguous chunks, ${sum} frames, no gaps or duplicates` };
}

export function takeBytes(m: TakeManifest): number {
  return m.frames * m.channels * 4;
}
