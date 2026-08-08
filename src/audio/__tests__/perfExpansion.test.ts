import { describe, expect, it } from "vitest";
import {
  DEFAULT_INERTIA_PRESET,
  INERTIA_PRESETS,
  inertiaCurve,
  inertiaDistance,
  inertiaRateAt,
  makeInertiaSegment,
} from "../inertia";
import { TapeTimeline } from "../tape";
import { FaderSessionManager } from "@/input/faderSessions";
import { cycleAlgorithm, initialStemFx, nudgeMacro, rejectAlgorithm, isBankActive } from "@/machine/fx12";

const TOL = 1e-9;
const clamp01 = (v: number) => Math.min(1, Math.max(0, v));

describe("Workstream 2 — tape inertia", () => {
  it("defaults to Classic 300 ms / 450 ms", () => {
    expect(DEFAULT_INERTIA_PRESET).toBe("classic");
    expect(INERTIA_PRESETS.classic.startS).toBeCloseTo(0.3, 12);
    expect(INERTIA_PRESETS.classic.stopS).toBeCloseTo(0.45, 12);
  });

  it("wind-up terminates EXACTLY on the musical rate", () => {
    const seg = makeInertiaSegment({
      startAt: 0,
      currentRate: 1e-4,
      targetRate: 1,
      preset: INERTIA_PRESETS.classic,
      kind: "windUp",
    });
    expect(inertiaRateAt(seg, seg.durationS)).toBeCloseTo(1, 12);
    const curve = inertiaCurve(seg);
    expect(curve[curve.length - 1]).toBeCloseTo(1, 12);
  });

  it("integrated distance matches numeric integration of the rate curve", () => {
    const seg = makeInertiaSegment({
      startAt: 0,
      currentRate: 1,
      targetRate: 1e-4,
      preset: INERTIA_PRESETS.classic,
      kind: "windDown",
    });
    const N = 200000;
    let numeric = 0;
    for (let i = 0; i < N; i++) {
      const dt = ((i + 0.5) / N) * seg.durationS;
      numeric += inertiaRateAt(seg, dt) * (seg.durationS / N);
    }
    expect(inertiaDistance(seg, seg.durationS)).toBeCloseTo(numeric, 6);
  });

  it("the timeline playhead integrates the same ramp (no drift after it ends)", () => {
    const tl = new TapeTimeline();
    tl.anchor(0, 0);
    const seg = makeInertiaSegment({
      startAt: 0,
      currentRate: 1e-4,
      targetRate: 1,
      preset: INERTIA_PRESETS.classic,
      kind: "windUp",
    });
    tl.startInertia(0, seg);
    const atEnd = tl.positionAt(seg.durationS);
    expect(atEnd).toBeCloseTo(inertiaDistance(seg, seg.durationS), 9);
    tl.endInertia(seg.durationS, 1);
    expect(tl.positionAt(seg.durationS + 1)).toBeCloseTo(atEnd + 1, 9);
  });

  it("a reversal mid-ramp rebases from the instantaneous rate and is shorter", () => {
    const up = makeInertiaSegment({
      startAt: 0,
      currentRate: 1e-4,
      targetRate: 1,
      preset: INERTIA_PRESETS.classic,
      kind: "windUp",
    });
    const mid = inertiaRateAt(up, up.durationS * 0.9);
    const down = makeInertiaSegment({
      startAt: 0.27,
      currentRate: mid,
      targetRate: 1e-4,
      preset: INERTIA_PRESETS.classic,
      kind: "windDown",
    });
    expect(down.from).toBeCloseTo(mid, TOL);
    expect(down.durationS).toBeLessThan(INERTIA_PRESETS.classic.stopS);
  });
});

describe("Workstream 1 — simultaneous faders", () => {
  const mgr = () => new FaderSessionManager();

  it("four pointers own four faders and flush on ONE shared frame", () => {
    const m = mgr();
    for (let i = 0; i < 4; i++) {
      const s = m.begin({
        pointerId: 100 + i,
        faderIndex: i as 0 | 1 | 2 | 3,
        userY: 0,
        pointerValue: 0.5,
        currentValue: 0.5,
        channel: "fader",
        source: "touch",
        t: 0,
      });
      expect(s).not.toBeNull();
    }
    for (let i = 0; i < 4; i++) m.move(100 + i, 10, 0.5 + 0.1 * (i + 1), clamp01);
    const batch = m.flush();
    expect(batch).not.toBeNull();
    expect(batch!.previews).toHaveLength(4);
    expect(new Set(batch!.previews.map((p) => p.faderIndex)).size).toBe(4);
  });

  it("a second pointer cannot steal an owned fader, and each up ends only its own", () => {
    const m = mgr();
    const base = {
      faderIndex: 1 as const,
      userY: 0,
      pointerValue: 0.4,
      currentValue: 0.4,
      channel: "fader" as const,
      source: "touch" as const,
      t: 0,
    };
    expect(m.begin({ ...base, pointerId: 1 })).not.toBeNull();
    expect(m.begin({ ...base, pointerId: 2 })).toBeNull();
    expect(m.owner(1)).toBe(1);
    expect(m.end(2)).toBeNull();
    expect(m.end(1)?.pointerId).toBe(1);
    expect(m.owner(1)).toBeNull();
  });

  it("a modifier change mid-gesture truly rebases without a value jump", () => {
    const m = mgr();
    m.begin({
      pointerId: 7,
      faderIndex: 2,
      userY: 0,
      pointerValue: 0.6,
      currentValue: 0.6,
      channel: "fader",
      source: "mouse",
      t: 0,
    });
    m.move(7, 5, 0.8, clamp01);
    const rebased = m.rebase(7, "window", 0.8, 0.25);
    expect(rebased!.channel).toBe("window");
    // Pointer has not moved, so the new channel reports its own value exactly.
    expect(m.move(7, 5, 0.8, clamp01)).toBeCloseTo(0.25, 12);
    expect(m.flush()!.previews.every((p) => p.channel === "window")).toBe(true);
  });
});

describe("Workstream 3 — twelve FX", () => {
  it("macro values are per algorithm, never shared across a bank", () => {
    let bank = initialStemFx().banks[3]!;
    const before = bank.algorithms.map((a) => a.macroAmount);
    bank = nudgeMacro(3, bank, 1);
    const after = bank.algorithms.map((a) => a.macroAmount);
    const changed = after.filter((v, i) => v !== before[i]);
    expect(changed).toHaveLength(1);
    expect(after[bank.selectedAlgorithm]).not.toBe(before[bank.selectedAlgorithm]);
  });

  it("rejecting one algorithm never marks the whole bank rejected", () => {
    let bank = initialStemFx().banks[3]!;
    bank = rejectAlgorithm(bank, 2, "Spectral Freeze unsupported");
    expect(bank.algorithms[2]!.rejected).toBe("Spectral Freeze unsupported");
    expect(bank.algorithms[0]!.rejected).toBeNull();
    expect(bank.algorithms[1]!.rejected).toBeNull();
    const cycled = cycleAlgorithm(bank, 1);
    expect(cycled.selectedAlgorithm).not.toBe(bank.selectedAlgorithm);
  });

  it("cycling an algorithm does not activate an idle bank", () => {
    let bank = initialStemFx().banks[0]!;
    expect(isBankActive(bank)).toBe(false);
    bank = cycleAlgorithm(bank, 1);
    expect(isBankActive(bank)).toBe(false);
  });
});
