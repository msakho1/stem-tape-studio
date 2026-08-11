/**
 * Defect 2 — a released loop rejoins at the HIDDEN song position.
 *
 * The claim under test is a schedule claim: at the scheduled bar boundary the
 * replacement source starts reading at the frame the shared integrated-rate
 * timeline has reached, NOT at the captured loop start and NOT at the loop
 * window. It is asserted through the recording mock's start(when, offset).
 */

import { describe, expect, it } from "vitest";
import { AudioEngine } from "../engine";
import { MockCtx, makeBuffer, SR } from "./mockAudio";
import type { SongGrid } from "../gridAnalysis";

let cmdId = 0;
function cmd(type: string, payload: Record<string, number | string | boolean | null> = {}) {
  return { id: ++cmdId, t: 0, type, payload } as never;
}

const GRID: SongGrid = {
  bpm: 120,
  beatsPerBar: 4,
  firstBeatS: 0,
  firstDownbeatS: 0,
  beatSeconds: 0.5,
  barSeconds: 2,
  analysisSampleRate: SR,
  analysisFrames: SR * 16,
  confidence: 1,
  method: "test",
} as unknown as SongGrid;

async function rig() {
  const ctx = new MockCtx();
  (globalThis as unknown as { window: unknown }).window = { AudioContext: function () { return ctx; } };
  const engine = new AudioEngine();
  await engine.unlock();
  for (let i = 0; i < 4; i++) {
    engine.adoptBuffer(i as 0 | 1 | 2 | 3, makeBuffer(1, SR * 32, SR, 0.25), {
      name: `stem ${i + 1}`,
      provenance: "bundled-demo",
    });
  }
  engine.songGrid = GRID;
  const tick = () => (engine as unknown as { tick: () => void }).tick();
  return { engine, ctx, advance: (s: number) => (ctx.currentTime += s), tick };
}

describe("bar-synced loop release", () => {
  it("does not move the shared timeline when a loop is captured or wraps", async () => {
    const { engine, ctx, advance, tick } = await rig();
    engine.execute(cmd("transport.play"));
    advance(3.0);
    const before = engine.status().position;
    engine.execute(cmd("loop.capture", { lane: 0, bars: 1 }));
    expect(engine.status().position).toBeCloseTo(before, 6);
    // Let the lane wrap several times; the hidden clock must keep running.
    for (let i = 0; i < 40; i++) {
      advance(0.25);
      tick();
    }
    expect(engine.status().position).toBeCloseTo(before + 10, 3);
    void ctx;
  });

  it("rejoins at the hidden song frame on the next bar, never at the loop start", async () => {
    const { engine, ctx, advance, tick } = await rig();
    engine.execute(cmd("transport.play"));
    advance(3.0);
    engine.execute(cmd("loop.capture", { lane: 0, bars: 1 }));
    const loopStart = engine.status().tracks[0]!.loop.start;
    for (let i = 0; i < 20; i++) {
      advance(0.25);
      tick();
    }

    const before = ctx.started.length;
    const ack = engine.execute(cmd("loop.release", { lane: 0 }));
    expect(ack.status).toBe("completed");

    const plan = (engine as unknown as { lastReleasePlan: { lane: number; boundaryPos: number; sourceFrame: number; at: number }[] })
      .lastReleasePlan.at(-1)!;
    // The landing is a real bar boundary of the analysed grid.
    expect(plan.boundaryPos % GRID.barSeconds).toBeCloseTo(0, 6);
    expect(plan.boundaryPos).toBeGreaterThan(engine.status().position);

    // The replacement source is scheduled at that boundary, reading the hidden
    // song frame — within 2 frames — and NOT the captured loop start.
    const spawned = ctx.started.slice(before);
    expect(spawned).toHaveLength(1);
    const s = spawned[0]!;
    expect(Math.abs(s.when - plan.at)).toBeLessThan(1e-6);
    expect(Math.abs(Math.round(s.offset * SR) - plan.sourceFrame)).toBeLessThanOrEqual(2);
    expect(Math.abs(s.offset - loopStart)).toBeGreaterThan(0.05);

    // After the boundary + fade the loop window is closed and one path plays.
    advance(plan.at - ctx.currentTime + 0.05);
    tick();
    expect(engine.status().tracks[0]!.loop.enabled).toBe(false);
  });

  it("releases cleanly while stopped without scheduling a source", async () => {
    const { engine, ctx, advance } = await rig();
    engine.setInertiaPreset("off"); // stop must be immediate for this case
    engine.execute(cmd("transport.play"));
    advance(1.0);
    engine.execute(cmd("loop.capture", { lane: 1, bars: 1 }));
    engine.execute(cmd("transport.stop"));
    advance(1.0);
    const before = ctx.started.length;
    const ack = engine.execute(cmd("loop.release", { lane: 1 }));
    expect(ack.status).toBe("completed");
    expect(ctx.started.length).toBe(before);
    expect(engine.status().tracks[1]!.loop.enabled).toBe(false);
  });
});
