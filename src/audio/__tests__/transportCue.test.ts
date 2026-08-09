import { describe, expect, it } from "vitest";
import { TapeTimeline } from "../tape";
import { INERTIA_PRESETS, INERTIA_MIN_RATE, CUE_FADE_S, makeInertiaSegment, inertiaRateAt } from "../inertia";
import { TapeTimelineBus } from "../timelineEvents";
import { V26_MAP } from "@/machine/v26map";
import { STEM_TAPE_V1_MAP, TRANSPORT_OVERRIDE_ROWS, V26_ROWS_AS_REGISTRY } from "@/machine/stemTapeV1Map";
import { BANKS, BANK_BY_BUTTON, bankOfButton, physicalButtonOf, cycleAlgorithm, initialStemFx } from "@/machine/fx12";

const TOL = 1e-9;

describe("Correction 1 — three separate transport values", () => {
  it("a wind-down to ~0 never overwrites the musical target rate", () => {
    const tl = new TapeTimeline(1);
    tl.anchor(0, 0);
    const seg = makeInertiaSegment({
      startAt: 0,
      currentRate: 1,
      targetRate: INERTIA_MIN_RATE,
      preset: INERTIA_PRESETS.classic,
      kind: "windDown",
    });
    tl.startInertia(0, seg);
    tl.endInertia(seg.durationS, INERTIA_MIN_RATE);
    expect(tl.currentRate(seg.durationS)).toBeLessThan(1e-3);
    expect(tl.musicalRate()).toBeCloseTo(1, 12); // preserved
  });

  it("the position stays frozen while stopped and the musical rate stays 1.00×", () => {
    const tl = new TapeTimeline(1);
    tl.anchor(0, 12.5);
    tl.endInertia(0, INERTIA_MIN_RATE);
    tl.anchor(0, 12.5);
    expect(tl.positionAt(0)).toBeCloseTo(12.5, 12);
    expect(tl.musicalRate()).toBeCloseTo(1, 12);
  });

  it("100 stop/play cycles never decay the musical rate (one-tap transport proof)", () => {
    const tl = new TapeTimeline(1);
    let t = 0;
    let pos = 0;
    for (let i = 0; i < 100; i++) {
      // Play: wind up from standstill to the PRESERVED musical rate.
      const up = makeInertiaSegment({
        startAt: t,
        currentRate: INERTIA_MIN_RATE,
        targetRate: tl.musicalRate(),
        preset: INERTIA_PRESETS.classic,
        kind: "windUp",
      });
      expect(up.to).toBeCloseTo(1, 12);
      tl.anchor(t, pos);
      tl.startInertia(t, up);
      t += up.durationS;
      tl.endInertia(t, tl.musicalRate());
      t += 0.25;
      pos = tl.positionAt(t);
      expect(tl.currentRate(t)).toBeCloseTo(1, 9);
      // Stop: wind down to ~0. The musical target must survive.
      const down = makeInertiaSegment({
        startAt: t,
        currentRate: tl.currentRate(t),
        targetRate: INERTIA_MIN_RATE,
        preset: INERTIA_PRESETS.classic,
        kind: "windDown",
      });
      tl.startInertia(t, down);
      t += down.durationS;
      pos = tl.positionAt(t);
      tl.endInertia(t, INERTIA_MIN_RATE);
      tl.anchor(t, pos);
      expect(tl.musicalRate()).toBeCloseTo(1, 12);
      // Frozen: 1 s of wall clock advances the playhead by nothing.
      expect(tl.positionAt(t)).toBeCloseTo(pos, 9);
    }
    expect(tl.musicalRate()).toBeCloseTo(1, 12);
    expect(pos).toBeGreaterThan(20);
  });
});

describe("Correction 2 — interrupt and rebase, never complete", () => {
  it("interruptInertia returns the INSTANTANEOUS rate mid-ramp and re-anchors there", () => {
    const tl = new TapeTimeline(1);
    tl.anchor(0, 0);
    const down = makeInertiaSegment({
      startAt: 0,
      currentRate: 1,
      targetRate: INERTIA_MIN_RATE,
      preset: INERTIA_PRESETS.classic,
      kind: "windDown",
    });
    tl.startInertia(0, down);
    const at = 0.2;
    const expected = inertiaRateAt(down, at);
    const posBefore = tl.positionAt(at);
    const instant = tl.interruptInertia(at);
    expect(instant).toBeCloseTo(expected, 9);
    expect(instant).toBeGreaterThan(0.05);
    expect(instant).toBeLessThan(1);
    // Position is continuous across the interruption — no jump, no restart.
    expect(tl.positionAt(at)).toBeCloseTo(posBefore, 12);
    // The musical target is untouched, so the reversal winds up to 1.00×.
    expect(tl.musicalRate()).toBeCloseTo(1, 12);
    const up = makeInertiaSegment({
      startAt: at,
      currentRate: instant,
      targetRate: tl.musicalRate(),
      preset: INERTIA_PRESETS.classic,
      kind: "windUp",
    });
    expect(up.from).toBeCloseTo(instant, 12);
    expect(up.to).toBeCloseTo(1, 12);
    // Reversal is shortened proportionally — it never restarts from zero.
    expect(up.durationS).toBeLessThan(INERTIA_PRESETS.classic.startS + TOL);
  });
});

describe("Correction 3/7 — cue", () => {
  it("the cue fade is ~8 ms and the cue frame is fixed at zero for v1", () => {
    expect(CUE_FADE_S).toBeCloseTo(0.008, 12);
    const tl = new TapeTimeline(1);
    tl.anchor(0, 31.7);
    tl.anchor(CUE_FADE_S, 0);
    expect(tl.positionAt(CUE_FADE_S)).toBeCloseTo(0, 12);
    expect(tl.musicalRate()).toBeCloseTo(1, 12);
  });
});

describe("Correction 5 — the authoritative v2.6 map is untouched", () => {
  it("all 37 documented v2.6 rows survive verbatim", () => {
    expect(V26_MAP.length).toBe(37);
    expect(V26_ROWS_AS_REGISTRY.length).toBe(37);
    for (const r of V26_ROWS_AS_REGISTRY) expect(r.provenance).toBe("v2.6");
    const ids = new Set(V26_MAP.map((r) => r.id));
    expect(ids.has("rocker.chop")).toBe(true);
    expect(ids.has("play.restart")).toBe(true);
  });

  it("Stem Tape overrides are additive and declare what they supersede", () => {
    const cue = TRANSPORT_OVERRIDE_ROWS.find((r) => r.id === "play.cue")!;
    const scrub = TRANSPORT_OVERRIDE_ROWS.find((r) => r.id === "rocker.scrub")!;
    expect(cue.provenance).toBe("reinterpreted");
    expect(cue.supersedes).toContain("play.restart");
    expect(cue.originalBehaviour).toMatch(/restarts the loop/i);
    expect(scrub.supersedes).toContain("rocker.chop");
    expect(STEM_TAPE_V1_MAP.filter((r) => r.provenance === "v2.6").length).toBe(37);
    expect(STEM_TAPE_V1_MAP.some((r) => r.id === "rocker.scrub")).toBe(true);
  });
});

describe("Correction 6 — one ordered timeline event stream", () => {
  it("events are ordered, numbered and delivered to every subscriber", () => {
    const bus = new TapeTimelineBus();
    const seen: string[] = [];
    const off = bus.subscribe((e) => seen.push(`${e.seq}:${e.type}`));
    bus.emit({ type: "RateChange", rate: 1.5, musicalRate: 1.5, rampFrames: 0, cause: "rate.set" });
    bus.emit({ type: "GlideChange", from: 1.5, to: 1, tau: 0.08, cause: "rate.set" });
    bus.emit({ type: "LoopWrap", track: 0, positionS: 2 });
    bus.emit({ type: "DirectionChange", reversed: true });
    off();
    bus.emit({ type: "ChopChange", track: 1, div: 4 });
    expect(seen).toEqual(["1:RateChange", "2:GlideChange", "3:LoopWrap", "4:DirectionChange"]);
    expect(bus.log.map((e) => e.seq)).toEqual([1, 2, 3, 4, 5]);
    expect(bus.count("RateChange")).toBe(1);
  });
});

describe("Correction 8 / MOD button", () => {
  it("MOD is zero-indexed buttonIndex 3 === physical Button 4", () => {
    const rhythm = BANKS.find((b) => b.id === "mod")!;
    expect(rhythm.buttonIndex).toBe(3);
    expect(physicalButtonOf(BANKS.indexOf(rhythm) as 0 | 1 | 2 | 3)).toBe(4);
    expect(BANKS[bankOfButton(3)]!.id).toBe("mod");
    expect(BANK_BY_BUTTON.length).toBe(4);
  });

  it("one − from Reel Flange wraps to Rhythmic Gate", () => {
    const fx = initialStemFx();
    const bank = bankOfButton(3);
    let b = fx.banks[bank]!;
    expect(BANKS[bank]!.algorithms[b.selectedAlgorithm]!.id).toBe("reelFlange");
    b = cycleAlgorithm(b, -1);
    expect(BANKS[bank]!.algorithms[b.selectedAlgorithm]!.id).toBe("gate");
    b = cycleAlgorithm(b, 1);
    expect(BANKS[bank]!.algorithms[b.selectedAlgorithm]!.id).toBe("reelFlange");
    b = cycleAlgorithm(b, 1);
    expect(BANKS[bank]!.algorithms[b.selectedAlgorithm]!.id).toBe("formantShift");
  });

  it("tempo-derived effects read the MUSICAL rate, so a wind-down cannot silence the gate", () => {
    const tl = new TapeTimeline(1);
    tl.anchor(0, 0);
    const down = makeInertiaSegment({
      startAt: 0,
      currentRate: 1,
      targetRate: INERTIA_MIN_RATE,
      preset: INERTIA_PRESETS.classic,
      kind: "windDown",
    });
    tl.startInertia(0, down);
    const baseBpm = 120;
    const instantBpm = baseBpm * Math.abs(tl.currentRate(0.44));
    const musicalBpm = baseBpm * Math.abs(tl.musicalRate());
    expect(instantBpm).toBeLessThan(5); // the old, broken source — LFO ≈ 0 Hz
    expect(musicalBpm).toBeCloseTo(120, 9);
    expect(musicalBpm / 60).toBeCloseTo(2, 9); // 2 Hz pump
  });
});
