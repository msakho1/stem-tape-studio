/**
 * LED composition — persistent musical states must COMPOSE over the per-stem
 * activity base instead of erasing one another. Table-driven across the
 * combinations that have to stay readable simultaneously.
 */
import { describe, expect, it } from "vitest";
import {
  PLAY_SIDE_INDEX,
  applyModifiers,
  resolveSp1LedFrame,
  type AuthoritativeSp1LedState,
  type LedModifierKind,
} from "../sp1LedEngine";

const track = (patch: Partial<AuthoritativeSp1LedState["tracks"][number]> = {}) => ({
  loaded: true,
  muted: false,
  soloed: false,
  linked: true,
  pressed: false,
  reverse: false,
  looping: false,
  scratching: false,
  head: { loaded: true, muted: false, reverse: false, latched: false },
  ...patch,
});

function state(patch: Partial<AuthoritativeSp1LedState> = {}): AuthoritativeSp1LedState {
  return {
    power: "on",
    loading: false,
    error: null,
    playing: true,
    levels: [0.9, 0.9, 0.9, 0.9],
    tracks: [track(), track(), track(), track()],
    activeStem: 0,
    anySolo: false,
    fxOverlay: false,
    fxScope: "global",
    banks: [0, 1, 2, 3].map((i) => ({
      sideIndex: 4 + i,
      label: `BANK${i}`,
      algorithmId: "tilt",
      momentary: false,
      latched: false,
      rejected: null,
    })),
    globalLoop: { active: false, latched: false, division: 1 },
    loopPhase: null,
    slow: false,
    scratch: { master: false },
    scrub: { direction: 0, speedIndex: 0, latched: false, inertia: false },
    heads: { active: false },
    song: 0,
    bankJumpArmed: false,
    connectGreeting: null,
    flash: null,
    ...patch,
  };
}

const loopOn = { globalLoop: { active: true, latched: true, division: 1 as const } };
const fxOn = (s: AuthoritativeSp1LedState) => ({
  banks: s.banks.map((b, i) => (i === 0 ? { ...b, latched: true } : b)),
});

describe("composable LED modifiers", () => {
  it("keeps activity as the base layer during playback", () => {
    const f = resolveSp1LedFrame(state(), 0);
    expect(f.leds.slice(0, 4).every((l) => l.mode === "activity")).toBe(true);
  });

  const cases: Array<{ name: string; patch: (s: AuthoritativeSp1LedState) => Partial<AuthoritativeSp1LedState>; expect: LedModifierKind[] }> = [
    { name: "playback + mute", patch: (s) => ({ tracks: s.tracks.map((t, i) => (i === 0 ? { ...t, muted: true } : t)) }), expect: ["mute"] },
    {
      name: "playback + solo",
      patch: (s) => ({ anySolo: true, tracks: s.tracks.map((t, i) => (i === 0 ? { ...t, soloed: true } : t)) }),
      expect: ["solo"],
    },
    { name: "playback + loop", patch: () => loopOn, expect: ["loop"] },
    { name: "playback + reverse", patch: (s) => ({ tracks: s.tracks.map((t, i) => (i === 0 ? { ...t, reverse: true } : t)) }), expect: ["reverse"] },
    { name: "playback + FX", patch: (s) => fxOn(s), expect: ["fx"] },
    {
      name: "loop + reverse",
      patch: (s) => ({ ...loopOn, tracks: s.tracks.map((t, i) => (i === 0 ? { ...t, reverse: true } : t)) }),
      expect: ["loop", "reverse"],
    },
    { name: "loop + FX", patch: (s) => ({ ...loopOn, ...fxOn(s) }), expect: ["loop", "fx"] },
    {
      name: "reverse + FX",
      patch: (s) => ({ ...fxOn(s), tracks: s.tracks.map((t, i) => (i === 0 ? { ...t, reverse: true } : t)) }),
      expect: ["reverse", "fx"],
    },
    {
      name: "loop + reverse + FX",
      patch: (s) => ({ ...loopOn, ...fxOn(s), tracks: s.tracks.map((t, i) => (i === 0 ? { ...t, reverse: true } : t)) }),
      expect: ["loop", "reverse", "fx"],
    },
    { name: "slow + loop", patch: () => ({ ...loopOn, slow: true }), expect: ["loop", "slow"] },
    { name: "slow + FX", patch: (s) => ({ ...fxOn(s), slow: true }), expect: ["fx", "slow"] },
    {
      name: "solo + reverse",
      patch: (s) => ({ anySolo: true, tracks: s.tracks.map((t, i) => (i === 0 ? { ...t, soloed: true, reverse: true } : t)) }),
      expect: ["solo", "reverse"],
    },
    { name: "master scratch readiness", patch: () => ({ scratch: { master: true } }), expect: ["scratch"] },
    {
      name: "isolated stem scratch readiness",
      patch: (s) => ({ tracks: s.tracks.map((t, i) => (i === 0 ? { ...t, scratching: true } : t)) }),
      expect: ["scratch"],
    },
  ];

  it.each(cases)("$name — every state survives on Track 1", ({ patch, expect: kinds }) => {
    const s0 = state();
    const s = state(patch(s0));
    const led = resolveSp1LedFrame(s, 0).leds[0]!;
    // The base meter is never replaced …
    expect(led.mode).toBe("activity");
    // … and every persistent state is composed over it, none erased.
    for (const k of kinds) expect(led.modifiers).toContain(k);
    expect(led.owner).toContain("activity");
  });

  it("suppresses a muted stem even with hot source audio", () => {
    const s = state({ tracks: state().tracks.map((t, i) => (i === 0 ? { ...t, muted: true } : t)) });
    const f = resolveSp1LedFrame(s, 0);
    expect(f.leds[0]!.brightness).toBeLessThanOrEqual(18);
    expect(f.leds[1]!.brightness).toBeGreaterThan(60);
  });

  it("emphasizes a soloed stem while keeping it audio-reactive", () => {
    const base = { anySolo: true, tracks: state().tracks.map((t, i) => (i === 0 ? { ...t, soloed: true } : t)) };
    const loud = resolveSp1LedFrame(state({ ...base, levels: [0.9, 0.9, 0.9, 0.9] }), 0);
    const quiet = resolveSp1LedFrame(state({ ...base, levels: [0.05, 0.9, 0.9, 0.9] }), 0);
    expect(loud.leds[0]!.brightness).toBeGreaterThan(quiet.leds[0]!.brightness); // still reactive
    expect(loud.leds[0]!.mode).toBe("activity"); // not a static solid
    expect(loud.leds[0]!.brightness).toBeGreaterThan(loud.leds[1]!.brightness); // non-solo recedes
  });

  it("uses the real loop-wrap anchor when the engine offers one", () => {
    const s = state({ ...loopOn, loopPhase: 0.02 });
    const onTick = resolveSp1LedFrame(s, 0).leds[0]!;
    const offTick = resolveSp1LedFrame(state({ ...loopOn, loopPhase: 0.5 }), 0).leds[0]!;
    expect(onTick.phaseAnchor).toBe("loop-wrap");
    expect(onTick.brightness).toBeGreaterThan(offTick.brightness);
    // No real phase available → falls back to the app clock, never fabricated.
    expect(resolveSp1LedFrame(state(loopOn), 0).leds[0]!.phaseAnchor).not.toBe("loop-wrap");
  });

  it("keeps the loop visible on the PLAY-side LED while FX owns it", () => {
    const s0 = state();
    const s = state({ ...loopOn, fxOverlay: true, ...fxOn(s0), loopPhase: 0.01 });
    const led = resolveSp1LedFrame(s, 0).leds[PLAY_SIDE_INDEX]!;
    expect(led.precedenceKey).toBe("fxActive"); // FX owns the base …
    expect(led.modifiers).toContain("loop"); // … loop is not erased
  });

  it("marks a reversed lane's direction without hiding its meter", () => {
    const s = state({ tracks: state().tracks.map((t, i) => (i === 1 ? { ...t, reverse: true } : t)) });
    const f = resolveSp1LedFrame(s, 0);
    expect(f.leds[1]!.direction).toBe("reverse");
    expect(f.leds[0]!.direction).not.toBe("reverse"); // per-track only
    expect(f.leds[1]!.mode).toBe("activity");
  });

  it("STOP clears the activity base and every transient", () => {
    const f = resolveSp1LedFrame(state({ playing: false, levels: [0, 0, 0, 0], ...loopOn }), 0);
    expect(f.leds.slice(0, 4).every((l) => l.mode !== "activity")).toBe(true);
  });

  it("pitch confirmation is momentary and returns to the composite state", () => {
    const composite = state({ ...loopOn, slow: true });
    const during = resolveSp1LedFrame({ ...composite, flash: { kind: "pitch", startedAt: 0 } }, 10).leds[0]!;
    expect(during.mode).toBe("one-shot-single-flash");
    expect(during.modifiers).toEqual([]); // momentary override is never reshaped
    expect(during.restoreTo).toContain("activity");
    const after = resolveSp1LedFrame(composite, 400).leds[0]!;
    expect(after.mode).toBe("activity");
    expect(after.modifiers).toEqual(expect.arrayContaining(["loop", "slow"]));
  });

  it("applies modifiers in a bounded, order-stable way", () => {
    expect(applyModifiers(127, [{ kind: "mute" }], 0)).toBeLessThanOrEqual(18);
    expect(applyModifiers(0, [{ kind: "solo" }], 0)).toBeGreaterThan(0);
    expect(applyModifiers(127, [{ kind: "non-solo" }], 0)).toBeLessThan(127);
    expect(applyModifiers(200, [{ kind: "fx" }], 0)).toBeLessThanOrEqual(127);
    expect(applyModifiers(-5, [], 0)).toBe(-5); // no modifiers → untouched base
  });
});
