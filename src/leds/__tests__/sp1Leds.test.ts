import { describe, it, expect } from "vitest";
import {
  PERIOD,
  PHYSICAL_LED_MAP,
  PLAY_SIDE_INDEX,
  PRECEDENCE,
  SCRUB_LED_PERIODS_MS,
  formatSp1Frame,
  isAnimatedMode,
  resolveSp1LedFrame,
  type AuthoritativeSp1LedState,
} from "../sp1LedEngine";

function baseState(patch: Partial<AuthoritativeSp1LedState> = {}): AuthoritativeSp1LedState {
  const track = (loaded = true) => ({
    loaded,
    muted: false,
    soloed: false,
    linked: true,
    pressed: false,
    head: { loaded, muted: false, reverse: false, latched: false },
  });
  return {
    power: "on",
    loading: false,
    error: null,
    playing: false,
    tracks: [track(), track(), track(), track()],
    activeStem: 0,
    anySolo: false,
    fxOverlay: false,
    fxScope: "stem",
    banks: [0, 1, 2, 3].map((i) => ({
      sideIndex: 4 + i,
      label: `B${i}`,
      algorithmId: i === 1 ? "gate" : "filter",
      momentary: false,
      latched: false,
      rejected: null,
    })),
    globalLoop: { active: false, latched: false, division: 1 },
    scrub: { direction: 0, speedIndex: 1, latched: false, inertia: false },
    heads: { active: false },
    song: 0,
    bankJumpArmed: false,
    flash: null,
    ...patch,
  };
}

describe("physical map", () => {
  it("is exactly eight indices in the documented order", () => {
    expect(PHYSICAL_LED_MAP.map((s) => s.id)).toEqual([
      "track-led-1",
      "track-led-2",
      "track-led-3",
      "track-led-4",
      "side-led-1",
      "side-led-2",
      "side-led-3",
      "side-led-4",
    ]);
    expect(PHYSICAL_LED_MAP[4]!.name).toContain("nearest PLAY");
    expect(PHYSICAL_LED_MAP[7]!.name).toContain("nearest FUNCTION");
  });

  it("always resolves eight LEDs and eight values", () => {
    const f = resolveSp1LedFrame(baseState(), 0);
    expect(f.leds).toHaveLength(8);
    expect(f.values).toHaveLength(8);
    expect(f.values.every((v) => v >= 0 && v <= 127)).toBe(true);
  });
});

describe("transport", () => {
  it("fully illuminates physical index 4 while a song plays", () => {
    const f = resolveSp1LedFrame(baseState({ playing: true }), 0);
    const s1 = f.leds[PLAY_SIDE_INDEX]!;
    expect(s1.mode).toBe("solid");
    expect(s1.brightness).toBe(127);
    expect(s1.provenance).toBe("stock-hardware-confirmed");
  });

  it("stops illuminating index 4 when playback stops", () => {
    const f = resolveSp1LedFrame(baseState({ playing: false, song: 1 }), 0);
    expect(f.leds[PLAY_SIDE_INDEX]!.owner).not.toContain("transport");
    expect(f.leds[PLAY_SIDE_INDEX]!.brightness).toBe(0);
  });
});

describe("scrub", () => {
  it("moves the head across the track row and reverses direction", () => {
    const fwd = baseState({ scrub: { direction: 1, speedIndex: 1, latched: false, inertia: false } });
    const p = SCRUB_LED_PERIODS_MS[1]!;
    const headAt = (t: number, st = fwd) =>
      resolveSp1LedFrame(st, t).values.slice(0, 4).indexOf(127);
    expect(headAt(0)).toBe(0);
    expect(headAt(p / 4)).toBe(1);
    expect(headAt(p / 2)).toBe(2);
    const rev = baseState({ scrub: { direction: -1, speedIndex: 1, latched: false, inertia: false } });
    expect(headAt(0, rev)).toBe(3);
    expect(headAt(p / 4, rev)).toBe(2);
  });

  it("uses a distinct period for each of the four persistent speeds", () => {
    const periods = [0, 1, 2, 3].map(
      (i) =>
        resolveSp1LedFrame(
          baseState({ scrub: { direction: 1, speedIndex: i as 0, latched: false, inertia: false } }),
          0,
        ).leds[0]!.periodMs,
    );
    expect(periods).toEqual([...SCRUB_LED_PERIODS_MS]);
    expect(new Set(periods).size).toBe(4);
  });

  it("keeps a latched shuttle lit with a non-zero floor after release", () => {
    const st = baseState({ scrub: { direction: 0, speedIndex: 2, latched: true, inertia: false } });
    const f = resolveSp1LedFrame(st, 0);
    expect(f.leds[0]!.owner).toContain("latched");
    expect(f.leds[0]!.floor).toBeGreaterThan(0);
  });
});

describe("global loop", () => {
  it("blinks index 4 at the division rate and marks latched with a floor", () => {
    const mom = resolveSp1LedFrame(
      baseState({ globalLoop: { active: true, latched: false, division: 4 } }),
      0,
    ).leds[PLAY_SIDE_INDEX]!;
    expect(mom.mode).toBe("blink");
    expect(mom.periodMs).toBe(200);
    expect(mom.floor).toBe(0);

    const lat = resolveSp1LedFrame(
      baseState({ globalLoop: { active: false, latched: true, division: 4 } }),
      0,
    ).leds[PLAY_SIDE_INDEX]!;
    expect(lat.owner).toContain("latched");
    expect(lat.floor).toBeGreaterThan(0);
  });

  it("shows the division on the remaining side LEDs", () => {
    const f = resolveSp1LedFrame(baseState({ globalLoop: { active: true, latched: false, division: 8 } }), 0);
    expect(f.leds[5]!.owner).toContain("division");
    expect(f.leds[7]!.owner).toContain("division");
  });
});

describe("fx", () => {
  it("keeps the rhythmic gate rapid pulse and still distinguishes latched", () => {
    const st = baseState({ fxOverlay: true });
    st.banks[1] = { ...st.banks[1]!, momentary: true };
    const momentary = resolveSp1LedFrame(st, 0).leds[5]!;
    expect(momentary.mode).toBe("rapid-pulse");
    expect(momentary.periodMs).toBe(PERIOD.rapidPulse);
    expect(momentary.floor).toBe(0);

    const st2 = baseState({ fxOverlay: true });
    st2.banks[1] = { ...st2.banks[1]!, latched: true };
    const latched = resolveSp1LedFrame(st2, 0).leds[5]!;
    expect(latched.mode).toBe("rapid-pulse");
    expect(latched.floor).toBeGreaterThan(0);
    expect(latched.owner).toContain("latched");
  });
});

describe("one-shots and precedence", () => {
  it("expires the FX confirmation flash on the application clock", () => {
    const st = baseState({ flash: { kind: "fx-latch", startedAt: 0 } });
    const during = resolveSp1LedFrame(st, 10).leds[0]!;
    expect(during.mode).toBe("one-shot-single-flash");
    expect(during.brightness).toBe(127);
    expect(during.restoreTo).not.toBeNull();
    expect(resolveSp1LedFrame(st, PERIOD.oneShotSingle + 5).leds[0]!.brightness).toBe(0);
  });

  it("flashes twice for a refused heads entry and outranks the confirmation flash", () => {
    const st = baseState({ flash: { kind: "heads-reject", startedAt: 0 } });
    const slice = PERIOD.oneShotDouble / 4;
    const samples = [0, slice, 2 * slice, 3 * slice].map((t) => resolveSp1LedFrame(st, t + 1).values[0]);
    expect(samples).toEqual([127, 0, 127, 0]);
    expect(PRECEDENCE.rejectionFlash).toBeGreaterThan(PRECEDENCE.confirmationFlash);
  });

  it("records the loser and the restoration target for every temporary owner", () => {
    const st = baseState({
      playing: true,
      flash: { kind: "fx-latch", startedAt: 0 },
    });
    const led = resolveSp1LedFrame(st, 5).leds[0]!;
    expect(led.precedenceKey).toBe("confirmationFlash");
    expect(led.lostTo).toBeTruthy();
    expect(led.restoreTo).toBeTruthy();
  });

  it("powers every LED off when the surface is off, beating all other owners", () => {
    const f = resolveSp1LedFrame(baseState({ power: "off", playing: true, heads: { active: true } }), 0);
    expect(f.values).toEqual([0, 0, 0, 0, 0, 0, 0, 0]);
    expect(f.leds.every((l) => l.precedence === PRECEDENCE.power)).toBe(true);
  });
});

describe("signature", () => {
  it("is stable across animation sampling but changes with semantics", () => {
    const st = baseState({ playing: true });
    const a = resolveSp1LedFrame(st, 0);
    const b = resolveSp1LedFrame(st, 137);
    expect(b.signature).toBe(a.signature);
    expect(a.animated).toBe(true);
    const c = resolveSp1LedFrame(baseState({ playing: false }), 0);
    expect(c.signature).not.toBe(a.signature);
  });

  it("reports a non-animated frame when nothing needs a ticker", () => {
    const idle = resolveSp1LedFrame(baseState({ power: "off" }), 0);
    expect(idle.animated).toBe(false);
    expect(idle.leds.every((l) => !isAnimatedMode(l.mode))).toBe(true);
  });

  it("formats the eight-index frame", () => {
    expect(formatSp1Frame(resolveSp1LedFrame(baseState({ playing: true }), 0))).toMatch(
      /^\[T1 \d+, T2 \d+, T3 \d+, T4 \d+ \| S1 \d+, S2 \d+, S3 \d+, S4 \d+\] owner=/,
    );
  });
});
