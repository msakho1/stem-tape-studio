import { describe, expect, it } from "vitest";
import { initialSurfaceState, pressControl, applyGesture, releaseControl, type SurfaceState } from "@/machine/surface";
import type { Control } from "@/device/geometry";

const last = (s: SurfaceState, type: string) => [...s.commands].reverse().find((c) => c.type === type);

function press(s: SurfaceState, ...controls: Control[]) {
  let next = s;
  for (const c of controls) next = pressControl(next, c);
  if (controls.length >= 2) next = applyGesture(next, { type: "chordStart", controls, t: 0 });
  return next;
}

describe("Track-button chord audition", () => {
  it("auditions two lanes together on simultaneous press", () => {
    let s = press(initialSurfaceState(), "track-button-1", "track-button-3");
    expect(last(s, "lane.audition")?.payload["mask"]).toBe("1010");
    expect(s.auditionChord).toEqual([0, 2]);
  });

  it("auditions three lanes together", () => {
    const s = press(initialSurfaceState(), "track-button-1", "track-button-2", "track-button-4");
    expect(last(s, "lane.audition")?.payload["mask"]).toBe("1101");
  });

  it("does not mute lanes when the chord releases", () => {
    let s = press(initialSurfaceState(), "track-button-1", "track-button-2");
    for (const c of ["track-button-1", "track-button-2"] as Control[]) s = releaseControl(s, c);
    s = applyGesture(s, { type: "chordRelease", controls: ["track-button-1", "track-button-2"], releaseSpreadMs: 20, t: 10 });
    expect(last(s, "lane.audition")?.payload["mask"]).toBe("");
    // the deferred taps that follow are consumed by the chord, not muted
    s = applyGesture(s, { type: "tap", control: "track-button-1", count: 1, t: 20 });
    s = applyGesture(s, { type: "tap", control: "track-button-2", count: 1, t: 21 });
    expect(s.tracks[0]!.content).toBe("loaded");
    expect(s.tracks[1]!.content).toBe("loaded");
    expect(s.auditionChord).toEqual([]);
  });

  it("narrows the chord when one member lifts", () => {
    let s = press(initialSurfaceState(), "track-button-1", "track-button-2");
    s = releaseControl(s, "track-button-2");
    s = applyGesture(s, { type: "holdEnd", control: "track-button-2", level: "hold", duration: 600, t: 30 });
    expect(last(s, "lane.audition")?.payload["mask"]).toBe("1000");
  });

  it("leaves a single Track press as a normal mute", () => {
    let s = pressControl(initialSurfaceState(), "track-button-1");
    s = releaseControl(s, "track-button-1");
    s = applyGesture(s, { type: "tap", control: "track-button-1", count: 1, t: 5 });
    expect(s.tracks[0]!.content).toBe("muted");
  });
});
