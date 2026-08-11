/**
 * Defect 1 — Heads Mode path isolation.
 *
 * Asserted against the recording Web Audio mock, so the claims are about the
 * GRAPH and the SCHEDULE, not about reducer flags:
 *
 *   - exactly two edges reach the master: normalBus→normalTap and
 *     headsBus→headsTap;
 *   - every stem's audible chain terminates on the normal bus, never on master;
 *   - while heads is active the normal bus gate is EXACTLY 0 after the fade,
 *     while its stem sources are still running (the hidden song timeline must
 *     keep advancing);
 *   - exiting reopens the normal bus and closes the heads bus.
 */

import { describe, expect, it } from "vitest";
import { AudioEngine } from "../engine";
import { MockCtx, MockGain, MockNode, makeBuffer, SR } from "./mockAudio";

let cmdId = 0;
function cmd(type: string, payload: Record<string, number | string | boolean | null> = {}) {
  return { id: ++cmdId, t: 0, type, payload } as never;
}

interface Rig {
  engine: AudioEngine;
  ctx: MockCtx;
  advance: (s: number) => void;
}

async function rig(): Promise<Rig> {
  const ctx = new MockCtx();
  (globalThis as unknown as { window: unknown }).window = { AudioContext: function () { return ctx; } };
  const engine = new AudioEngine();
  await engine.unlock();
  for (let i = 0; i < 4; i++) {
    engine.adoptBuffer(i as 0 | 1 | 2 | 3, makeBuffer(1, SR * 16, SR, 0.25), {
      name: `stem ${i + 1}`,
      provenance: "bundled-demo",
    });
  }
  return { engine, ctx, advance: (s: number) => (ctx.currentTime += s) };
}

function priv(engine: AudioEngine) {
  return engine as unknown as {
    master: MockGain;
    normalBus: MockGain;
    headsBus: MockGain;
    normalTap: MockNode;
    headsTap: MockNode;
    tracks: { analyser: MockNode; sources: unknown[] }[];
  };
}

describe("heads / normal bus isolation", () => {
  it("routes every stem through the normal bus and nothing else into master", async () => {
    const { engine } = await rig();
    const p = priv(engine);
    // Only the two taps feed the master.
    const intoMaster = [p.normalTap, p.headsTap].filter((n) => n.outs.includes(p.master));
    expect(intoMaster).toHaveLength(2);
    expect(p.normalBus.outs).toEqual([p.normalTap]);
    expect(p.headsBus.outs).toEqual([p.headsTap]);
    for (const t of p.tracks) {
      expect(t.analyser.outs).toContain(p.normalBus);
      expect(t.analyser.outs).not.toContain(p.master);
    }
  });

  it("closes the normal bus to exactly zero while heads is active, and reopens on exit", async () => {
    const { engine, ctx, advance } = await rig();
    const p = priv(engine);
    engine.execute(cmd("transport.play"));
    advance(1.0);

    const posBefore = engine.status().position;
    const r = engine.enterHeadsMode();
    expect(r.ok).toBe(true);

    const settled = ctx.currentTime + 0.06;
    expect(p.normalBus.gain.at(settled)).toBeCloseTo(0, 6);
    expect(p.headsBus.gain.at(settled)).toBeCloseTo(1, 6);

    // The stem sources are STILL RUNNING — silence comes from the gate only,
    // so the hidden song timeline keeps advancing underneath heads.
    expect(p.tracks.every((t) => t.sources.length > 0)).toBe(true);
    advance(2.0);
    expect(engine.status().position).toBeGreaterThan(posBefore + 1.9);

    engine.exitHeadsMode();
    const after = ctx.currentTime + 0.06;
    expect(p.normalBus.gain.at(after)).toBeCloseTo(1, 6);
    expect(p.headsBus.gain.at(after)).toBeCloseTo(0, 6);
  });
});
