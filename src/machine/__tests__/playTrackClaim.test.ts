/**
 * PLAY first-claim ownership — PLAY + Track solo/link restored.
 *
 * The chords were never the problem: arbitration order was. Whoever reaches
 * PLAY first owns it, so both gestures stay reliable.
 *
 *   Track down  < 450 ms after PLAY → chord claims PLAY, global-loop hold
 *                                     cancelled (PLAY's holdStart is dropped).
 *   release     < 700 ms overlap    → stem.solo (latch)
 *   overlap reaches 700 ms held     → stem.link (release then inert)
 *   Track down >= 450 ms after PLAY → Hold PLAY already owns PLAY; the Track
 *                                     press is an ordinary Track gesture.
 *
 * A claimed chord emits no transport and no loop command.
 */

import { beforeEach, describe, expect, it, vi } from "vitest";
import { ChordArbiter, DEFAULT_ARBITER_TIMINGS, type PerfIntent } from "@/machine/chordArbiter";
import { DEFAULT_TIMINGS } from "@/input/gestures";

const CLAIM = DEFAULT_ARBITER_TIMINGS.globalLoopClaimMs; // 450
const LINK = DEFAULT_ARBITER_TIMINGS.soloLinkMs; // 700

function harness() {
  const intents: PerfIntent[] = [];
  const a = new ChordArbiter(() => ({ activeStem: 0, fxOverlay: false, selectedBank: null }));
  a.onIntent((i) => intents.push(i));
  return { a, intents };
}

beforeEach(() => {
  vi.useFakeTimers();
});

describe("first-claim ownership of PLAY", () => {
  it("boundary: the claim window is exactly the gesture engine's holdMs", () => {
    // One owner only: a Track arriving at 449 ms wins, at 450 ms it is too late.
    expect(CLAIM).toBe(DEFAULT_TIMINGS.holdMs);
  });

  it("Track at 449 ms claims PLAY and cancels the global-loop hold", () => {
    const { a } = harness();
    a.handle({ phase: "down", control: "play", t: 0, id: 33, pointerId: 1 });
    expect(a.isClaimed("play")).toBe(false);
    a.handle({ phase: "down", control: "track-button-1", t: CLAIM - 1, id: 82, pointerId: 1 });
    expect(a.isClaimed("play")).toBe(true); // PLAY's holdStart is dropped
    expect(a.isClaimed("track-button-1")).toBe(true);
  });

  it("Track at exactly 450 ms does NOT claim: the global loop owns PLAY", () => {
    const { a, intents } = harness();
    a.handle({ phase: "down", control: "play", t: 0, id: 33, pointerId: 1 });
    a.handle({ phase: "down", control: "track-button-1", t: CLAIM, id: 82, pointerId: 1 });
    expect(a.isClaimed("play")).toBe(false);
    expect(a.isClaimed("track-button-1")).toBe(false);
    a.handle({ phase: "up", control: "track-button-1", t: CLAIM + 100, id: 82, pointerId: 1 });
    expect(intents).toEqual([]); // ordinary Track gesture, no chord
  });

  it("Track well after the hold (2 s) still behaves normally", () => {
    const { a, intents } = harness();
    a.handle({ phase: "down", control: "play", t: 0, id: 33, pointerId: 1 });
    a.handle({ phase: "down", control: "track-button-3", t: 2000, id: 36, pointerId: 1 });
    a.handle({ phase: "up", control: "track-button-3", t: 2100, id: 36, pointerId: 1 });
    expect(a.isClaimed("track-button-3")).toBe(false);
    expect(intents).toEqual([]);
  });
});

describe("solo / link boundary at 700 ms", () => {
  it("release at 699 ms overlap latches solo", () => {
    const { a, intents } = harness();
    a.handle({ phase: "down", control: "play", t: 0, id: 33, pointerId: 1 });
    a.handle({ phase: "down", control: "track-button-2", t: 100, id: 75, pointerId: 1 });
    vi.advanceTimersByTime(LINK - 1);
    a.handle({ phase: "up", control: "track-button-2", t: 100 + LINK - 1, id: 75, pointerId: 1 });
    expect(intents).toHaveLength(1);
    expect(intents[0]).toMatchObject({ type: "stem.solo", stem: 1 });
    expect((intents[0] as { overlapMs: number }).overlapMs).toBeCloseTo(LINK - 1, 6);
    vi.advanceTimersByTime(2000);
    expect(intents).toHaveLength(1); // the link timer was cancelled
  });

  it("reaching 700 ms while held fires link, and the later release is inert", () => {
    const { a, intents } = harness();
    a.handle({ phase: "down", control: "play", t: 0, id: 33, pointerId: 1 });
    a.handle({ phase: "down", control: "track-button-4", t: 100, id: 93, pointerId: 1 });
    vi.advanceTimersByTime(LINK);
    expect(intents).toHaveLength(1);
    expect(intents[0]).toMatchObject({ type: "stem.link", stem: 3, overlapMs: LINK });
    a.handle({ phase: "up", control: "track-button-4", t: 100 + LINK + 300, id: 93, pointerId: 1 });
    expect(intents).toHaveLength(1); // no second solo on release
  });

  it("PLAY released first before 700 ms still latches solo at the real overlap", () => {
    const { a, intents } = harness();
    a.handle({ phase: "down", control: "play", t: 0, id: 33, pointerId: 1 });
    a.handle({ phase: "down", control: "track-button-1", t: 200, id: 82, pointerId: 1 });
    vi.advanceTimersByTime(300);
    a.handle({ phase: "up", control: "play", t: 500, id: 33, pointerId: 1 });
    expect(intents).toEqual([{ type: "stem.solo", stem: 0, overlapMs: 300 }]);
    vi.advanceTimersByTime(2000);
    expect(intents).toHaveLength(1);
  });

  it("a lost pointer cancels the pending chord without emitting anything", () => {
    const { a, intents } = harness();
    a.handle({ phase: "down", control: "play", t: 0, id: 33, pointerId: 1 });
    a.handle({ phase: "down", control: "track-button-1", t: 100, id: 82, pointerId: 1 });
    a.handle({ phase: "cancel", control: "track-button-1", t: 300, id: 82, pointerId: 1 });
    vi.advanceTimersByTime(2000);
    expect(intents).toEqual([]);
  });

  it("both stems can be soloed in sequence — each Track arms its own decision", () => {
    const { a, intents } = harness();
    a.handle({ phase: "down", control: "play", t: 0, id: 33, pointerId: 1 });
    a.handle({ phase: "down", control: "track-button-1", t: 50, id: 82, pointerId: 1 });
    a.handle({ phase: "down", control: "track-button-2", t: 80, id: 75, pointerId: 1 });
    vi.advanceTimersByTime(200);
    a.handle({ phase: "up", control: "track-button-1", t: 250, id: 82, pointerId: 1 });
    a.handle({ phase: "up", control: "track-button-2", t: 280, id: 75, pointerId: 1 });
    expect(intents.map((i) => i.type)).toEqual(["stem.solo", "stem.solo"]);
    expect(intents.map((i) => (i as { stem: number }).stem)).toEqual([0, 1]);
  });
});
