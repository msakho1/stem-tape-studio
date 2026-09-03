/**
 * S3 correction — EXCLUSIVE WORKLET OWNERSHIP AFTER TAKEOVER.
 *
 * The reported defect was an echo / layered copy of the song under a scratch:
 * the worklet leg was summing with node voices that were still running on the
 * same stem gate. Two independent causes are proved fixed here, against the
 * real AudioEngine and the recording Web Audio mock:
 *
 *  1. migrateTrack faded only the NEWEST tracked source, so older seam / loop /
 *     release voices stayed live and audible under the worklet.
 *  2. beginMasterScratch could re-enter during its own asynchronous migration
 *     and build a SECOND WorkletTrack on the same stem gate; the first was
 *     never disposed and played the same PCM from a different position forever.
 */

import { afterEach, describe, expect, it } from "vitest";
import { AudioEngine } from "../engine";
import { MockCtx, MockGain, makeBuffer, SR } from "./mockAudio";

/** Minimal AudioWorkletNode that acks the migration handshake truthfully. */
class FakeWorkletNode {
  static instances: FakeWorkletNode[] = [];
  port = {
    onmessage: null as ((e: { data: unknown }) => void) | null,
    postMessage: (msg: Record<string, unknown>) => {
      const seq = msg["seq"] as number;
      const reply = (status: string, extra: Record<string, unknown> = {}) =>
        queueMicrotask(() => this.port.onmessage?.({ data: { seq, status, detail: String(msg["type"]), ...extra } }));
      switch (msg["type"]) {
        case "adopt":
          return reply("ready");
        case "prepareHandoff":
          return reply("ready", { resultingSourceFrame: msg["sourceFrame"] });
        case "start":
          return reply("applied", { resultingSourceFrame: msg["sourceFrame"] });
        default:
          return reply("applied", { masterFrame: 0, masterVelocity: 0 });
      }
    },
    close: () => {},
  };
  onprocessorerror: (() => void) | null = null;
  outs: unknown[] = [];
  constructor(_ctx: unknown, _name: string, _opts: unknown) {
    FakeWorkletNode.instances.push(this);
  }
  connect(d: unknown) {
    this.outs.push(d);
    return d as never;
  }
  disconnect() {
    this.outs = [];
  }
}

type G = Record<string, unknown>;

async function rig() {
  FakeWorkletNode.instances = [];
  const ctx = new MockCtx() as MockCtx & { audioWorklet: { addModule: () => Promise<void> } };
  ctx.audioWorklet = { addModule: async () => {} };
  const g = globalThis as unknown as G;
  g["window"] = { AudioContext: function () { return ctx; }, isSecureContext: true };
  g["AudioWorkletNode"] = FakeWorkletNode;
  const engine = new AudioEngine();
  await engine.unlock();
  for (let i = 0; i < 4; i++) {
    engine.adoptBuffer(i as 0 | 1 | 2 | 3, makeBuffer(2, SR * 16, SR, 0.3), {
      name: `stem ${i + 1}`,
      provenance: "bundled-demo",
    });
  }
  return { engine, ctx };
}

let cmdId = 0;
function cmd(type: string, payload: Record<string, number | string | boolean | null> = {}) {
  return { id: ++cmdId, t: 0, type, payload } as never;
}
function play(engine: AudioEngine) {
  engine.execute(cmd("transport.play"));
}

function priv(engine: AudioEngine) {
  return engine as unknown as {
    tracks: {
      stemGate: MockGain;
      sources: { node: { stoppedAt: number | null }; fade: MockGain }[];
      migrationStatus: string;
      engineMode: string;
    }[];
    pendingRelease: unknown[];
  };
}

afterEach(() => {
  const g = globalThis as unknown as G;
  delete g["AudioWorkletNode"];
});

describe("S3 — exclusive ownership after master-scratch takeover", () => {
  it("retires EVERY live node voice on the lane, not just the newest", async () => {
    const { engine } = await rig();
    const p = priv(engine);
    play(engine);
    // Force extra live voices on lane 0 the way a loop wrap / seam does: the
    // relocate path leaves the outgoing voice fading while a new one starts.
    engine.execute(cmd("transport.scrub", { position: 0.2 }));
    engine.execute(cmd("transport.scrub", { position: 0.4 }));
    const before = p.tracks[0]!.sources.map((s) => s.node);
    expect(before.length).toBeGreaterThan(0);

    const r = await engine.beginMasterScratch();
    expect(r.ok).toBe(true);

    // Every voice that existed before the takeover is scheduled to stop.
    for (const n of before) expect(n.stoppedAt).not.toBeNull();
    // And no lane keeps a live node voice under the worklet.
    for (const t of p.tracks) {
      expect(t.engineMode).toBe("worklet");
      for (const s of t.sources) expect(s.node.stoppedAt).not.toBeNull();
    }
    await engine.endMasterScratch();
  });

  it("drops the protected release target so the identity guard cannot spare a voice", async () => {
    const { engine } = await rig();
    const p = priv(engine);
    play(engine);
    // A protected release target would previously survive fadeOutAndStop.
    p.pendingRelease[0] = { live: p.tracks[0]!.sources[0], at: 0 };
    await engine.beginMasterScratch();
    expect(p.pendingRelease[0]).toBeNull();
    for (const s of p.tracks[0]!.sources) expect(s.node.stoppedAt).not.toBeNull();
    await engine.endMasterScratch();
  });

  it("never builds two worklet nodes for one lane, even on re-entrant entry", async () => {
    const { engine } = await rig();
    play(engine);
    const [a, b, c] = await Promise.all([
      engine.beginMasterScratch(),
      engine.beginMasterScratch(),
      engine.beginMasterScratch(),
    ]);
    expect(a.ok && b.ok && c.ok).toBe(true);
    // Exactly one processor per lane: an orphaned second node was the echo.
    expect(FakeWorkletNode.instances.length).toBe(4);
    await engine.endMasterScratch();
  });

  it("a release racing the entry still ends the scratch (no latched master head)", async () => {
    const { engine } = await rig();
    play(engine);
    const entering = engine.beginMasterScratch();
    const ending = engine.endMasterScratch();
    await entering;
    const r = await ending;
    expect(r.ok).toBe(true);
    expect(engine.masterScratch).toBeNull();
  });

  it("commanded velocity is applied while engaged and zero settles the tape", async () => {
    const { engine } = await rig();
    play(engine);
    await engine.beginMasterScratch();
    engine.setMasterScratchVelocity(1.8);
    expect(engine.masterScratch?.velocity).toBeCloseTo(1.8, 6);
    engine.setMasterScratchVelocity(-1.2);
    expect(engine.masterScratch?.velocity).toBeCloseTo(-1.2, 6);
    // Hand held still ⇒ the surface commands exactly 0 ⇒ stopped record.
    engine.setMasterScratchVelocity(0);
    expect(engine.masterScratch?.velocity).toBe(0);
    await engine.endMasterScratch();
  });
});
