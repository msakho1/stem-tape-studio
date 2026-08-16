/**
 * Heads Mode path isolation — Vocal-only model.
 *
 * Asserted against the recording Web Audio mock, so the claims are about the
 * GRAPH and the SCHEDULE, not about reducer flags:
 *
 *   - every stem's audible chain terminates on the normal bus, never on master;
 *   - the Head sum re-enters the graph at the VOCAL LANE input, so Heads obey
 *     the Vocal stem's mute, solo, fader and FX chain;
 *   - entering Heads closes the dry Vocal stem gate to 0 and opens the Head
 *     injection to 1, while the normal bus itself stays wide open so Drums,
 *     Bass and Instrument keep sounding and the song timeline keeps advancing;
 *   - exiting restores the dry Vocal and closes the injection.
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
    headsLimiter: MockNode;
    headsInject: MockGain;
    tracks: { analyser: MockNode; input: MockNode; stemGate: MockGain; sources: unknown[] }[];
  };
}

describe("heads / normal bus isolation", () => {
  it("keeps every stem on the normal bus and injects the Head sum at the Vocal lane", async () => {
    const { engine } = await rig();
    const p = priv(engine);
    // Only the normal tap reaches the master; the heads tap is a lane-level
    // send, not a second path to the output.
    expect(p.normalTap.outs).toContain(p.master);
    expect(p.headsTap.outs).not.toContain(p.master);
    expect(p.normalBus.outs).toEqual([p.normalTap]);
    expect(p.headsBus.outs).toEqual([p.headsTap]);
    // Head sum -> soft limiter -> injection gate -> VOCAL lane input.
    expect(p.headsTap.outs).toContain(p.headsLimiter);
    expect(p.headsLimiter.outs).toContain(p.headsInject);
    expect(p.headsInject.outs).toContain(p.tracks[0]!.input);
    expect(p.headsInject.gain.value).toBe(0);
    for (const t of p.tracks) {
      expect(t.analyser.outs).toContain(p.normalBus);
      expect(t.analyser.outs).not.toContain(p.master);
    }
  });

  it("replaces only the Vocal stem while heads is active, and restores it on exit", async () => {
    const { engine, ctx, advance } = await rig();
    const p = priv(engine);
    engine.execute(cmd("transport.play"));
    advance(1.0);

    const posBefore = engine.status().position;
    const r = engine.enterHeadsMode();
    expect(r.ok).toBe(true);

    const settled = ctx.currentTime + 0.06;
    // Vocal is replaced, not stacked: dry gate closed, injection open.
    expect(p.tracks[0]!.stemGate.gain.at(settled)).toBeCloseTo(0, 6);
    expect(p.headsInject.gain.at(settled)).toBeCloseTo(1, 6);
    // The other three stems are untouched, and the normal bus is never gated.
    expect(p.normalBus.gain.at(settled)).toBeCloseTo(1, 6);
    for (let i = 1; i < 4; i++) expect(p.tracks[i]!.stemGate.gain.at(settled)).toBeCloseTo(1, 6);

    // The stem sources are STILL RUNNING — the hidden song timeline keeps
    // advancing underneath heads.
    expect(p.tracks.every((t) => t.sources.length > 0)).toBe(true);
    advance(2.0);
    expect(engine.status().position).toBeGreaterThan(posBefore + 1.9);

    engine.exitHeadsMode();
    const after = ctx.currentTime + 0.06;
    expect(p.tracks[0]!.stemGate.gain.at(after)).toBeCloseTo(1, 6);
    expect(p.headsInject.gain.at(after)).toBeCloseTo(0, 6);
  });
});
