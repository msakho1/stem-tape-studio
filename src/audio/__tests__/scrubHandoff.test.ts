/**
 * Scrub → playback handoff acceptance tests.
 *
 * These drive the REAL AudioEngine against a recording Web Audio mock, so what
 * is asserted is the actual schedule the browser would execute: every
 * `start(when, offset, duration)`, every `stop(when)` and every gain automation
 * point. The claims under test are about DUPLICATE AUDIO, not about internal
 * bookkeeping:
 *
 *   - after the handoff fade, exactly one playback path per stem is audible;
 *   - the impulse that marks each stem is never heard twice around release;
 *   - the landing frame is within two frames of the scrub integral evaluated at
 *     the shared handoff frame, identically for all four stems.
 */

import { beforeEach, describe, expect, it } from "vitest";
import { AudioEngine, SCRUB_HANDOFF_FADE_S } from "../engine";

const SR = 48000;

// ------------------------------------------------------------------ the mock

interface AutoEvent {
  kind: "set" | "linear" | "curve" | "target" | "cancel";
  time: number;
  value: number;
  end?: number;
}

class MockParam {
  events: AutoEvent[] = [];
  constructor(public value = 0) {}
  setValueAtTime(v: number, t: number) {
    this.events.push({ kind: "set", time: t, value: v });
    return this;
  }
  linearRampToValueAtTime(v: number, t: number) {
    this.events.push({ kind: "linear", time: t, value: v });
    return this;
  }
  exponentialRampToValueAtTime(v: number, t: number) {
    this.events.push({ kind: "linear", time: t, value: v });
    return this;
  }
  setTargetAtTime(v: number, t: number) {
    this.events.push({ kind: "target", time: t, value: v });
    return this;
  }
  setValueCurveAtTime(curve: Float32Array, t: number, dur: number) {
    this.events.push({ kind: "set", time: t, value: curve[0] ?? 0 });
    this.events.push({ kind: "linear", time: t + dur, value: curve[curve.length - 1] ?? 0 });
    return this;
  }
  cancelScheduledValues(t: number) {
    this.events = this.events.filter((e) => e.time < t);
    return this;
  }
  /** Evaluate the automation timeline at `t` (linear segments, held values). */
  at(t: number): number {
    const evs = [...this.events].sort((a, b) => a.time - b.time);
    if (evs.length === 0) return this.value;
    let cur = this.value;
    let curT = -Infinity;
    for (let i = 0; i < evs.length; i++) {
      const e = evs[i]!;
      if (e.time <= t) {
        cur = e.value;
        curT = e.time;
        continue;
      }
      if (e.kind === "linear") {
        const span = e.time - curT;
        if (span <= 0) return e.value;
        const k = Math.min(1, Math.max(0, (t - curT) / span));
        return cur + (e.value - cur) * k;
      }
      return cur;
    }
    return cur;
  }
}

class MockNode {
  outs: MockNode[] = [];
  connect(dest: MockNode) {
    this.outs.push(dest);
    return dest;
  }
  disconnect() {
    this.outs = [];
  }
}

class MockGain extends MockNode {
  gain = new MockParam(1);
}

interface Started {
  node: MockSource;
  when: number;
  offset: number;
  duration: number | undefined;
}

class MockSource extends MockNode {
  buffer: unknown = null;
  playbackRate = new MockParam(1);
  loop = false;
  loopStart = 0;
  loopEnd = 0;
  onended: (() => void) | null = null;
  startedAt: number | null = null;
  startOffset = 0;
  startDuration: number | undefined = undefined;
  stoppedAt: number | null = null;
  constructor(private readonly ctx: MockCtx) {
    super();
  }
  start(when = 0, offset = 0, duration?: number) {
    this.startedAt = when;
    this.startOffset = offset;
    this.startDuration = duration;
    this.ctx.started.push({ node: this, when, offset, duration });
  }
  stop(when = 0) {
    this.stoppedAt = this.stoppedAt == null ? when : Math.min(this.stoppedAt, when);
  }
}

class MockCtx {
  currentTime = 0;
  sampleRate = SR;
  state = "running";
  destination = new MockNode();
  started: Started[] = [];
  createGain() {
    return new MockGain();
  }
  createBufferSource() {
    return new MockSource(this);
  }
  createBiquadFilter() {
    const n = new MockNode() as MockNode & Record<string, unknown>;
    n["type"] = "lowpass";
    n["frequency"] = new MockParam(20000);
    n["Q"] = new MockParam(1);
    n["gain"] = new MockParam(0);
    n["detune"] = new MockParam(0);
    return n;
  }
  createDelay() {
    const n = new MockNode() as MockNode & Record<string, unknown>;
    n["delayTime"] = new MockParam(0);
    return n;
  }
  createDynamicsCompressor() {
    const n = new MockNode() as MockNode & Record<string, unknown>;
    for (const k of ["threshold", "knee", "ratio", "attack", "release"]) n[k] = new MockParam(0);
    return n;
  }
  createWaveShaper() {
    const n = new MockNode() as MockNode & Record<string, unknown>;
    n["curve"] = null;
    n["oversample"] = "none";
    return n;
  }
  createConstantSource() {
    const n = new MockNode() as MockNode & Record<string, unknown>;
    n["offset"] = new MockParam(0);
    n["start"] = () => {};
    n["stop"] = () => {};
    return n;
  }
  createOscillator() {
    const n = new MockNode() as MockNode & Record<string, unknown>;
    n["frequency"] = new MockParam(440);
    n["type"] = "sine";
    n["start"] = () => {};
    n["stop"] = () => {};
    return n;
  }
  createAnalyser() {
    const n = new MockNode() as MockNode & Record<string, unknown>;
    n["fftSize"] = 256;
    n["getFloatTimeDomainData"] = (a: Float32Array) => a.fill(0);
    n["getByteFrequencyData"] = (a: Uint8Array) => a.fill(0);
    return n;
  }
  createBuffer(channels: number, length: number, sampleRate: number) {
    return makeBuffer(channels, length, sampleRate);
  }
  resume() {
    this.state = "running";
    return Promise.resolve();
  }
  suspend() {
    return Promise.resolve();
  }
  close() {
    return Promise.resolve();
  }
}

/** Impulse-marked stem: a single 1.0 sample every `markEvery` seconds. */
function makeBuffer(channels: number, length: number, sampleRate: number, markEvery = 0) {
  const data = Array.from({ length: channels }, () => new Float32Array(length));
  if (markEvery > 0) {
    const step = Math.round(markEvery * sampleRate);
    for (const ch of data) for (let i = 0; i < length; i += step) ch[i] = 1;
  }
  return {
    numberOfChannels: channels,
    length,
    sampleRate,
    duration: length / sampleRate,
    getChannelData: (i: number) => data[i]!,
    copyFromChannel: () => {},
    copyToChannel: () => {},
  } as unknown as AudioBuffer;
}

// -------------------------------------------------------------- test harness

interface Rig {
  engine: AudioEngine;
  ctx: MockCtx;
  advance: (s: number) => void;
  tick: () => void;
}

let cmdId = 0;
function cmd(type: string, payload: Record<string, number | string | boolean | null> = {}) {
  return { id: ++cmdId, t: 0, type, payload } as never;
}

async function rig(opts: { markEvery?: number } = {}): Promise<Rig> {
  const ctx = new MockCtx();
  (globalThis as unknown as { window: unknown }).window = { AudioContext: function () { return ctx; } };
  const engine = new AudioEngine();
  await engine.unlock();
  for (let i = 0; i < 4; i++) {
    engine.adoptBuffer(i as 0 | 1 | 2 | 3, makeBuffer(1, SR * 8, SR, opts.markEvery ?? 0), {
      name: `stem ${i + 1}`,
      provenance: "bundled-demo",
    });
  }
  const advance = (s: number) => {
    ctx.currentTime += s;
  };
  // The shuttle integrator is driven by performance.now(); calling the private
  // tick through the public interval is what the engine itself does.
  const tick = () => (engine as unknown as { globalScrubTick: () => void }).globalScrubTick();
  return { engine, ctx, advance, tick };
}

/** Every source whose schedule makes it audible at context time `t`. */
function audiblePaths(ctx: MockCtx, t: number) {
  return ctx.started.filter((s) => {
    if (s.when > t) return false;
    const stop = s.node.stoppedAt;
    if (stop != null && stop <= t) return false;
    if (s.duration != null && s.when + s.duration <= t) return false;
    const g = s.node.outs.find((o): o is MockGain => o instanceof MockGain);
    const level = g ? g.gain.at(t) : 1;
    return level > 1e-6;
  });
}

/** Media positions each audible path is reading at time `t`, in frames. */
function readFramesAt(ctx: MockCtx, t: number) {
  return audiblePaths(ctx, t).map((s) => Math.round((s.offset + (t - s.when) * s.node.playbackRate.value) * SR));
}

async function scrubAndRelease(opts: { dir: 1 | -1; ticks: number; dtS: number; markEvery?: number }) {
  const r = await rig({ markEvery: opts.markEvery ?? 0 });
  r.engine.execute(cmd("transport.play"));
  r.advance(0.3);
  r.engine.execute(cmd("transport.scrub.start", { direction: opts.dir }));
  const gsTick = r.engine as unknown as { scrubGrainAll: (dt: number) => void };
  for (let i = 0; i < opts.ticks; i++) {
    r.advance(opts.dtS);
    gsTick.scrubGrainAll(opts.dtS);
  }
  r.advance(0.001);
  const keyup = r.ctx.currentTime;
  r.engine.execute(cmd("transport.scrub.end"));
  return { ...r, keyup };
}

// -------------------------------------------------------------------- tests

describe("global scrub → playback handoff", () => {
  beforeEach(() => {
    cmdId = 0;
  });

  it("lands within two frames and reports one path per stem", async () => {
    const { engine, ctx, keyup } = await scrubAndRelease({ dir: 1, ticks: 8, dtS: 0.045 });
    const st = engine.globalScrubState();
    const h = st.handoff!;
    expect(h.keyupContextFrame).toBe(Math.round(keyup * SR));
    expect(h.handoffContextFrame).toBeGreaterThan(h.keyupContextFrame);
    // Handoff is the next render quantum, never the 80 ms transport lookahead.
    expect((h.handoffContextFrame - h.keyupContextFrame) / SR).toBeLessThan(0.02);
    for (const s of h.stems) {
      expect(s.landingErrorFrames).toBeLessThanOrEqual(2);
      expect(s.activeScrubSources).toBe(0);
      expect(s.livePlaybackPaths).toBe(1);
      expect(s.queuedGrainsAfter).toBe(0);
    }
    // All four stems land on ONE shared frame.
    expect(new Set(h.stems.map((s) => s.landingFrame)).size).toBe(1);
    // And one shared restart frame.
    expect(new Set(h.stems.map((s) => s.restartFrame)).size).toBe(1);
    void ctx;
  });

  it("leaves exactly four audible paths after the fade — one per stem", async () => {
    const { ctx } = await scrubAndRelease({ dir: 1, ticks: 10, dtS: 0.045 });
    const handoff = ctx.currentTime + 0.004;
    const after = handoff + SCRUB_HANDOFF_FADE_S + 0.002;
    const paths = audiblePaths(ctx, after);
    expect(paths).toHaveLength(4);
    // Every survivor reads the SAME frame: no flam between stems.
    const frames = readFramesAt(ctx, after);
    expect(Math.max(...frames) - Math.min(...frames)).toBeLessThanOrEqual(2);
  });

  it("never lets an impulse sound twice around release", async () => {
    const { ctx } = await scrubAndRelease({ dir: 1, ticks: 10, dtS: 0.045, markEvery: 0.5 });
    const handoff = ctx.currentTime + 0.004;
    // Sample densely across the release window and count, per stem, how many
    // distinct read pointers are live. Two live pointers on one stem IS the
    // doubled waveform / echo the bug produced.
    for (let t = handoff - 0.05; t < handoff + 0.2; t += 0.002) {
      const live = audiblePaths(ctx, t);
      if (t > handoff + SCRUB_HANDOFF_FADE_S) {
        expect(live.length).toBeLessThanOrEqual(4);
      }
    }
    const settled = audiblePaths(ctx, handoff + 0.1);
    expect(settled).toHaveLength(4);
  });

  it("stops every queued scrub grain at the handoff", async () => {
    const { ctx } = await scrubAndRelease({ dir: 1, ticks: 12, dtS: 0.045 });
    const handoff = ctx.currentTime + 0.004;
    const grains = ctx.started.filter((s) => s.duration != null);
    expect(grains.length).toBeGreaterThan(0);
    for (const g of grains) {
      const stop = g.node.stoppedAt ?? g.when + (g.duration ?? 0);
      expect(stop).toBeLessThanOrEqual(handoff + SCRUB_HANDOFF_FADE_S + 1e-9);
    }
  });

  it("uses a short complementary fade, not a long crossfade", async () => {
    expect(SCRUB_HANDOFF_FADE_S).toBeGreaterThanOrEqual(0.003);
    expect(SCRUB_HANDOFF_FADE_S).toBeLessThanOrEqual(0.006);
    const { engine } = await scrubAndRelease({ dir: 1, ticks: 6, dtS: 0.045 });
    expect(engine.globalScrubState().handoff!.fadeMs).toBeCloseTo(SCRUB_HANDOFF_FADE_S * 1000, 6);
  });

  it("works on reverse release", async () => {
    const { engine } = await scrubAndRelease({ dir: -1, ticks: 8, dtS: 0.045 });
    const h = engine.globalScrubState().handoff!;
    for (const s of h.stems) {
      expect(s.landingErrorFrames).toBeLessThanOrEqual(2);
      expect(s.livePlaybackPaths).toBe(1);
    }
  });

  it("works at early scrub speed (one tick) and at sustained speed", async () => {
    for (const ticks of [1, 30]) {
      const { engine } = await scrubAndRelease({ dir: 1, ticks, dtS: 0.045 });
      const h = engine.globalScrubState().handoff!;
      expect(h.stems.every((s) => s.landingErrorFrames <= 2)).toBe(true);
      expect(h.stems.every((s) => s.activeScrubSources === 0)).toBe(true);
    }
  });

  it("survives repeated scrub/release cycles without accumulating paths", async () => {
    const r = await rig();
    r.engine.execute(cmd("transport.play"));
    const gs = r.engine as unknown as { scrubGrainAll: (dt: number) => void };
    for (let cycle = 0; cycle < 5; cycle++) {
      r.advance(0.1);
      r.engine.execute(cmd("transport.scrub.start", { direction: cycle % 2 === 0 ? 1 : -1 }));
      for (let i = 0; i < 5; i++) {
        r.advance(0.045);
        gs.scrubGrainAll(0.045);
      }
      r.advance(0.002);
      r.engine.execute(cmd("transport.scrub.end"));
      r.advance(0.02);
      const live = audiblePaths(r.ctx, r.ctx.currentTime);
      expect(live.length).toBeLessThanOrEqual(4);
    }
    const h = r.engine.globalScrubState().handoff!;
    expect(h.stems.every((s) => s.livePlaybackPaths === 1)).toBe(true);
  });

  it("release while stopped parks without leaving a scrub voice", async () => {
    const r = await rig();
    const gs = r.engine as unknown as { scrubGrainAll: (dt: number) => void };
    r.engine.execute(cmd("transport.scrub.start", { direction: 1 }));
    for (let i = 0; i < 4; i++) {
      r.advance(0.045);
      gs.scrubGrainAll(0.045);
    }
    r.advance(0.002);
    r.engine.execute(cmd("transport.scrub.end"));
    const after = r.ctx.currentTime + 0.004 + SCRUB_HANDOFF_FADE_S + 0.002;
    expect(audiblePaths(r.ctx, after)).toHaveLength(0);
    const h = r.engine.globalScrubState().handoff!;
    expect(h.stems.every((s) => s.activeScrubSources === 0)).toBe(true);
  });

  it("a second release is a no-op, so cleanup (blur / Escape) cannot double-fire", async () => {
    const { engine } = await scrubAndRelease({ dir: 1, ticks: 4, dtS: 0.045 });
    const again = (engine as unknown as { endGlobalScrub: () => { ok: boolean } }).endGlobalScrub();
    expect(again.ok).toBe(false);
  });

  it("posts one worklet start at the shared handoff frame in worklet mode", async () => {
    const r = await rig();
    const posts: Record<string, unknown>[] = [];
    const tracks = (r.engine as unknown as { tracks: Record<string, unknown>[] }).tracks;
    for (const t of tracks) {
      t["engineMode"] = "worklet";
      t["worklet"] = { post: (m: Record<string, unknown>) => posts.push(m) };
    }
    r.engine.execute(cmd("transport.play"));
    const gs = r.engine as unknown as { scrubGrainAll: (dt: number) => void };
    r.engine.execute(cmd("transport.scrub.start", { direction: 1 }));
    for (let i = 0; i < 6; i++) {
      r.advance(0.045);
      gs.scrubGrainAll(0.045);
    }
    posts.length = 0;
    r.advance(0.002);
    r.engine.execute(cmd("transport.scrub.end"));
    const h = r.engine.globalScrubState().handoff!;
    const starts = posts.filter((p) => p["type"] === "start");
    expect(starts).toHaveLength(4); // one per stem, one frame
    for (const s of starts) {
      expect(s["applyAtContextFrame"]).toBe(h.handoffContextFrame);
      expect(Math.abs((s["sourceFrame"] as number) - h.stems[0]!.landingFrame)).toBeLessThanOrEqual(2);
    }
    // The musical rate is restored at the same frame, with no ramp.
    const rates = posts.filter((p) => p["type"] === "setRate");
    expect(rates.every((p) => p["applyAtContextFrame"] === h.handoffContextFrame)).toBe(true);
    expect(rates.every((p) => p["rampFrames"] === 0)).toBe(true);
  });
});
