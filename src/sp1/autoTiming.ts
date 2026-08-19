/**
 * Automatic tempo and downbeat for the uploader.
 *
 * Local, deterministic, non-AI. It reuses the instrument's shared grid
 * analyser (src/audio/gridAnalysis.ts): onset envelope -> autocorrelation with
 * explicit octave arbitration into the documented 70-180 BPM musical band ->
 * comb-search beat phase -> bar-position downbeat.
 *
 * Preference order, per the product rule:
 *   1. the Drums stem alone, when it carries usable onset energy
 *   2. otherwise the combined transient envelope of every loaded stem
 *
 * Confidence is a deterministic agreement test between those two estimates; it
 * is advisory only and NEVER blocks preparation or upload.
 */

import { analyzeSongGrid, MAX_BPM, MIN_BPM, type SongGrid } from "@/audio/gridAnalysis";
import type { StemSlotName } from "./prepare";

export interface TimingSource {
  name: StemSlotName;
  /** One decoded channel view. Never copied. */
  channel: Float32Array;
  sampleRate: number;
}

export type TimingConfidence = "high" | "low";
export type TimingOrigin = "drums" | "combined" | "fallback";

export interface SongTiming {
  bpm: number;
  /** Seconds of the first meaningful downbeat / onset. */
  downbeatSeconds: number;
  confidence: TimingConfidence;
  origin: TimingOrigin;
  /** True when the value came from user editing rather than analysis. */
  edited: boolean;
}

/** Documented musical band the octave arbitration folds tempo into. */
export const TEMPO_RANGE = { min: MIN_BPM, max: MAX_BPM };

/** RMS gate under which a drum stem is considered unusable for analysis. */
export const DRUMS_ENERGY_FLOOR = 1e-4;

function rms(channel: Float32Array): number {
  if (channel.length === 0) return 0;
  const step = Math.max(1, Math.floor(channel.length / 200000));
  let sum = 0;
  let n = 0;
  for (let i = 0; i < channel.length; i += step) {
    const v = channel[i]!;
    sum += v * v;
    n++;
  }
  return n ? Math.sqrt(sum / n) : 0;
}

function gridOf(sources: TimingSource[]): SongGrid | null {
  return analyzeSongGrid(sources.map((s) => ({ channel: s.channel, sampleRate: s.sampleRate })));
}

/**
 * Analyse timing for a prepared song. Returns a usable value in every case:
 * when no tempo can be estimated at all it falls back to the first detected
 * onset with a nominal tempo, and reports low confidence.
 */
export function analyzeTiming(sources: TimingSource[]): SongTiming {
  const usable = sources.filter((s) => s.channel.length > 0 && s.sampleRate > 0);
  if (usable.length === 0) {
    return { bpm: 120, downbeatSeconds: 0, confidence: "low", origin: "fallback", edited: false };
  }

  const drums = usable.find((s) => s.name === "drums" && rms(s.channel) > DRUMS_ENERGY_FLOOR);
  const combinedGrid = gridOf(usable);
  const drumsGrid = drums ? gridOf([drums]) : null;
  const chosen = drumsGrid ?? combinedGrid;

  if (!chosen) {
    return {
      bpm: 120,
      downbeatSeconds: firstOnsetSeconds(usable),
      confidence: "low",
      origin: "fallback",
      edited: false,
    };
  }

  const reference = drumsGrid ? combinedGrid : null;
  const agrees =
    !!reference && Math.abs(reference.bpm - chosen.bpm) / chosen.bpm <= 0.02;

  // A stronger downbeat could not be established when the two estimates
  // disagree; default safely to the first detected musical onset.
  const downbeat =
    Number.isFinite(chosen.firstDownbeatS) && chosen.firstDownbeatS >= 0
      ? chosen.firstDownbeatS
      : firstOnsetSeconds(usable);

  return {
    bpm: Math.round(chosen.bpm * 100) / 100,
    downbeatSeconds: Math.round(downbeat * 1000) / 1000,
    confidence: drumsGrid ? (agrees ? "high" : "low") : "low",
    origin: drumsGrid ? "drums" : "combined",
    edited: false,
  };
}

/** First sample whose magnitude crosses a fixed floor, in seconds. */
export function firstOnsetSeconds(sources: TimingSource[], floor = 0.01): number {
  let best = Infinity;
  for (const s of sources) {
    for (let i = 0; i < s.channel.length; i++) {
      if (Math.abs(s.channel[i]!) >= floor) {
        best = Math.min(best, i / s.sampleRate);
        break;
      }
    }
  }
  return Number.isFinite(best) ? Math.round(best * 1000) / 1000 : 0;
}

/** Compact interface copy. Never uses the phrase "beat zero". */
export function timingLabel(t: SongTiming | null): string {
  if (!t) return "Analysing tempo…";
  const bpm = Math.round(t.bpm);
  if (t.edited) return `${bpm} BPM`;
  return t.confidence === "high"
    ? `Detected: ${bpm} BPM`
    : `Tempo estimated at ${bpm} BPM — edit if needed`;
}
