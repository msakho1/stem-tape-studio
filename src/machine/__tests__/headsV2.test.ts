/**
 * Heads Mode v2 — four independent lane heads.
 *
 * Head N reads lane N on its own clock, so the heads sound while the main
 * transport is paused and no head movement can move the song playhead. These
 * tests pin the CONTROL contract at the reducer and gesture level:
 *
 *   tap                       → heads.mute (this head only)
 *   double-tap                → heads.loop.capture / release
 *   triple-tap                → heads.latch (independent playback)
 *   hold                      → heads.play.hold with the held mask
 *   FUNCTION + double-tap     → lane.reverse (universal lane layer → head)
 *   FUNCTION + Track + Vol ±  → heads.loop.resize on the held head
 *
 * and the deferred recognition that makes them possible: nothing is emitted
 * until the multi-tap window closes, so no ×1 mute is ever heard and undone.
 */

import { describe, expect, it, vi } from "vitest";
import { applyGesture, initialSurfaceState, pressControl, releaseControl, type SurfaceState } from "@/machine/surface";
import { GestureEngine, type Gesture } from "@/input/gestures";

let clock = 0;
const tap = (control: string, count = 1): Gesture =>
  ({ type: "tap", control, count, t: (clock += 1000) }) as Gesture;
const holdStart = (control: string): Gesture =>
  ({ type: "holdStart", control, level: "hold", duration: 500, t: (clock += 1000) }) as Gesture;

/** FUNCTION + triple-tap PLAY = heads on (v2.6 `play.heads`). */
function headsOn(): SurfaceState {
  let s = pressControl(initialSurfaceState(), "function");
  s = applyGesture(s, tap("play", 3));
  s = releaseControl(s, "function");
  return s;
}

const types = (s: SurfaceState, from = 0) => s.commands.slice(from).map((c) => c.type);

describe("Heads v2 · entry", () => {
  it("turns four independent heads on without touching the transport", () => {
    const s = headsOn();
    expect(s.headsMode).toBe(true);
    expect(s.playing).toBe(false);
    expect(types(s)).toContain("heads.enter");
    // No source assignment survives: head N is lane N by construction.
    expect(types(s)).not.toContain("heads.source");
    expect(s.tracks.every((t) => !t.headLatched && !t.headLoop.active && !t.headReverse)).toBe(true);
  });
});

describe("Heads v2 · Track button table", () => {
  it("×1 mutes only that head", () => {
    let s = headsOn();
    const n = s.commands.length;
    s = applyGesture(s, tap("track-button-2"));
    expect(s.tracks[1]!.headMuted).toBe(true);
    expect(s.tracks[0]!.headMuted).toBe(false);
    const mute = s.commands.slice(n).find((c) => c.type === "heads.mute")!;
    expect(mute.payload).toMatchObject({ head: 1, muted: true });
    expect(types(s, n)).not.toContain("track.mute");
  });

  it("×3 latches independent playback and never toggles the transport", () => {
    let s = headsOn();
    const n = s.commands.length;
    s = applyGesture(s, tap("track-button-3", 3));
    expect(s.tracks[2]!.headLatched).toBe(true);
    expect(s.playing).toBe(false);
    const latch = s.commands.slice(n).find((c) => c.type === "heads.latch")!;
    expect(latch.payload).toMatchObject({ head: 2, latched: true });
    expect(types(s, n)).not.toContain("heads.mute");
  });

  it("×2 captures the head's own loop and a following ×1 releases it", () => {
    let s = headsOn();
    let n = s.commands.length;
    s = applyGesture(s, tap("track-button-1", 2));
    expect(s.tracks[0]!.headLoop).toEqual({ active: true, bars: 1 });
    expect(s.commands.slice(n).find((c) => c.type === "heads.loop.capture")!.payload).toMatchObject({ head: 0, bars: 1 });

    n = s.commands.length;
    s = applyGesture(s, tap("track-button-1"));
    expect(s.tracks[0]!.headLoop.active).toBe(false);
    // A loop release must NOT mute the head as a side effect.
    expect(s.tracks[0]!.headMuted).toBe(false);
  });

  it("hold plays exactly the held heads (heads.play.hold), not the tape audition", () => {
    let s = headsOn();
    s = pressControl(s, "track-button-1");
    s = pressControl(s, "track-button-4");
    const n = s.commands.length;
    s = applyGesture(s, holdStart("track-button-1"));
    const hold = s.commands.slice(n).find((c) => c.type === "heads.play.hold")!;
    expect(hold.payload["mask"]).toBe("1001");
    expect(types(s, n)).not.toContain("lane.audition");
  });

  it("FUNCTION + Track held + Volume ± resizes that head's loop only", () => {
    let s = headsOn();
    s = pressControl(s, "function");
    s = pressControl(s, "track-button-3");
    const n = s.commands.length;
    s = applyGesture(s, tap("volume-plus"));
    expect(s.tracks[2]!.headLoop.bars).toBe(2);
    expect(s.tracks[0]!.headLoop.bars).toBe(1);
    const resize = s.commands.slice(n).find((c) => c.type === "heads.loop.resize")!;
    expect(resize.payload).toMatchObject({ head: 2, bars: 2 });
  });
});

describe("Heads v2 · deferred tap recognition", () => {
  it("emits ×3 once and never dispatches an intermediate ×1 or ×2", () => {
    vi.useFakeTimers();
    const seen: Gesture[] = [];
    const g = new GestureEngine();
    g.onGesture((ev) => seen.push(ev));
    let t = 0;
    for (let i = 0; i < 3; i++) {
      g.press("track-button-1", 1, (t += 40));
      g.release("track-button-1", 1, (t += 40));
      vi.advanceTimersByTime(80);
    }
    vi.advanceTimersByTime(400);
    const taps = seen.filter((s) => s.type === "tap");
    expect(taps).toHaveLength(1);
    expect(taps[0]).toMatchObject({ control: "track-button-1", count: 3 });
    vi.useRealTimers();
  });

  it("still resolves a lone tap to ×1 after the window closes", () => {
    vi.useFakeTimers();
    const seen: Gesture[] = [];
    const g = new GestureEngine();
    g.onGesture((ev) => seen.push(ev));
    g.press("track-button-2", 1, 0);
    g.release("track-button-2", 1, 40);
    vi.advanceTimersByTime(400);
    expect(seen.filter((s) => s.type === "tap")).toMatchObject([{ count: 1, control: "track-button-2" }]);
    vi.useRealTimers();
  });
});
