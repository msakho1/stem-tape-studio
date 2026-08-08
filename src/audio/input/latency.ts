/**
 * Input-latency compensation (plan §L 6B).
 *
 * Three tiers, in order of honesty:
 *   1. reported estimate  — baseLatency + outputLatency + track latency
 *   2. manual offset      — the musician nudges it in ms
 *   3. loopback measurement — a real round-trip impulse test (optional)
 *
 * The reported estimate is labelled an ESTIMATE everywhere; it is never
 * presented as a measurement.
 */

export interface LatencyModel {
  baseLatencyS: number;
  outputLatencyS: number;
  inputLatencyS: number;
  manualOffsetMs: number;
  measuredRoundTripMs: number | null;
  source: "estimate" | "manual" | "measured";
}

export function estimateLatency(ctx: AudioContext, streamLatencyS: number | null): LatencyModel {
  return {
    baseLatencyS: ctx.baseLatency ?? 0,
    outputLatencyS: (ctx as AudioContext & { outputLatency?: number }).outputLatency ?? 0,
    inputLatencyS: streamLatencyS ?? 0,
    manualOffsetMs: 0,
    measuredRoundTripMs: null,
    source: "estimate",
  };
}

export function totalCompensationMs(m: LatencyModel): number {
  if (m.source === "measured" && m.measuredRoundTripMs != null) return m.measuredRoundTripMs + m.manualOffsetMs;
  return (m.baseLatencyS + m.outputLatencyS + m.inputLatencyS) * 1000 + m.manualOffsetMs;
}

export function compensationFrames(m: LatencyModel, sampleRate: number): number {
  return Math.round((totalCompensationMs(m) / 1000) * sampleRate);
}

export function latencyStatement(m: LatencyModel, sampleRate: number): string {
  const ms = totalCompensationMs(m);
  const frames = compensationFrames(m, sampleRate);
  const label =
    m.source === "measured"
      ? "measured loopback round trip"
      : m.source === "manual"
        ? "reported estimate plus your manual offset"
        : "reported estimate (baseLatency + outputLatency + track latency) — an estimate, not a measurement";
  return `${ms.toFixed(1)} ms / ${frames} frames — ${label}`;
}

/**
 * Loopback measurement: find the impulse in the captured signal and return the
 * round trip in ms. Pure so it is unit-testable against synthetic capture.
 */
export function findImpulseDelayFrames(captured: Float32Array, emitFrame: number, threshold = 0.2): number | null {
  for (let i = Math.max(0, emitFrame); i < captured.length; i++) {
    if (Math.abs(captured[i]!) >= threshold) return i - emitFrame;
  }
  return null;
}
