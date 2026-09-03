/**
 * S2 verification — signed master head.
 *
 * FOUR REAL TapeProcessor instances (the shipped public/tape-processor.js) are
 * driven with one master-scratch program. The tests prove that all four lanes
 * derive their read positions from ONE master position, including a
 * long-running direction/rate torture run designed specifically to expose
 * accumulated inter-stem drift.
 */

import { beforeAll, describe, expect, it } from "vitest";
import { advanceMaster, clampVelocity, masterToSourceFrame, SCRATCH_TUNING } from "../masterScratch";

const SR = 48000;
const g = globalThis as unknown as Record<string, unknown>;

type Proc = {
  port: { postMessage: (m: unknown, t?: unknown) => void; onmessage: ((e: { data: unknown }) => void) | null; close: () => void };
  process: (i: unknown, o: Float32Array[][]) => boolean;
  readPosition: number;
  rateScale: number;
  wrapCount: number;
  master: { pos: number; vel: number } | null;
  rate: number;
  direction: 1 | -1;
};
let Processor: new (opts?: unknown) => Proc;

beforeAll(async () => {
  g["sampleRate"] = SR;
  g["currentFrame"] = 0;
  g["AudioWorkletProcessor"] = class {
    port = { postMessage: () => {}, onmessage: null as ((e: { data: unknown }) => void) | null, close: () => {} };
  };
  g["registerProcessor"] = (_n: string, ctor: unknown) => {
    Processor = ctor as typeof Processor;
  };
  const m = (await import("../../../public/tape-processor.js?raw")) as { default: string };
  new Function("AudioWorkletProcessor", "registerProcessor", "sampleRate", m.default)(
    g["AudioWorkletProcessor"],
    g["registerProcessor"],
    SR,
  );
});

interface Lane {
  p: Proc;
  send: (msg: Record<string, unknown>) => void;
}

/** Four lanes, each with its own PCM (and optionally its own source rate). */
function fourLanes(frames = SR * 20, sourceRates = [SR, SR, SR, SR]): Lane[] {
  g["currentFrame"] = 0;
  return sourceRates.map((sourceRate, i) => {
    const p = new Processor({ processorOptions: { trackId: i } });
    p.port.postMessage = () => {};
    const send = (msg: Record<string, unknown>) => p.port.onmessage?.({ data: msg });
    const n = Math.round((frames * sourceRate) / SR);
    const pcm = new Float32Array(n);
    for (let k = 0; k < n; k++) pcm[k] = Math.sin(k / 97);
    send({
      type: "adopt",
      seq: 1,
      channels: [pcm.buffer],
      metadata: { frames: n, channels: 1, sampleRate: sourceRate, durationS: n / sourceRate },
    });
    return { p, send };
  });
}

function engage(lanes: Lane[], masterFrame: number, velocity: number, loop?: { start: number; end: number }, songFrames = SR * 20) {
  for (const l of lanes) {
    l.send({
      type: "masterScratch",
      seq: 2,
      phase: "engage",
      applyAtContextFrame: g["currentFrame"] as number,
      masterFrame,
      velocity,
      loopEnabled: !!loop,
      loopStartMaster: loop?.start ?? 0,
      loopEndMaster: loop?.end ?? 0,
      songFramesMaster: songFrames,
    });
  }
}

function setVelocity(lanes: Lane[], velocity: number, rampFrames = 0) {
  for (const l of lanes) {
    l.send({
      type: "masterScratch",
      seq: 3,
      phase: "velocity",
      applyAtContextFrame: g["currentFrame"] as number,
      velocity,
      rampFrames,
    });
  }
}

function render(lanes: Lane[], blocks: number, quantum = 128) {
  for (let b = 0; b < blocks; b++) {
    for (const l of lanes) {
      const chan = [new Float32Array(quantum)];
      l.p.process([], [chan]);
    }
    g["currentFrame"] = (g["currentFrame"] as number) + quantum;
  }
}

const masterOf = (l: Lane) => l.p.master!.pos;

describe("S2 — one master position, four derived read positions", () => {
  it("commands +1.0, +0.5, 0, -0.5, -1.0 and keeps one coherent master position", () => {
    const lanes = fourLanes();
    engage(lanes, 100000, 1);
    for (const v of [1, 0.5, 0, -0.5, -1]) {
      setVelocity(lanes, v);
      render(lanes, 20);
      const positions = lanes.map(masterOf);
      expect(new Set(positions).size).toBe(1);
      expect(lanes[0]!.p.master!.vel).toBeCloseTo(v, 12);
    }
  });

  it("ramps through zero from positive to negative without a discontinuity", () => {
    const lanes = fourLanes();
    engage(lanes, 200000, 1);
    setVelocity(lanes, -1, 2048);
    const seen: number[] = [];
    for (let i = 0; i < 24; i++) {
      render(lanes, 1);
      seen.push(lanes[0]!.p.master!.vel);
    }
    expect(seen.some((v) => v > 0)).toBe(true);
    expect(seen[seen.length - 1]!).toBeLessThan(seen[0]!);
    for (let i = 1; i < seen.length; i++) expect(Math.abs(seen[i]! - seen[i - 1]!)).toBeLessThan(0.2);
    render(lanes, 40);
    expect(lanes[0]!.p.master!.vel).toBeCloseTo(-1, 10);
    expect(new Set(lanes.map(masterOf)).size).toBe(1);
  });

  it("read positions are an exact mapping of the master position (mixed source rates)", () => {
    const lanes = fourLanes(SR * 20, [SR, SR, 44100, 96000]);
    engage(lanes, 50000, 1);
    setVelocity(lanes, -0.75);
    render(lanes, 50);
    const master = masterOf(lanes[0]!);
    for (const l of lanes) {
      expect(masterOf(l)).toBe(master); // identical accumulation, not "close"
      expect(l.p.readPosition).toBeCloseTo(masterToSourceFrame(master, l.p.rateScale), 9);
    }
  });

  it("long-running torture run: 400 direction/rate changes leave ZERO inter-stem drift", () => {
    const lanes = fourLanes(SR * 60, [SR, SR, 44100, 96000]);
    engage(lanes, 1_000_000, 1);
    let seed = 12345;
    const rnd = () => ((seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648);
    for (let step = 0; step < 400; step++) {
      const v = clampVelocity((rnd() * 2 - 1) * 4);
      setVelocity(lanes, v, step % 3 === 0 ? 256 : 0);
      render(lanes, 6);
      const positions = lanes.map(masterOf);
      expect(Math.max(...positions) - Math.min(...positions)).toBe(0);
    }
    // ~5.1 s of context time later, the lanes are still one instant.
    const master = masterOf(lanes[0]!);
    for (const l of lanes) {
      expect(masterOf(l)).toBe(master);
      expect(Math.abs(l.p.readPosition / l.p.rateScale - master)).toBeLessThan(1e-9);
    }
  });

  it("matches the reference integrator in masterScratch.ts", () => {
    const lanes = fourLanes();
    engage(lanes, 10000, 1);
    setVelocity(lanes, -2, 512);
    render(lanes, 8);
    const ref = advanceMaster(
      { pos: 10000, vel: 1, target: -2, rampLeft: 512, step: (-2 - 1) / 512, loopEnabled: false, loopStart: 0, loopEnd: 0, songFrames: SR * 20 },
      8 * 128,
    );
    expect(masterOf(lanes[0]!)).toBeCloseTo(ref.pos, 6);
  });
});

describe("S2 — bounds", () => {
  it("clamps at song start and stops integrating backwards", () => {
    const lanes = fourLanes(SR * 2);
    engage(lanes, 1000, -1, undefined, SR * 2);
    setVelocity(lanes, -3);
    render(lanes, 40);
    for (const l of lanes) {
      expect(masterOf(l)).toBe(0);
      expect(l.p.master!.vel).toBe(0);
      expect(l.p.wrapCount).toBe(0);
    }
  });

  it("clamps at song end with no accidental wrap", () => {
    const songFrames = SR * 2;
    const lanes = fourLanes(SR * 2);
    engage(lanes, songFrames - 500, 3, undefined, songFrames);
    render(lanes, 40);
    for (const l of lanes) {
      expect(masterOf(l)).toBe(songFrames);
      expect(l.p.wrapCount).toBe(0);
    }
  });

  it("wraps inside an active loop in BOTH signed directions, identically on all lanes", () => {
    const loop = { start: 48000, end: 96000 };
    const lanes = fourLanes(SR * 20);
    engage(lanes, 95000, 2, loop);
    render(lanes, 60); // forward wrap
    for (const l of lanes) {
      expect(masterOf(l)).toBeGreaterThanOrEqual(loop.start);
      expect(masterOf(l)).toBeLessThan(loop.end);
      expect(l.p.wrapCount).toBeGreaterThanOrEqual(1);
    }
    setVelocity(lanes, -2);
    render(lanes, 200); // backward wraps
    const positions = lanes.map(masterOf);
    expect(new Set(positions).size).toBe(1);
    expect(positions[0]!).toBeGreaterThanOrEqual(loop.start);
    expect(positions[0]!).toBeLessThan(loop.end);
    expect(new Set(lanes.map((l) => l.p.wrapCount)).size).toBe(1);
  });
});

describe("S2 — deterministic release", () => {
  it("hands the exact master position back to normal transport with no jump", () => {
    const lanes = fourLanes();
    engage(lanes, 400000, 1);
    setVelocity(lanes, -1.5);
    render(lanes, 30);
    const before = lanes.map((l) => l.p.readPosition);
    for (const l of lanes) {
      l.send({
        type: "masterScratch",
        seq: 9,
        phase: "release",
        applyAtContextFrame: g["currentFrame"] as number,
        resumeRate: 1,
        rampFrames: 0,
      });
    }
    render(lanes, 1);
    lanes.forEach((l, i) => {
      expect(l.p.master).toBe(null);
      // Continues from exactly where the tape was left, in the direction it
      // was travelling (one block on) — no jump, no re-seek.
      expect(l.p.readPosition - before[i]!).toBeCloseTo(-128 * l.p.rateScale, 6);
      expect(l.p.direction).toBe(-1); // derived from the signed velocity
      expect(l.p.rate).toBeCloseTo(1, 9);
    });
  });
});

describe("S3 tuning surface", () => {
  it("keeps a conservative, centrally tunable velocity ceiling", () => {
    expect(SCRATCH_TUNING.maxAbsVelocity).toBeLessThanOrEqual(4);
    expect(clampVelocity(99)).toBe(SCRATCH_TUNING.maxAbsVelocity);
    expect(clampVelocity(-99)).toBe(-SCRATCH_TUNING.maxAbsVelocity);
  });
});
