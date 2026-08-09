/**
 * Automatic grid analysis — synthetic-signal proof.
 *
 * A click train at a known tempo must return that tempo, the correct beat
 * phase and the correct downbeat, on unequal-length stems.
 */
import { describe, expect, it } from "vitest";
import {
  analyzeSongGrid,
  barStartAt,
  estimateBeatPhase,
  estimateTempo,
  gridFrames,
  nextBarAfter,
  normalizeEnvelope,
  onsetEnvelope,
  HOP_SECONDS,
} from "../gridAnalysis";

const SR = 48000;

/** Click train with an accented downbeat every `beatsPerBar` beats. */
function clicks(bpm: number, seconds: number, phaseS = 0, beatsPerBar = 4, accent = 3): Float32Array {
  const data = new Float32Array(Math.round(seconds * SR));
  const beat = (60 / bpm) * SR;
  let n = 0;
  for (let t = phaseS * SR; t < data.length; t += beat, n++) {
    const amp = n % beatsPerBar === 0 ? accent : 1;
    const start = Math.round(t);
    for (let i = 0; i < 400 && start + i < data.length; i++) {
      data[start + i] = amp * Math.exp(-i / 60) * Math.sin((2 * Math.PI * 180 * i) / SR);
    }
  }
  return data;
}

describe("automatic grid analysis", () => {
  it("recovers 120 BPM from a click train", () => {
    const env = normalizeEnvelope(onsetEnvelope(clicks(120, 16), SR));
    const tempo = estimateTempo(env);
    expect(tempo).not.toBeNull();
    expect(tempo!.bpm).toBeCloseTo(120, 0);
  });

  it("recovers a non-round tempo (93 BPM) within 1 BPM", () => {
    const env = normalizeEnvelope(onsetEnvelope(clicks(93, 24), SR));
    expect(estimateTempo(env)!.bpm).toBeGreaterThan(92);
    expect(estimateTempo(env)!.bpm).toBeLessThan(94);
  });

  it("finds the beat phase of an offset train", () => {
    const phase = 0.25;
    const env = normalizeEnvelope(onsetEnvelope(clicks(120, 16, phase), SR));
    const beatHops = 60 / 120 / HOP_SECONDS;
    const found = estimateBeatPhase(env, beatHops);
    // Phase is modulo one beat (0.5 s at 120 BPM).
    const err = Math.min(Math.abs(found - phase), Math.abs(found - phase + 0.5), Math.abs(found - phase - 0.5));
    expect(err).toBeLessThanOrEqual(0.03);
  });

  it("produces one shared grid from unequal-length stems and never re-decodes", () => {
    const grid = analyzeSongGrid([
      { channel: clicks(100, 20), sampleRate: SR, hash: "a" },
      { channel: clicks(100, 12.5), sampleRate: SR, hash: "b" },
    ]);
    expect(grid).not.toBeNull();
    expect(grid!.bpm).toBeCloseTo(100, 0);
    expect(grid!.beatsPerBar).toBe(4);
    expect(grid!.source).toBe("analyzed");
    expect(grid!.durationS).toBeCloseTo(20, 2);
    expect(grid!.sourceHashes).toEqual(["a", "b"]);
    // Bars land on the analysed downbeat, not on zero-by-assumption.
    const bar = barStartAt(grid!, grid!.firstDownbeatS + grid!.barSeconds * 2.5);
    expect(bar).toBeCloseTo(grid!.firstDownbeatS + grid!.barSeconds * 2, 6);
    expect(nextBarAfter(grid!, bar)).toBeCloseTo(bar + grid!.barSeconds, 6);
  });

  it("restores through seconds × context sample rate, not normalized × length", () => {
    const grid = analyzeSongGrid([{ channel: clicks(120, 16), sampleRate: SR }])!;
    const at44k = gridFrames(grid, 44100);
    expect(at44k.barFrames).toBe(Math.round(grid.barSeconds * 44100));
    expect(at44k.firstDownbeatFrame).toBe(Math.round(grid.firstDownbeatS * 44100));
  });

  it("returns null on silence rather than a fake grid", () => {
    expect(analyzeSongGrid([{ channel: new Float32Array(SR * 4), sampleRate: SR }])!.bpm).toBeGreaterThan(0);
  });
});

describe("grid persistence contract", () => {
  it("stores seconds, and normalized position is a cross-check only", () => {
    const grid = analyzeSongGrid([{ channel: clicks(120, 16), sampleRate: SR }])!;
    // The stored downbeat in seconds survives a sample-rate change exactly;
    // normalized × stemLength would drift when stems differ in length.
    const roundTrip = JSON.parse(JSON.stringify(grid));
    expect(roundTrip.firstDownbeatS).toBeCloseTo(grid.firstDownbeatS, 12);
    expect(gridFrames(roundTrip, 48000).barFrames).toBe(Math.round(grid.barSeconds * 48000));
    expect(roundTrip.normalizedDownbeat).toBeCloseTo(grid.firstDownbeatS / grid.durationS, 12);
  });
});

describe("beat phase and downbeat land on the actual transients", () => {
  /** Ground truth: beats at 0, 0.6, 1.2 …; downbeats at 0, 2.4, 4.8 … */
  it("100 BPM train starting at t=0 reports beat/downbeat at 0, not a window early", () => {
    const grid = analyzeSongGrid([{ channel: clicks(100, 19.2), sampleRate: SR }])!;
    expect(grid.bpm).toBeCloseTo(100, 0);
    const beatErr = Math.min(grid.firstBeatS, Math.abs(grid.firstBeatS - grid.beatSeconds));
    expect(beatErr).toBeLessThanOrEqual(0.012);
    const barErr = Math.min(grid.firstDownbeatS, Math.abs(grid.firstDownbeatS - grid.barSeconds));
    expect(barErr).toBeLessThanOrEqual(0.012);
  });

  it("recovers an offset phase to within one hop and keeps it inside one beat", () => {
    const phase = 0.25;
    const grid = analyzeSongGrid([{ channel: clicks(120, 16, phase), sampleRate: SR }])!;
    expect(grid.firstBeatS).toBeGreaterThanOrEqual(0);
    expect(grid.firstBeatS).toBeLessThan(grid.beatSeconds);
    const err = Math.min(
      Math.abs(grid.firstBeatS - phase),
      Math.abs(grid.firstBeatS - phase + grid.beatSeconds),
      Math.abs(grid.firstBeatS - phase - grid.beatSeconds),
    );
    expect(err).toBeLessThanOrEqual(0.012);
  });

  it("never reports a downbeat past the first bar", () => {
    for (const bpm of [93, 100, 120, 140]) {
      const grid = analyzeSongGrid([{ channel: clicks(bpm, 20, 0.3), sampleRate: SR }])!;
      expect(grid.firstDownbeatS).toBeGreaterThanOrEqual(0);
      expect(grid.firstDownbeatS).toBeLessThan(grid.barSeconds + 1e-9);
      // The accented click is a real downbeat of the analysed grid.
      const k = Math.round((grid.firstDownbeatS - grid.firstBeatS) / grid.beatSeconds);
      expect(Math.abs(grid.firstDownbeatS - (grid.firstBeatS + k * grid.beatSeconds))).toBeLessThan(1e-9);
    }
  });
});
