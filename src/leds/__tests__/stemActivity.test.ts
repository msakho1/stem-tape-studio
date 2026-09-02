import { describe, expect, it } from "vitest";
import {
  DECAY_MS,
  SILENCE_RMS,
  StemActivityEnvelopes,
  levelFromRms,
} from "../stemActivity";
import { resolveSp1LedFrame, sp1LedStateFrom } from "../sp1LedEngine";
import { initialSurfaceState, type SurfaceState } from "@/machine/surface";

function loadedPlaying(): SurfaceState {
  const base = initialSurfaceState();
  return {
    ...base,
    playing: true,
    tracks: base.tracks.map((t) => ({ ...t, content: "loaded" as const })) as SurfaceState["tracks"],
  };
}

describe("LED Stage 2 — stem activity envelopes", () => {
  it("maps silence to zero and loud material below full scale", () => {
    expect(levelFromRms(0)).toBe(0);
    expect(levelFromRms(SILENCE_RMS * 0.5)).toBe(0);
    const quiet = levelFromRms(0.01);
    const loud = levelFromRms(0.3);
    expect(quiet).toBeGreaterThan(0);
    expect(quiet).toBeLessThan(loud);
    expect(loud).toBeLessThanOrEqual(1);
    expect(levelFromRms(0.05)).toBeGreaterThan(levelFromRms(0.02));
  });

  it("attacks fast and decays over roughly the configured time constant", () => {
    const env = new StemActivityEnvelopes();
    env.sample([0.3, 0, 0, 0], 0, true);
    const attacked = env.sample([0.3, 0, 0, 0], 16, true)[0]!;
    expect(attacked).toBeGreaterThan(0.8);

    let t = 16;
    for (let i = 0; i < 16; i++) {
      t += DECAY_MS / 16;
      env.sample([0, 0, 0, 0], t, true);
    }
    // One full time constant later: ~1/e of the peak.
    const decayed = env.values[0]!;
    expect(decayed).toBeLessThan(attacked * 0.5);
    expect(decayed).toBeGreaterThan(0);
  });

  it("keeps the four envelopes independent", () => {
    const env = new StemActivityEnvelopes();
    env.sample([0.3, 0, 0.2, 0], 0, true);
    const v = env.sample([0.3, 0, 0.2, 0], 16, true);
    expect(v[0]).toBeGreaterThan(0.5);
    expect(v[1]).toBe(0);
    expect(v[2]).toBeGreaterThan(0.4);
    expect(v[3]).toBe(0);
  });

  it("collapses to silence when the transport is stopped — no frozen RMS", () => {
    const env = new StemActivityEnvelopes();
    env.sample([0.3, 0.3, 0.3, 0.3], 0, true);
    env.sample([0.3, 0.3, 0.3, 0.3], 16, true);
    const stopped = env.sample([0.3, 0.3, 0.3, 0.3], 32, false);
    expect([...stopped]).toEqual([0, 0, 0, 0]);
  });

  it("drives the track LEDs from their own levels while playing", () => {
    const state = loadedPlaying();
    const frame = resolveSp1LedFrame(sp1LedStateFrom(state, 0, [0, 1, 0.5, 0]), 0);
    const track = frame.leds.slice(0, 4);
    // Track 1 is the active stem (pre-existing precedence, Stage 3+ work);
    // every other loaded stem now rides its own audio activity.
    expect(track.slice(1).map((l) => l.mode)).toEqual(["activity", "activity", "activity"]);
    expect(track[1]!.brightness).toBeGreaterThan(track[2]!.brightness);
    expect(track[2]!.brightness).toBeGreaterThan(track[3]!.brightness);
    // No free-running 2400 ms breathe on the base playback layer.
    expect(track.slice(1).every((l) => l.periodMs === null)).toBe(true);
  });

  it("keeps the semantic signature stable while levels move", () => {
    const state = loadedPlaying();
    const a = resolveSp1LedFrame(sp1LedStateFrom(state, 0, [0.1, 0.2, 0.3, 0.4]), 0);
    const b = resolveSp1LedFrame(sp1LedStateFrom(state, 16, [0.9, 0.1, 0.7, 0.2]), 16);
    expect(b.signature).toBe(a.signature);
    expect(b.values).not.toEqual(a.values);
  });

  it("returns loaded stems to the stopped state when the transport stops", () => {
    const state = { ...loadedPlaying(), playing: false };
    const frame = resolveSp1LedFrame(sp1LedStateFrom(state, 0, [0, 0, 0, 0]), 0);
    expect(frame.leds.slice(1, 4).map((l) => l.mode)).toEqual(["dim", "dim", "dim"]);
  });
});
