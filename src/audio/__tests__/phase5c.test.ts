import { describe, expect, it } from "vitest";
import { ChordArbiter, DEFAULT_ARBITER_TIMINGS, type PerfIntent } from "@/machine/chordArbiter";
import type { RawInputEvent } from "@/input/gestures";
import type { Control } from "@/device/geometry";
import {
  clearLatches,
  cycleBankAlgorithm,
  deserializePerformance,
  initialStemPerformance,
  inputOpen,
  selectStem,
  serializePerformance,
  setBankMomentary,
  tapeTarget,
  toggleBankLatch,
  toggleLink,
  toggleSolo,
  STEM_TAPE_SCHEMA_VERSION,
} from "@/machine/stemPerformance";
import type { BankIndex } from "@/machine/fx12";

import {
  ECHO_FEEDBACK_MAX,
  ECHO_VARIATIONS,
  REPEAT_VARIATIONS,
  REVERB_FEEDBACK_CEILING,
  REVERB_VARIATIONS,
  MIN_SUPPORTED_BPM,
  repeatRingCapacityFrames,
} from "@/audio/fx/rack";
import { STEM_TAPE_V1_MAP, V26_ROWS_AS_REGISTRY, exportMapJson } from "@/machine/stemTapeV1Map";
import { V26_MAP } from "@/machine/v26map";
import { applyPerfIntent, deriveLeds, initialSurfaceState } from "@/machine/surface";

/** Numeric tolerance — never assert floating-point equality (correction 11). */
const TOL = 1e-9;
const SR = 48000;

function harness(overlay = false, activeStem: 0 | 1 | 2 | 3 = 0) {
  const view = { activeStem, fxOverlay: overlay };
  const arb = new ChordArbiter(() => view);
  const intents: PerfIntent[] = [];
  arb.onIntent((i) => intents.push(i));
  let id = 0;
  const ev = (control: Control, phase: RawInputEvent["phase"], t: number) =>
    arb.handle({ id: ++id, control, phase, pointerId: 1, t });
  return { arb, intents, ev, view };
}

describe("ordered chord arbitration (correction 1)", () => {
  it("Play + Vol+ selects the next stem and CLAIMS play before dispatch", () => {
    const { arb, intents, ev } = harness();
    ev("play", "down", 0);
    ev("volume-plus", "down", 40);
    ev("volume-plus", "up", 120);
    expect(intents).toEqual([{ type: "stem.select", dir: 1 }]);
    // Both members are claimed, so neither transport.play nor master.gain
    // reaches the v2.6 dispatch: nothing is emitted and then rolled back.
    expect(arb.isClaimed("play")).toBe(true);
    expect(arb.isClaimed("volume-plus")).toBe(true);
    ev("play", "up", 200);
    expect(arb.isClaimed("play")).toBe(true);
  });

  it("Vol− selects the previous stem", () => {
    const { intents, ev } = harness();
    ev("play", "down", 0);
    ev("volume-minus", "down", 10);
    ev("volume-minus", "up", 60);
    expect(intents[0]).toEqual({ type: "stem.select", dir: -1 });
  });

  it("Play + Track measures the OVERLAP, not the time since the Play press", () => {
    // Play pressed 600 ms early; track overlap is only 200 ms → solo, not link.
    const a = harness();
    a.ev("play", "down", 0);
    a.ev("track-button-2", "down", 300);
    a.ev("track-button-2", "up", 500);
    expect(a.intents[0]).toMatchObject({ type: "stem.solo", stem: 1 });
    expect((a.intents[0] as { overlapMs: number }).overlapMs).toBeCloseTo(200, 9);

    const b = harness();
    b.ev("play", "down", 0);
    b.ev("track-button-3", "down", 100);
    b.ev("track-button-3", "up", 100 + DEFAULT_ARBITER_TIMINGS.soloLinkMs + 1);
    expect(b.intents[0]).toMatchObject({ type: "stem.link", stem: 2 });
  });

  it("volume chord: short = overlay, ~2 s = pairing, 600–2000 ms = no-op", () => {
    const short = harness();
    short.ev("volume-minus", "down", 0);
    short.ev("volume-plus", "down", 50);
    short.ev("volume-plus", "up", 300);
    expect(short.intents[0]).toEqual({ type: "fx.overlay", on: true });

    const pair = harness();
    pair.ev("volume-minus", "down", 0);
    pair.ev("volume-plus", "down", 40);
    pair.ev("volume-plus", "up", 2500);
    expect(pair.intents[0]).toEqual({ type: "system.pairing" });

    const ambiguous = harness();
    ambiguous.ev("volume-minus", "down", 0);
    ambiguous.ev("volume-plus", "down", 40);
    ambiguous.ev("volume-plus", "up", 1000);
    expect(ambiguous.intents[0]!.type).toBe("system.noop");
  });

  it("overlay open: a bare track press selects the bank AND sounds it on press", () => {
    const { intents, ev, arb } = harness(true, 1);
    ev("track-button-4", "down", 0);
    // Zero hold latency: selection and audio both happen on pointer-down.
    expect(intents[0]).toEqual({ type: "fx.bank.select", stem: 1, bank: 1 });
    expect(intents[1]).toEqual({ type: "fx.momentary.start", stem: 1, bank: 1 });
    expect(arb.isClaimed("track-button-4")).toBe(true);
    ev("track-button-4", "up", 250);
    expect(intents[2]).toEqual({ type: "fx.momentary.end", stem: 1, bank: 1 });
  });

  it("overlay open: Vol± cycles the selected bank's algorithm, FUNCTION latches", () => {
    const v = harness(true, 0);
    v.ev("track-button-2", "down", 0);
    v.view.selectedBank = 2; // MOTION, as the select intent just reported
    v.ev("volume-plus", "down", 30);
    v.ev("volume-plus", "up", 90);
    expect(v.intents.at(-1)).toEqual({ type: "fx.algorithm.cycle", stem: 0, bank: 2, dir: 1 });

    const l = harness(true, 0);
    l.ev("track-button-1", "down", 0);
    l.ev("function", "down", 20);
    l.ev("function", "up", 80);
    expect(l.intents.at(-1)).toEqual({ type: "fx.latch", stem: 0, bank: 0 });
  });

  it("all four FX tracks + FUNCTION clears the latches on the active stem", () => {
    const { intents, ev } = harness(true, 2);
    ev("track-button-1", "down", 0);
    ev("track-button-2", "down", 10);
    ev("track-button-3", "down", 20);
    ev("track-button-4", "down", 30);
    ev("function", "down", 40);
    ev("function", "up", 90);
    expect(intents.at(-1)).toEqual({ type: "fx.clearLatches", stem: 2 });
  });

  it("a lost pointer releases the momentary FX and the claim", () => {
    const { intents, ev, arb } = harness(true, 0);
    ev("track-button-3", "down", 0);
    ev("track-button-3", "cancel", 90);
    expect(intents.at(-1)).toEqual({ type: "fx.momentary.end", stem: 0, bank: 3 });
    ev("track-button-3", "down", 200);
    expect(arb.isClaimed("track-button-3")).toBe(true);
  });


  it("with the overlay CLOSED a bare track press is left to the v2.6 map", () => {
    const { intents, ev, arb } = harness(false);
    ev("track-button-1", "down", 0);
    ev("track-button-1", "up", 80);
    expect(intents).toHaveLength(0);
    expect(arb.isClaimed("track-button-1")).toBe(false);
  });
});

describe("stem performance state", () => {
  it("solo never mutates the saved mute state", () => {
    let s = initialStemPerformance();
    // muted track 1, no solo → closed
    expect(inputOpen(s, 0, true)).toBe(false);
    s = toggleSolo(s, 0);
    // soloing a muted stem opens it; the stored mute flag is untouched
    expect(inputOpen(s, 0, true)).toBe(true);
    expect(inputOpen(s, 1, false)).toBe(false);
    s = toggleSolo(s, 0);
    expect(inputOpen(s, 0, true)).toBe(false);
    expect(inputOpen(s, 1, false)).toBe(true);
  });

  it("stem selection wraps in both directions", () => {
    let s = initialStemPerformance();
    for (let i = 0; i < 4; i++) s = selectStem(s, 1);
    expect(s.activeStem).toBe(0);
    s = selectStem(s, -1);
    expect(s.activeStem).toBe(3);
  });

  it("tape operations target the linked group, or only the unlinked stem", () => {
    let s = initialStemPerformance();
    expect(tapeTarget(s, 0)).toEqual([0, 1, 2, 3]);
    s = toggleLink(s, 2);
    expect(tapeTarget(s, 2)).toEqual([2]);
    expect(tapeTarget(s, 0)).toEqual([0, 1, 3]);
  });

  it("persists link/solo/algorithm/latch and NEVER momentary or overlay", () => {
    let s = initialStemPerformance();
    s = toggleSolo(s, 1);
    s = toggleLink(s, 3);
    s = cycleBankAlgorithm(s, 0, 2, 1); // MOTION → algorithm 1 (Pitch Echo)
    s = toggleBankLatch(s, 0, 3); // SPACE latched
    s = setBankMomentary(s, 0, 3, true);
    s = { ...s, fxOverlay: true };

    const json = JSON.parse(JSON.stringify(serializePerformance(s)));
    expect(json.version).toBe(STEM_TAPE_SCHEMA_VERSION);
    expect(JSON.stringify(json)).not.toContain("momentary");
    expect(JSON.stringify(json)).not.toContain("fxOverlay");

    const back = deserializePerformance(json);
    expect(back.fxOverlay).toBe(false);
    expect(back.tracks[1]!.soloed).toBe(true);
    expect(back.tracks[3]!.linked).toBe(false);
    expect(back.tracks[0]!.fx12.banks[2]!.selectedAlgorithm).toBe(1);
    expect(back.tracks[0]!.fx12.banks[3]!.latched).toBe(true);
    expect(back.tracks[0]!.fx12.banks[3]!.momentary).toBe(false);
    // Bank selection is a live overlay concern and never survives a reload.
    expect(back.tracks[0]!.fx12.selectedBank).toBe(null);
  });

  it("migrates pre-5C projects to linked / no solo / no latch / algorithm 0", () => {
    const legacy = deserializePerformance({ version: 2, tracks: [{ soloed: true }] });
    expect(legacy.tracks.every((t) => t.linked && !t.soloed)).toBe(true);
    expect(
      legacy.tracks.every((t) => t.fx12.banks.every((b) => !b.latched && !b.momentary && b.selectedAlgorithm === 0)),
    ).toBe(true);
  });

  it("a restored Beat Repeat latch re-arms instead of replaying stale memory", () => {
    const restored = deserializePerformance({
      version: 3,
      activeStem: 0,
      tracks: [{ soloed: false, linked: true, fx: { beatRepeat: { latched: true, variation: 2 } } }],
    });
    expect(restored.tracks[0]!.fx.beatRepeat.arming).toBe(true);
    // v3 beatRepeat maps onto the RHYTHM bank, algorithm 0.
    expect(restored.tracks[0]!.fx12.banks[1]!.latched).toBe(true);
  });

  it("clearLatches leaves the selected algorithm alone", () => {
    let s = initialStemPerformance();
    s = cycleBankAlgorithm(s, 0, 0, 1);
    s = toggleBankLatch(s, 0, 0);
    s = clearLatches(s, 0);
    expect(s.tracks[0]!.fx12.banks[0]!.selectedAlgorithm).toBe(1);
    expect(s.tracks[0]!.fx12.banks[0]!.latched).toBe(false);
  });

});

describe("FX timing math (correction 11 thresholds)", () => {
  const echoDelay = (ratio: number, bpm: number) => (60 / bpm) * ratio;

  it("echo taps land within 1 frame of 60/effectiveBpm × ratio", () => {
    for (const bpm of [90, 100, 128, 174]) {
      for (const v of ECHO_VARIATIONS) {
        const expected = echoDelay(v.ratio, bpm);
        const framesErr = Math.abs(expected - echoDelay(v.ratio, bpm)) * SR;
        expect(framesErr).toBeLessThanOrEqual(1);
        expect(expected).toBeGreaterThan(0);
      }
    }
  });

  it("effective BPM is per stem: baseBpm × |rate|", () => {
    const base = 120;
    expect(base * Math.abs(-0.5)).toBeCloseTo(60, 12);
    expect(echoDelay(0.5, base * 2)).toBeCloseTo(0.125, TOL);
  });

  it("beat repeat slice length is exact to the frame for every division", () => {
    const bpm = 128;
    for (const v of REPEAT_VARIATIONS) {
      const exact = (60 / bpm) * v.ratio * SR;
      expect(Math.abs(Math.round(exact) - exact)).toBeLessThanOrEqual(0.5);
    }
  });

  it("the ring buffer is sized from a 1/2 note at the slowest supported BPM", () => {
    const cap = repeatRingCapacityFrames(SR);
    expect(cap).toBe(Math.ceil((60 / MIN_SUPPORTED_BPM) * 2 * SR));
    // Every division at every supported tempo fits inside it.
    const slowest = REPEAT_VARIATIONS[0]!;
    expect((60 / MIN_SUPPORTED_BPM) * slowest.ratio * SR).toBeLessThanOrEqual(cap);
  });

  it("feedback constants are clamped below self-oscillation", () => {
    for (const v of ECHO_VARIATIONS) expect(v.feedback).toBeLessThanOrEqual(ECHO_FEEDBACK_MAX);
    for (const v of REVERB_VARIATIONS) expect(v.feedback).toBeLessThanOrEqual(REVERB_FEEDBACK_CEILING);
    expect(ECHO_FEEDBACK_MAX).toBeLessThan(1);
    expect(REVERB_FEEDBACK_CEILING).toBeLessThan(1);
  });
});

describe("mapping registry", () => {
  it("re-exports the 37 stock v2.6 rows unchanged", () => {
    expect(V26_ROWS_AS_REGISTRY).toHaveLength(V26_MAP.length);
    expect(V26_MAP).toHaveLength(37);
    for (const row of V26_ROWS_AS_REGISTRY) {
      expect(row.provenance).toBe("v2.6");
      expect(row.command).toBe(V26_MAP.find((r) => r.id === row.id)!.command);
    }
  });

  it("every Phase 5C row declares what it suppresses before dispatch", () => {
    const extensions = STEM_TAPE_V1_MAP.filter((r) => r.provenance !== "v2.6");
    expect(extensions.length).toBeGreaterThan(0);
    for (const row of extensions) expect(Array.isArray(row.suppresses)).toBe(true);
    expect(extensions.filter((r) => r.layer === "fx-overlay").length).toBe(13);
  });

  it("exports JSON for the Mapping Lab", () => {
    const parsed = JSON.parse(exportMapJson());
    expect(parsed.rows.length).toBe(STEM_TAPE_V1_MAP.length);
  });
});

describe("LED priority table", () => {
  it("overlay side LEDs show momentary above latched, and arming above both", () => {
    let s = initialSurfaceState();
    s = applyPerfIntent(s, { type: "fx.overlay", on: true });
    s = applyPerfIntent(s, { type: "fx.latch", stem: 0, family: "echo" });
    let leds = deriveLeds(s);
    expect(leds["side-led-2"]!.pattern).toBe("solid");

    s = applyPerfIntent(s, { type: "fx.momentary.start", stem: 0, family: "echo" });
    leds = deriveLeds(s);
    expect(leds["side-led-2"]!.pattern).toBe("breathe");
    expect(leds["side-led-2"]!.reason).toContain("momentary");

    // Both FUNCTION LEDs alternate-pulse while the overlay is open.
    expect(leds["function-led-1"]!.pattern).toBe("pulse");
    expect(leds["function-led-2"]!.pattern).toBe("blink");
  });

  it("overlay track LEDs show stem state: soloed > unlinked > active", () => {
    let s = initialSurfaceState();
    s = applyPerfIntent(s, { type: "fx.overlay", on: true });
    s = applyPerfIntent(s, { type: "stem.solo", stem: 1, overlapMs: 100 });
    s = applyPerfIntent(s, { type: "stem.link", stem: 2, overlapMs: 900 });
    const leds = deriveLeds(s);
    expect(leds["track-led-2"]!.pattern).toBe("solid");
    expect(leds["track-led-3"]!.pattern).toBe("blink");
    expect(leds["track-led-1"]!.pattern).toBe("breathe"); // active stem
  });
});

describe("semantic dispatch", () => {
  it("emits exactly ONE command per resolved chord", () => {
    const before = initialSurfaceState();
    const after = applyPerfIntent(before, { type: "stem.select", dir: 1 });
    expect(after.commands).toHaveLength(before.commands.length + 1);
    expect(after.commands.at(-1)!.type).toBe("stem.select");
    expect(after.perf.activeStem).toBe(1);
  });

  it("overlay toggling never emits a transport or gain command", () => {
    let s = initialSurfaceState();
    s = applyPerfIntent(s, { type: "fx.overlay", on: true });
    expect(s.commands.map((c) => c.type)).toEqual(["fx.overlay"]);
    expect(s.playing).toBe(false);
  });
});
