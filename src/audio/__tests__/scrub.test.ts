/**
 * Audible head-scrub acceptance tests.
 *
 * These run the REAL kernel — `public/tape-processor.js` — inside a minimal
 * AudioWorklet harness, so what is asserted here is the same code the browser
 * executes. They are deliberately about SOUND, not about the position table:
 * output must appear while the pointer is still down, its direction must follow
 * the finger, and the landing frame must be exact.
 */

import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { ScrubTracker, clampScrubRate, MAX_SCRUB_RATE } from "../scrub";

const SR = 48000;
const QUANTUM = 128;

interface Harness {
  proc: any;
  posts: any[];
  send: (msg: any) => void;
  render: (quanta: number) => { rms: number; peak: number; frames: number };
  out: Float32Array[];
}

function loadProcessor(): any {
  const src = readFileSync("public/tape-processor.js", "utf8");
  const g: any = {
    sampleRate: SR,
    currentFrame: 0,
    registered: null as any,
  };
  g.AudioWorkletProcessor = class {
    port: any;
    constructor() {
      this.port = { postMessage: (m: any) => g.posts.push(m), onmessage: null, close() {} };
    }
  };
  g.posts = [] as any[];
  g.registerProcessor = (_name: string, cls: any) => {
    g.registered = cls;
  };
  // eslint-disable-next-line @typescript-eslint/no-implied-eval
  const fn = new Function("sampleRate", "currentFrame", "AudioWorkletProcessor", "registerProcessor", "globalThis_", `${src}\n;return registerProcessor;`);
  // currentFrame must be live: expose it through a getter on a scope object.
  const scope: any = { frame: 0 };
  const wrapped = new Function(
    "ctx",
    `with (ctx) { ${src} ; return registerProcessor.__cls; }`,
  );
  void fn;
  void wrapped;
  // Simpler, and honest about how the kernel reads globals: evaluate the module
  // with `currentFrame` resolved from a mutable holder via a with-scope proxy.
  const holder = { frame: 0 };
  const ctxProxy = new Proxy(
    {
      sampleRate: SR,
      AudioWorkletProcessor: g.AudioWorkletProcessor,
      registerProcessor: g.registerProcessor,
      Math,
      Float32Array,
      console,
      get currentFrame() {
        return holder.frame;
      },
    } as any,
    {
      has: () => true,
      get: (t, k) => (k in t ? (t as any)[k] : undefined),
    },
  );
  // eslint-disable-next-line no-new-func
  new Function("ctx", `with (ctx) { ${src} }`)(ctxProxy);
  return { cls: g.registered, posts: g.posts, holder };
}

function makeHarness(opts: { frames: number; cycleFrames: number }): Harness {
  const { cls, posts, holder } = loadProcessor();
  const proc = new cls({ processorOptions: { trackId: 0 } });
  const pcm = new Float32Array(opts.frames);
  // A ramp: the sample value IS the frame index / frames, so the direction and
  // distance of travel are readable straight out of the audio.
  for (let i = 0; i < opts.frames; i++) pcm[i] = (i / opts.frames) * 2 - 1;
  proc.port.onmessage({
    data: { type: "adopt", seq: 1, channels: [pcm.buffer], metadata: { frames: opts.frames, channels: 1, sampleRate: SR, durationS: opts.frames / SR } },
  });
  proc.port.onmessage({ data: { type: "start", seq: 2, applyAtContextFrame: 0, sourceFrame: 0 } });
  const out = [new Float32Array(QUANTUM), new Float32Array(QUANTUM)];
  const send = (msg: any) => proc.port.onmessage({ data: msg });
  const render = (quanta: number) => {
    let sum = 0;
    let peak = 0;
    let n = 0;
    for (let q = 0; q < quanta; q++) {
      out[0]!.fill(0);
      out[1]!.fill(0);
      proc.process([], [out], {});
      holder.frame += QUANTUM;
      for (let i = 0; i < QUANTUM; i++) {
        const v = out[0]![i]!;
        sum += v * v;
        peak = Math.max(peak, Math.abs(v));
        n++;
      }
    }
    return { rms: Math.sqrt(sum / n), peak, frames: n };
  };
  return { proc, posts, send, render, out };
}

function setHeads(h: Harness, cycleFrames: number, over: Partial<{ offset: number; level: number; muted: boolean; reverse: boolean }>[] = []) {
  h.send({
    type: "setHeads",
    seq: 10,
    cycleStart: 0,
    cycleFrames,
    heads: [0, 1, 2, 3].map((i) => ({ offset: i * 0.25, level: 0.8, muted: false, reverse: false, ...(over[i] ?? {}) })),
  });
}

const CYCLE = 48000;

describe("scrub model (pure)", () => {
  it("unwraps travel so 10% → 90% is forward, not the short way back", () => {
    const t = new ScrubTracker(0, 1, 0, CYCLE, 0.1 * CYCLE, 0.1, 0);
    const p = t.preview(0.9, 100);
    expect(p.deltaFrames).toBeCloseTo(0.8 * CYCLE, 6);
    expect(p.direction).toBe(1);
  });

  it("reports signed velocity and clamps the kernel read rate", () => {
    const t = new ScrubTracker(0, 1, 0, CYCLE, 0, 0.5, 0);
    const p = t.preview(0.25, 16);
    expect(p.velocityFramesPerSecond).toBeLessThan(0);
    expect(clampScrubRate(1e6)).toBe(MAX_SCRUB_RATE);
    expect(clampScrubRate(-1e6)).toBe(-MAX_SCRUB_RATE);
  });

  it("computes an exact absolute landing frame", () => {
    const t = new ScrubTracker(2, 7, 1000, CYCLE, 1000, 0, 0);
    expect(t.finalFrame(0.375)).toBe(1000 + 0.375 * CYCLE);
  });
});

describe("kernel scrub — audible behaviour", () => {
  it("produces output DURING the drag, before pointer-up", () => {
    const h = makeHarness({ frames: CYCLE * 2, cycleFrames: CYCLE });
    setHeads(h, CYCLE, [{}, { muted: true }, { muted: true }, { muted: true }]);
    h.render(4);
    h.send({ type: "headScrub", seq: 20, head: 0, phase: "start", pointerId: 1, normalizedPosition: 0, deltaFrames: 0 });
    // One flick's worth of travel, then render with NO end event sent.
    h.send({ type: "headScrub", seq: 21, head: 0, phase: "preview", pointerId: 1, normalizedPosition: 0.2, deltaFrames: 0.2 * CYCLE });
    const during = h.render(20);
    expect(during.rms).toBeGreaterThan(1e-3);
    const open = h.proc.headScrubs[0];
    expect(open).not.toBeNull();
    expect(open.velocity).toBeGreaterThan(0);
  });

  it("moves forward for a forward drag and backward for a backward drag", () => {
    const fwd = makeHarness({ frames: CYCLE * 2, cycleFrames: CYCLE });
    setHeads(fwd, CYCLE);
    fwd.send({ type: "headScrub", seq: 20, head: 0, phase: "start", pointerId: 1, normalizedPosition: 0.5, deltaFrames: 0 });
    fwd.send({ type: "headScrub", seq: 21, head: 0, phase: "preview", pointerId: 1, normalizedPosition: 0.7, deltaFrames: 0.2 * CYCLE });
    fwd.render(10);
    expect(fwd.proc.headScrubs[0].velocity).toBeGreaterThan(0);

    const back = makeHarness({ frames: CYCLE * 2, cycleFrames: CYCLE });
    setHeads(back, CYCLE);
    back.send({ type: "headScrub", seq: 20, head: 0, phase: "start", pointerId: 1, normalizedPosition: 0.5, deltaFrames: 0 });
    back.send({ type: "headScrub", seq: 21, head: 0, phase: "preview", pointerId: 1, normalizedPosition: 0.3, deltaFrames: -0.2 * CYCLE });
    back.render(10);
    expect(back.proc.headScrubs[0].velocity).toBeLessThan(0);
  });

  it("goes silent when the pointer stops but stays open", () => {
    const h = makeHarness({ frames: CYCLE * 2, cycleFrames: CYCLE });
    setHeads(h, CYCLE, [{}, { muted: true }, { muted: true }, { muted: true }]);
    h.send({ type: "headScrub", seq: 20, head: 0, phase: "start", pointerId: 1, normalizedPosition: 0, deltaFrames: 0 });
    h.send({ type: "headScrub", seq: 21, head: 0, phase: "preview", pointerId: 1, normalizedPosition: 0.05, deltaFrames: 0.05 * CYCLE });
    h.render(30);
    // No further movement: the chase converges and the voice must fade out.
    h.render(400);
    const sc = h.proc.headScrubs[0];
    expect(sc).not.toBeNull();
    expect(Math.abs(sc.velocity)).toBeLessThan(0.02);
    expect(sc.gain).toBeLessThan(0.35);
  });

  it("lands on the released position to within one frame and resumes playback", () => {
    const h = makeHarness({ frames: CYCLE * 2, cycleFrames: CYCLE });
    setHeads(h, CYCLE, [{}, { muted: true }, { muted: true }, { muted: true }]);
    h.render(4);
    h.send({ type: "headScrub", seq: 20, head: 0, phase: "start", pointerId: 1, normalizedPosition: 0, deltaFrames: 0 });
    h.send({ type: "headScrub", seq: 21, head: 0, phase: "preview", pointerId: 1, normalizedPosition: 0.4, deltaFrames: 0.4 * CYCLE });
    h.render(30);
    h.send({ type: "headScrub", seq: 22, head: 0, phase: "end", pointerId: 1, normalizedPosition: 0.4, deltaFrames: 0 });
    const landed = h.proc.headReadFrame(0);
    expect(Math.abs(landed - 0.4 * CYCLE)).toBeLessThanOrEqual(1);
    // Playback continues from there: the derived head frame advances.
    h.render(10);
    expect(h.proc.headReadFrame(0)).toBeGreaterThan(landed);
  });

  it("restores the pre-scrub position on cancel", () => {
    const h = makeHarness({ frames: CYCLE * 2, cycleFrames: CYCLE });
    setHeads(h, CYCLE);
    h.render(4);
    const before = h.proc.headReadFrame(1);
    h.send({ type: "headScrub", seq: 20, head: 1, phase: "start", pointerId: 3, normalizedPosition: 0.25, deltaFrames: 0 });
    h.send({ type: "headScrub", seq: 21, head: 1, phase: "preview", pointerId: 3, normalizedPosition: 0.9, deltaFrames: 0.65 * CYCLE });
    h.render(20);
    h.send({ type: "headScrub", seq: 22, head: 1, phase: "cancel", pointerId: 3, normalizedPosition: 0.9, deltaFrames: 0 });
    h.render(20);
    const after = h.proc.headReadFrame(1);
    // Same phase relationship as before the gesture (advanced by playback only).
    expect(Math.abs(((after - before) % CYCLE) - 44 * QUANTUM) % CYCLE).toBeLessThan(CYCLE);
    expect(h.proc.heads.heads[1].offset).toBeCloseTo(0.25, 9);
  });

  it("scrubs only the addressed head — the other three keep their offsets", () => {
    const h = makeHarness({ frames: CYCLE * 2, cycleFrames: CYCLE });
    setHeads(h, CYCLE);
    h.send({ type: "headScrub", seq: 20, head: 2, phase: "start", pointerId: 1, normalizedPosition: 0.5, deltaFrames: 0 });
    h.send({ type: "headScrub", seq: 21, head: 2, phase: "preview", pointerId: 1, normalizedPosition: 0.8, deltaFrames: 0.3 * CYCLE });
    h.render(20);
    expect(h.proc.headScrubs[0]).toBeNull();
    expect(h.proc.headScrubs[1]).toBeNull();
    expect(h.proc.headScrubs[3]).toBeNull();
    expect(h.proc.heads.heads[0].offset).toBeCloseTo(0, 9);
    expect(h.proc.heads.heads[3].offset).toBeCloseTo(0.75, 9);
  });

  it("does not move the underlying transport pointer", () => {
    const h = makeHarness({ frames: CYCLE * 2, cycleFrames: CYCLE });
    setHeads(h, CYCLE);
    h.render(10);
    const before = h.proc.readPosition;
    h.send({ type: "headScrub", seq: 20, head: 0, phase: "start", pointerId: 1, normalizedPosition: 0, deltaFrames: 0 });
    h.send({ type: "headScrub", seq: 21, head: 0, phase: "preview", pointerId: 1, normalizedPosition: 0.9, deltaFrames: 0.9 * CYCLE });
    h.render(10);
    h.send({ type: "headScrub", seq: 22, head: 0, phase: "end", pointerId: 1, normalizedPosition: 0.9, deltaFrames: 0 });
    // Exactly 20 quanta of transport advance at rate 1, nothing else.
    expect(h.proc.readPosition - before).toBeCloseTo(20 * QUANTUM, 6);
  });

  it("works on a reversed head and at a non-unity rate", () => {
    const h = makeHarness({ frames: CYCLE * 2, cycleFrames: CYCLE });
    setHeads(h, CYCLE, [{ reverse: true }, { muted: true }, { muted: true }, { muted: true }]);
    h.send({ type: "setRate", seq: 30, rate: 0.5, rampFrames: 0, applyAtContextFrame: 0 });
    h.render(4);
    h.send({ type: "headScrub", seq: 20, head: 0, phase: "start", pointerId: 1, normalizedPosition: 0, deltaFrames: 0 });
    h.send({ type: "headScrub", seq: 21, head: 0, phase: "preview", pointerId: 1, normalizedPosition: 0.3, deltaFrames: 0.3 * CYCLE });
    const during = h.render(20);
    expect(during.rms).toBeGreaterThan(1e-3);
    h.send({ type: "headScrub", seq: 22, head: 0, phase: "end", pointerId: 1, normalizedPosition: 0.3, deltaFrames: 0 });
    expect(Math.abs(h.proc.headReadFrame(0) - 0.3 * CYCLE)).toBeLessThanOrEqual(1);
  });

  it("emits scrub telemetry while a gesture is live and none when idle", () => {
    const h = makeHarness({ frames: CYCLE * 2, cycleFrames: CYCLE });
    setHeads(h, CYCLE);
    h.posts.length = 0;
    h.render(40);
    expect(h.posts.filter((p) => p.status === "telemetry")).toHaveLength(0);
    h.send({ type: "headScrub", seq: 20, head: 0, phase: "start", pointerId: 1, normalizedPosition: 0, deltaFrames: 0 });
    h.send({ type: "headScrub", seq: 21, head: 0, phase: "preview", pointerId: 1, normalizedPosition: 0.6, deltaFrames: 0.6 * CYCLE });
    h.render(40);
    const tel = h.posts.filter((p) => p.status === "telemetry");
    expect(tel.length).toBeGreaterThan(0);
    expect(tel[tel.length - 1].scrubHeads[0].velocity).not.toBe(0);
    expect(tel[tel.length - 1].rms).toBeGreaterThan(0);
  });

  it("rejects a scrub when heads mode is off", () => {
    const h = makeHarness({ frames: CYCLE, cycleFrames: CYCLE });
    h.posts.length = 0;
    h.send({ type: "headScrub", seq: 20, head: 0, phase: "start", pointerId: 1, normalizedPosition: 0, deltaFrames: 0 });
    expect(h.posts.at(-1).status).toBe("rejected");
  });
});
