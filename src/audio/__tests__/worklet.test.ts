/**
 * Phase 5B offline verification.
 *
 * The real TapeProcessor source (public/tape-processor.js) is loaded into a
 * stubbed AudioWorkletGlobalScope, so these are measurements of the shipped
 * render loop — not of a copy written for the test.
 */

import { beforeAll, describe, expect, it } from "vitest";
import { pairwiseDrift, sharedApplyFrame } from "../workletProtocol";
import { estimateMigration } from "../workletBudget";
import { MiB } from "../memory";

const SR = 48000;
const g = globalThis as unknown as Record<string, unknown>;

let Processor: new (opts?: unknown) => {
  port: { postMessage: (m: unknown, t?: unknown) => void; onmessage: ((e: { data: unknown }) => void) | null; close: () => void };
  process: (i: unknown, o: Float32Array[][]) => boolean;
  readPosition: number;
  wrapCount: number;
  playing: boolean;
};

beforeAll(async () => {
  g["sampleRate"] = SR;
  g["currentFrame"] = 0;
  g["currentTime"] = 0;
  g["AudioWorkletProcessor"] = class {
    port = {
      postMessage: () => {},
      onmessage: null as ((e: { data: unknown }) => void) | null,
      close: () => {},
    };
  };
  g["registerProcessor"] = (_name: string, ctor: unknown) => {
    Processor = ctor as typeof Processor;
  };
  await import("../../../public/tape-processor.js?raw").then(async (m) => {
    // Evaluate the real source in this stubbed scope.
    const src = (m as { default: string }).default;
    // eslint-disable-next-line @typescript-eslint/no-implied-eval
    new Function("AudioWorkletProcessor", "registerProcessor", "sampleRate", "currentFrame", src)(
      g["AudioWorkletProcessor"],
      g["registerProcessor"],
      SR,
      0,
    );
  });
});

interface Harness {
  p: InstanceType<typeof Processor>;
  acks: { seq: number; status: string; detail: string; resultingSourceFrame?: number }[];
  send: (msg: Record<string, unknown>) => void;
  render: (blocks: number, quantum?: number) => Float32Array;
}

function harness(frames: number, fill: (i: number) => number): Harness {
  const p = new Processor({ processorOptions: { trackId: 0 } });
  const acks: Harness["acks"] = [];
  p.port.postMessage = (m: unknown) => acks.push(m as Harness["acks"][number]);
  const send = (msg: Record<string, unknown>) => p.port.onmessage?.({ data: msg });
  const pcm = new Float32Array(frames);
  for (let i = 0; i < frames; i++) pcm[i] = fill(i);
  send({
    type: "adopt",
    seq: 1,
    channels: [pcm.buffer],
    metadata: { frames, channels: 1, sampleRate: SR, durationS: frames / SR },
  });
  const render = (blocks: number, quantum = 128) => {
    const out = new Float32Array(blocks * quantum);
    for (let b = 0; b < blocks; b++) {
      const chan = [new Float32Array(quantum)];
      p.process([], [chan]);
      out.set(chan[0]!, b * quantum);
      (g["currentFrame"] as number);
      g["currentFrame"] = (g["currentFrame"] as number) + quantum;
    }
    return out;
  };
  return { p, acks, send, render };
}

describe("TapeProcessor — ownership and protocol", () => {
  it("acknowledges adopt with a ready status and owns the PCM", () => {
    const h = harness(1000, (i) => i / 1000);
    expect(h.acks[0]!.status).toBe("ready");
    expect(h.acks[0]!.detail).toContain("adopted 1ch × 1000 frames");
  });

  it("rejects structural commands before adopt", () => {
    const p = new Processor({ processorOptions: { trackId: 2 } });
    const acks: { status: string; detail: string }[] = [];
    p.port.postMessage = (m: unknown) => acks.push(m as { status: string; detail: string });
    p.port.onmessage?.({ data: { type: "setChop", seq: 9, division: 2, index: 0, applyAtContextFrame: 0 } });
    expect(acks[0]!.status).toBe("rejected");
    expect(acks[0]!.detail).toContain("before adopt");
  });
});

describe("TapeProcessor — render loop", () => {
  it("plays forward at exactly 1.0× and reproduces the source sample-for-sample", () => {
    const h = harness(4096, (i) => Math.sin((i * 2 * Math.PI * 100) / SR));
    h.send({ type: "start", seq: 2, applyAtContextFrame: 0, sourceFrame: 0 });
    const out = h.render(4, 128);
    for (let i = 1; i < 500; i++) {
      expect(Math.abs(out[i]! - Math.sin((i * 2 * Math.PI * 100) / SR))).toBeLessThan(1e-6);
    }
    expect(h.p.readPosition).toBeCloseTo(512, 6);
  });

  it("honours the output array length rather than assuming a 128-frame quantum", () => {
    g["currentFrame"] = 0;
    const h = harness(4096, (i) => i * 0.0001);
    h.send({ type: "start", seq: 2, applyAtContextFrame: 0, sourceFrame: 0 });
    h.render(1, 256);
    expect(h.p.readPosition).toBeCloseTo(256, 6);
  });

  it("moves the pointer backwards for reverse with no additional PCM", () => {
    g["currentFrame"] = 0;
    const h = harness(4096, (i) => i * 0.0001);
    h.send({ type: "start", seq: 2, applyAtContextFrame: 0, sourceFrame: 2000 });
    h.send({ type: "setDirection", seq: 3, direction: -1, applyAtContextFrame: 0 });
    h.render(1, 128);
    expect(h.p.readPosition).toBeCloseTo(2000 - 128, 6);
  });

  it("outputs silence outside a shorter track's source range", () => {
    g["currentFrame"] = 0;
    const h = harness(200, () => 0.5);
    h.send({ type: "start", seq: 2, applyAtContextFrame: 0, sourceFrame: 0 });
    const out = h.render(1, 512);
    expect(out[100]).toBeCloseTo(0.5, 5);
    expect(out[300]).toBe(0);
  });

  it("wraps inside the active loop and counts wraps", () => {
    g["currentFrame"] = 0;
    const h = harness(48000, (i) => Math.sin(i / 50));
    h.send({ type: "setWindow", seq: 2, start: 0, end: 4800, enabled: true, applyAtContextFrame: 0 });
    h.send({ type: "start", seq: 3, applyAtContextFrame: 0, sourceFrame: 0 });
    h.render(120, 128); // 15360 frames over a 4800-frame loop
    expect(h.p.wrapCount).toBeGreaterThanOrEqual(3);
    expect(h.p.readPosition).toBeGreaterThanOrEqual(0);
    expect(h.p.readPosition).toBeLessThan(4800);
  });

  it("chop division shortens the loop to windowWidth / div", () => {
    g["currentFrame"] = 0;
    const h = harness(48000, (i) => Math.sin(i / 50));
    h.send({ type: "setWindow", seq: 2, start: 0, end: 4800, enabled: true, applyAtContextFrame: 0 });
    h.send({ type: "setChop", seq: 3, division: 4, index: 1, applyAtContextFrame: 0 });
    h.send({ type: "start", seq: 4, applyAtContextFrame: 0, sourceFrame: 1200 });
    h.render(4, 128);
    expect(h.p.readPosition).toBeGreaterThanOrEqual(1200);
    expect(h.p.readPosition).toBeLessThan(2400);
  });

  it("applies a rate ramp inside the processor", () => {
    g["currentFrame"] = 0;
    const h = harness(48000, (i) => i * 1e-5);
    h.send({ type: "start", seq: 2, applyAtContextFrame: 0, sourceFrame: 0 });
    h.send({ type: "setRate", seq: 3, rate: 2, rampFrames: 128, applyAtContextFrame: 0 });
    h.render(2, 128);
    // 128 frames ramping 1→2 (≈192) then 128 frames at 2× (256).
    expect(h.p.readPosition).toBeGreaterThan(430);
    expect(h.p.readPosition).toBeLessThan(460);
  });

  it("stop mutes output without moving the pointer", () => {
    g["currentFrame"] = 0;
    const h = harness(4096, () => 0.7);
    h.send({ type: "start", seq: 2, applyAtContextFrame: 0, sourceFrame: 0 });
    h.render(1, 128);
    const pos = h.p.readPosition;
    h.send({ type: "stop", seq: 3, applyAtContextFrame: (g["currentFrame"] as number) });
    const out = h.render(1, 128);
    expect(out.every((v) => v === 0)).toBe(true);
    expect(h.p.readPosition).toBe(pos);
  });

  it("survives a forced failure by throwing exactly once (processorerror path)", () => {
    g["currentFrame"] = 0;
    const h = harness(4096, () => 0.1);
    h.send({ type: "start", seq: 2, applyAtContextFrame: 0, sourceFrame: 0 });
    h.send({ type: "__forceError", seq: 3, inFrames: 0 });
    expect(() => h.render(1, 128)).toThrow(/forced failure/);
  });
});

describe("Phase 5B gates", () => {
  it("gates a 199 MiB project at ~398 MiB and requires High Memory Mode on iOS", () => {
    const perTrack = Math.round((199 * MiB) / 4);
    const budget = { warnBytes: 192 * MiB, standardBlockBytes: 384 * MiB, highMemoryBlockBytes: 512 * MiB, platform: "ios" as const };
    const off = estimateMigration([perTrack, perTrack, perTrack, perTrack], budget, false);
    expect(off.worstCasePeakBytes / MiB).toBeCloseTo(398, 0);
    expect(off.allowed).toBe(false);
    expect(off.statement).toContain("refused");
    const on = estimateMigration([perTrack, perTrack, perTrack, perTrack], budget, true);
    expect(on.allowed).toBe(true);
  });

  it("refuses above the 512 MiB High Memory ceiling even with the mode on", () => {
    const budget = { warnBytes: 192 * MiB, standardBlockBytes: 384 * MiB, highMemoryBlockBytes: 512 * MiB, platform: "ios" as const };
    const big = estimateMigration([300 * MiB], budget, true);
    expect(big.allowed).toBe(false);
  });

  it("does not reduce the gate because migration is sequential", () => {
    const budget = { warnBytes: 192 * MiB, standardBlockBytes: 384 * MiB, highMemoryBlockBytes: 1024 * MiB, platform: "desktop" as const };
    const e = estimateMigration([100 * MiB, 100 * MiB], budget, false);
    expect(e.worstCasePeakBytes).toBe(400 * MiB);
    expect(e.optimisticPeakBytes).toBe(300 * MiB);
  });

  it("computes a shared apply frame and pairwise drift", () => {
    const ctx = { currentTime: 1, sampleRate: 48000 } as BaseAudioContext;
    expect(sharedApplyFrame(ctx, 0.1)).toBe(52800);
    const d = pairwiseDrift([1000, 1001, 1000, null]);
    expect(d.maxDrift).toBe(1);
    expect(d.pairs).toContain("T1↔T2: 1 frames");
  });
});
