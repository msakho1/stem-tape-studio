/**
 * Loop ENTRY must not seek. Engaging the global loop only establishes the
 * window: the four stems keep reading forward from the exact frame they were
 * on, and the first wrap happens only when playback naturally reaches loopEnd.
 * Releasing mid-cycle (before any wrap is committed) must not seek either.
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
  engine.setInertiaPreset("off");
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

describe("loop entry", () => {
  it("spawns nothing and moves no read pointer when the loop is engaged inside the window", async () => {
    const { engine, ctx, advance } = await rig();
    engine.execute(cmd("transport.play"));
    advance(3.0);
    const posBefore = engine.status().position;
    const started = ctx.started.length;

    expect(engine.execute(cmd("loop.global.start", { division: 1 })).status).toBe("completed");

    // No new AudioBufferSourceNode: nobody seeked back to the loop start.
    expect(ctx.started.length).toBe(started);
    expect(engine.status().position).toBeCloseTo(posBefore, 6);
    const win = engine.status().tracks[0]!.loop;
    expect(win.enabled).toBe(true);
    // Window is the bar the playhead is inside, and the playhead stayed put.
    expect(win.start * 32).toBeLessThanOrEqual(posBefore);
    expect(win.end * 32).toBeGreaterThan(posBefore);
  });

  it("wraps only when playback naturally reaches the loop end, all four stems together", async () => {
    const { engine, ctx, advance, tick } = await rig();
    engine.execute(cmd("transport.play"));
    advance(3.0);
    engine.execute(cmd("loop.global.start", { division: 1 }));
    const started = ctx.started.length;

    // 0.5 s short of the boundary (loop end = 4.0 s): still no wrap.
    for (let i = 0; i < 5; i++) {
      advance(0.1);
      tick();
    }
    expect(ctx.started.length).toBe(started);

    // Cross the boundary: exactly four wrap sources, all at the same context
    // time and all reading the same loop-start offset.
    for (let i = 0; i < 8; i++) {
      advance(0.1);
      tick();
    }
    const wraps = ctx.started.slice(started);
    expect(wraps).toHaveLength(4);
    const when = wraps[0]!.when;
    for (const w of wraps) {
      expect(Math.abs(w.when - when)).toBeLessThan(1e-9);
      expect(Math.abs(w.offset - wraps[0]!.offset)).toBeLessThan(1e-9);
    }
    expect(wraps[0]!.offset).toBeCloseTo(2.0, 6);
    expect(engine.startSpreadMs()).toBeLessThan(1e-6);
  });
});

describe("loop release", () => {
  it("released mid-cycle before any wrap: no seek, no respawn, position preserved", async () => {
    const { engine, ctx, advance, tick } = await rig();
    engine.execute(cmd("transport.play"));
    advance(3.0);
    engine.execute(cmd("loop.global.start", { division: 1 }));
    for (let i = 0; i < 3; i++) {
      advance(0.1);
      tick();
    }
    const posBefore = engine.status().position;
    const started = ctx.started.length;

    expect(engine.execute(cmd("loop.global.release")).status).toBe("completed");

    expect(ctx.started.length).toBe(started);
    expect(engine.status().position).toBeCloseTo(posBefore, 6);
    for (let i = 0; i < 4; i++) expect(engine.status().tracks[i]!.loop.enabled).toBe(false);

    // Continues forward from exactly there.
    advance(1.0);
    expect(engine.status().position).toBeCloseTo(posBefore + 1.0, 3);
  });

  it("rapid engage/release cycles leave one live voice per stem", async () => {
    const { engine, ctx, advance, tick } = await rig();
    engine.execute(cmd("transport.play"));
    advance(2.5);
    for (let n = 0; n < 6; n++) {
      engine.execute(cmd("loop.global.start", { division: 1 }));
      advance(0.05);
      tick();
      engine.execute(cmd("loop.global.release"));
      advance(0.05);
      tick();
    }
    const tracks = (engine as unknown as {
      tracks: { sources: { node: { stoppedAt: number | null } }[] }[];
    }).tracks;
    for (let i = 0; i < 4; i++) {
      const live = tracks[i]!.sources.filter((s) => s.node.stoppedAt == null || s.node.stoppedAt > ctx.currentTime);
      expect(live).toHaveLength(1);
      expect(engine.status().tracks[i]!.loop.enabled).toBe(false);
    }
  });
});

describe("authoritative loop phase", () => {
  it("stays inside the audible loop window across many cycles, wrapping 1 -> 0", async () => {
    const { engine, advance, tick } = await rig();
    engine.execute(cmd("transport.play"));
    advance(3.0);
    engine.execute(cmd("loop.global.start", { division: 1 }));
    let wraps = 0;
    let prev = engine.loopPhase()!;
    expect(prev).toBeGreaterThanOrEqual(0);
    for (let i = 0; i < 400; i++) {
      advance(0.02);
      tick();
      const p = engine.loopPhase()!;
      expect(p).toBeGreaterThanOrEqual(0);
      expect(p).toBeLessThan(1);
      if (p < prev) wraps++;
      prev = p;
    }
    // 8 s of a 2 s loop -> four wraps, no drift out of the window.
    expect(wraps).toBeGreaterThanOrEqual(3);
    expect(engine.loopPhase()).toBeLessThan(1);
  });
});
