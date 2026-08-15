/**
 * Global loop = the SONG loops. On release the shared timeline continues from
 * the exact audible frame — no hidden clock, no jump, one voice per stem.
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

describe("global loop release", () => {
  it("continues from the audible frame and leaves one voice per stem", async () => {
    const { engine, ctx, advance, tick } = await rig();
    engine.execute(cmd("transport.play"));
    advance(3.0);
    engine.execute(cmd("loop.global.start", { division: 1 }));
    const win = engine.status().tracks[0]!.loop;
    for (let i = 0; i < 30; i++) {
      advance(0.1);
      tick();
    }

    const before = ctx.started.length;
    const ack = engine.execute(cmd("loop.global.release"));
    expect(ack.status).toBe("completed");

    const rel = (engine as unknown as { lastGlobalRelease: { at: number; position: number; lanes: number } })
      .lastGlobalRelease;
    expect(rel.lanes).toBe(4);
    // Landing sits inside the loop window that was audible, not at its start.
    const dur = 32;
    expect(rel.position).toBeGreaterThan(win.start * dur - 1e-6);
    expect(rel.position).toBeLessThan(win.end * dur + 0.05);

    // Four replacement sources, each reading the shared audible frame.
    const spawned = ctx.started.slice(before);
    expect(spawned).toHaveLength(4);
    for (const s of spawned) {
      expect(Math.abs(s.when - rel.at)).toBeLessThan(1e-6);
      expect(Math.abs(Math.round(s.offset * SR) - Math.round(rel.position * SR))).toBeLessThanOrEqual(2);
    }

    // Shared timeline is anchored to that frame and keeps moving forward.
    advance(rel.at - ctx.currentTime + 0.05);
    tick();
    expect(engine.status().position).toBeCloseTo(rel.position + 0.05, 3);
    advance(1.0);
    expect(engine.status().position).toBeCloseTo(rel.position + 1.05, 3);

    // Loops closed, exactly one live path per stem.
    const tracks = (engine as unknown as {
      tracks: { sources: { node: { stoppedAt: number | null } }[] }[];
    }).tracks;
    for (let i = 0; i < 4; i++) {
      expect(engine.status().tracks[i]!.loop.enabled).toBe(false);
      const live = tracks[i]!.sources.filter(
        (s) => s.node.stoppedAt == null || s.node.stoppedAt > ctx.currentTime,
      );
      expect(live).toHaveLength(1);
    }
  });

  it("releases cleanly while stopped without scheduling sources", async () => {
    const { engine, ctx, advance } = await rig();
    engine.setInertiaPreset("off");
    engine.execute(cmd("transport.play"));
    advance(1.0);
    engine.execute(cmd("loop.global.start", { division: 1 }));
    engine.execute(cmd("transport.stop"));
    advance(0.5);
    const before = ctx.started.length;
    expect(engine.execute(cmd("loop.global.release")).status).toBe("completed");
    expect(ctx.started.length).toBe(before);
    expect(engine.status().tracks[0]!.loop.enabled).toBe(false);
  });
});
