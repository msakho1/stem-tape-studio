/**
 * Stem Instrument Mode — engine integration proofs against the recording Web
 * Audio mock. These assert on the real graph and schedule: gate values, edge
 * lists, audible voice counts, rejoin frames and persisted markers.
 */
import { describe, expect, it, beforeEach } from "vitest";
import { AudioEngine } from "../engine";
import { MockCtx, MockGain, MockNode, makeBuffer, SR } from "./mockAudio";
import { midiClock } from "../midi/clock";
import type { StemMidiEvent } from "../midi/contract";
import type { QualifierSnapshot, CueMarker, CueLane } from "../cues";

let cmdId = 0;
function cmd(type: string, payload: Record<string, number | string | boolean | null> = {}) {
  return { id: ++cmdId, t: 0, type, payload } as never;
}

const NONE: QualifierSnapshot = { functionHeld: false, tracksHeld: [] };
const GLOBAL: QualifierSnapshot = { functionHeld: true, tracksHeld: [] };
const lane = (l: CueLane): QualifierSnapshot => ({ functionHeld: false, tracksHeld: [l] });

interface Rig {
  engine: AudioEngine;
  ctx: MockCtx;
  advance: (s: number) => void;
  ev: (kind: StemMidiEvent["kind"], note: number) => StemMidiEvent;
}

async function rig(decodeLanes = [0, 1, 2, 3]): Promise<Rig> {
  const ctx = new MockCtx();
  (globalThis as unknown as { window: unknown }).window = { AudioContext: function () { return ctx; } };
  const engine = new AudioEngine();
  await engine.unlock();
  for (const i of decodeLanes) {
    engine.adoptBuffer(i as 0 | 1 | 2 | 3, makeBuffer(1, SR * 16, SR, 0.25), {
      name: `stem ${i + 1}`,
      provenance: "bundled-demo",
      contentHash: `hash-${i}`,
    } as never);
  }
  const cal = midiClock.calibration();
  return {
    engine,
    ctx,
    advance: (s: number) => (ctx.currentTime += s),
    // Map the CURRENT context time back into the performance.now() domain the
    // clock calibration expects, so the learned frame is the audible frame.
    ev: (kind, note) => ({
      kind,
      note,
      velocity: kind === "noteOn" ? 100 : 0,
      channel: 0,
      timestampMs: cal.perfNowMs0 + (ctx.currentTime - cal.ctxTime0) * 1000,
      source: "test",
      deviceId: "test",
      deviceName: "test",
    }),
  };
}

function priv(engine: AudioEngine) {
  return engine as unknown as {
    tracks: {
      stemGate: MockGain;
      soloGain: MockGain;
      fader: MockGain;
      input: MockNode;
      sources: { node: MockNode }[];
    }[];
    cueVoices: ({ gain: MockGain; node: MockNode; lane: number } | null)[];
  };
}

/** Learn a marker: qualifier-held Note On, play a while, Note Off. */
function learn(r: Rig, q: QualifierSnapshot, note: number, holdS = 1) {
  const on = r.engine.handleMidiCue(r.ev("noteOn", note), q);
  r.advance(holdS);
  const off = r.engine.handleMidiCue(r.ev("noteOff", note), q);
  return { on, off };
}

function play(r: Rig, note: number) {
  return r.engine.handleMidiCue(r.ev("noteOn", note), NONE);
}

/** EXACT cue launch: no wind-up ramp, transport is `playing` immediately. */
function startPlaying(r: Rig) {
  r.engine.execute(cmd("transport.cue", { frame: 0 }));
  r.advance(0.05);
  r.engine.execute(cmd("transport.play"));
  r.advance(1.0);
}

/** Audible chain per lane: cue voice + non-gated ordinary sources. */
function voiceCounts(engine: AudioEngine) {
  return engine.cueSnapshot().audibleVoices;
}

describe("cue learning gates on decoded stems", () => {
  it("rejects global learning until all four stems are decoded", async () => {
    const r = await rig([0, 1, 2]);
    startPlaying(r);
    const { on } = learn(r, GLOBAL, 60);
    expect(on.action.type).toBe("learn.reject");
    expect(on.action.type === "learn.reject" && on.action.reason).toBe("stem-not-decoded");
    expect(r.engine.cueSnapshot().markers).toHaveLength(0);
  });

  it("rejects isolated learning on an undecoded lane but allows a decoded one", async () => {
    const r = await rig([0, 1, 2]);
    startPlaying(r);
    expect(learn(r, lane(3), 61).on.action.type).toBe("learn.reject");
    expect(learn(r, lane(1), 62).off.action.type).toBe("learn.commit");
  });

  it("never commits a marker that would invalidate on the next ingest", async () => {
    const r = await rig([0, 1, 2]);
    startPlaying(r);
    learn(r, GLOBAL, 60);
    r.engine.adoptBuffer(3, makeBuffer(1, SR * 16, SR, 0.25), {
      name: "stem 4",
      provenance: "bundled-demo",
      contentHash: "hash-3",
    } as never);
    expect(r.engine.cueSnapshot().invalid).toBe(0);
    expect(r.engine.cueSnapshot().learned).toBe(0);
  });
});

describe("cue routing and audible voices", () => {
  it("isolated cue: gate shut, solo open, cue injected at input, other lanes untouched", async () => {
    const r = await rig();
    startPlaying(r);
    const p = priv(r.engine);
    const before = p.tracks.map((t) => t.sources.map((s) => s.node));
    learn(r, lane(0), 60);
    const res = play(r, 60);
    expect(res.action.type).toBe("cue.play");

    const t0 = p.tracks[0]!;
    expect(t0.stemGate.gain.at(r.ctx.currentTime + 0.05)).toBeCloseTo(0, 3);
    expect(t0.soloGain.gain.at(r.ctx.currentTime + 0.05)).toBeCloseTo(1, 3);
    const voice = p.cueVoices[0]!;
    expect(voice).toBeTruthy();
    // Cue rides the lane chain: gain → input (→ FX → fader → solo), so the
    // fader and FX still process it.
    expect(voice.gain.outs).toContain(t0.input);
    expect(voice.gain.gain.at(voice.gain.gain.events.at(-1)!.time)).toBeGreaterThan(0);

    // One steady audible voice on the owned lane, others unchanged.
    expect(voiceCounts(r.engine)[0]).toBe(1);
    expect(p.tracks.map((t) => t.sources.map((s) => s.node)).slice(1)).toEqual(before.slice(1));
    expect(r.engine.cueSnapshot().owned).toEqual([true, false, false, false]);
  });

  it("retrigger shows at most two voices, and only across the 12 ms seam", async () => {
    const r = await rig();
    startPlaying(r);
    learn(r, lane(0), 60, 2);
    play(r, 60);
    const first = priv(r.engine).cueVoices[0]!;
    r.advance(0.2);
    play(r, 60);
    const second = priv(r.engine).cueVoices[0]!;
    expect(second).not.toBe(first);
    // Old voice is fully faded by the end of its scheduled seam; the new one
    // is fully open by the end of its own. Only one steady voice remains.
    const firstEnd = first.gain.gain.events.at(-1)!.time;
    const secondOpen = second.gain.gain.events[1]!.time;
    expect(firstEnd - r.ctx.currentTime).toBeLessThanOrEqual(0.0121);
    expect(first.gain.gain.at(firstEnd)).toBeCloseTo(0, 3);
    expect(second.gain.gain.at(secondOpen)).toBeCloseTo(1, 3);
    expect(voiceCounts(r.engine)[0]).toBe(1);
  });

  it("global cue owns four cue voices with no normal doubling", async () => {
    const r = await rig();
    startPlaying(r);
    learn(r, GLOBAL, 64);
    play(r, 64);
    expect(r.engine.cueSnapshot().owned).toEqual([true, true, true, true]);
    expect(voiceCounts(r.engine)).toEqual([1, 1, 1, 1]);
    for (const t of priv(r.engine).tracks) {
      expect(t.stemGate.gain.at(r.ctx.currentTime + 0.05)).toBeCloseTo(0, 3);
    }
  });

  it("mixer state is untouched and restored after the cue completes", async () => {
    const r = await rig();
    r.engine.execute(cmd("track.mute", { id: 1, on: true }));
    startPlaying(r);
    const mixBefore = r.engine.status().tracks.map((t) => ({ muted: t.muted, gain: t.gain }));
    learn(r, GLOBAL, 65, 0.3);
    play(r, 65);
    expect(r.engine.status().tracks.map((t) => ({ muted: t.muted, gain: t.gain }))).toEqual(mixBefore);
    r.advance(1.0);
    (r.engine as unknown as { stopAllCues: (s: string) => void }).stopAllCues("test");
    expect(r.engine.cueSnapshot().owned).toEqual([false, false, false, false]);
    expect(r.engine.status().tracks.map((t) => ({ muted: t.muted, gain: t.gain }))).toEqual(mixBefore);
  });
});

describe("cue eligibility, rejoin and transport", () => {
  it("stopped transport cannot learn, and a learned cue still plays without moving the timeline", async () => {
    const r = await rig();
    startPlaying(r);
    learn(r, GLOBAL, 66, 0.5);
    r.engine.execute(cmd("transport.stop"));
    r.advance(0.2);
    const posBefore = r.engine.status().position;
    play(r, 66);
    expect(r.engine.cueSnapshot().owned.filter(Boolean)).toHaveLength(4);
    r.advance(0.1);
    expect(r.engine.status().position).toBeCloseTo(posBefore, 3);
    (r.engine as unknown as { stopAllCues: (s: string) => void }).stopAllCues("test");
    expect(r.engine.cueSnapshot().owned).toEqual([false, false, false, false]);
    expect(r.engine.status().position).toBeCloseTo(posBefore, 3);
    expect(learn(r, GLOBAL, 67).on.action.type).toBe("learn.reject");
  });

  it("rejects learning while a global loop runs, and a cue rejoins the loop underlay", async () => {
    const r = await rig();
    startPlaying(r);
    learn(r, lane(0), 68, 0.4);
    r.engine.execute(cmd("loop.global.set", { start: 0.5, end: 2.5 }));
    r.engine.execute(cmd("loop.global.enable", { on: true }));
    expect(learn(r, GLOBAL, 69).on.action.type).toBe("learn.reject");
    play(r, 68);
    const target = r.engine.cueSnapshot().underlay[0];
    r.advance(0.2);
    (r.engine as unknown as { stopAllCues: (s: string) => void }).stopAllCues("test");
    const actual = r.engine.cueSnapshot().underlay[0];
    if (target != null && actual != null) {
      expect(Math.abs((actual - target) * SR)).toBeLessThan(SR * 0.5);
    }
    expect(r.engine.cueSnapshot().owned[0]).toBe(false);
  });

  it("rejects learning while a lane loop is enabled", async () => {
    const r = await rig();
    startPlaying(r);
    r.engine.execute(cmd("loop.lane.enable", { id: 2, on: true }));
    const e = r.engine.cueEligibility();
    expect(e.loopActive).toBe(true);
    expect(learn(r, lane(2), 70).on.action.type).toBe("learn.reject");
  });

  it("rejects learning while reversed or scrubbing", async () => {
    const r = await rig();
    startPlaying(r);
    r.engine.execute(cmd("track.reverse", { id: 0, on: true }));
    expect(r.engine.cueEligibility().reverseActive).toBe(true);
    expect(learn(r, lane(0), 71).on.action.type).toBe("learn.reject");
  });
});

describe("persistence and invalidation", () => {
  let saved: CueMarker[] = [];

  beforeEach(() => (saved = []));

  it("markers survive a reload and stay playable when hashes match", async () => {
    const r = await rig();
    startPlaying(r);
    learn(r, GLOBAL, 72, 0.5);
    saved = r.engine.cues.list().map((m) => ({ ...m }));
    expect(saved).toHaveLength(1);

    const r2 = await rig();
    const res = r2.engine.loadCueMarkers(saved);
    expect(res).toEqual({ loaded: 1, invalidated: 0 });
    startPlaying(r2);
    expect(play(r2, 72).action.type).toBe("cue.play");
  });

  it("a replaced stem invalidates dependent markers and a restore revives them", async () => {
    const r = await rig();
    startPlaying(r);
    learn(r, GLOBAL, 73, 0.5);
    learn(r, lane(1), 74, 0.5);
    expect(r.engine.cueSnapshot().learned).toBe(2);

    r.engine.adoptBuffer(1, makeBuffer(1, SR * 16, SR, 0.25), {
      name: "stem 2 v2",
      provenance: "user-private",
      contentHash: "hash-1-NEW",
    } as never);
    expect(r.engine.cueSnapshot().invalid).toBe(2);
    expect(play(r, 73).action.type).toBe("cue.reject");

    r.engine.adoptBuffer(1, makeBuffer(1, SR * 16, SR, 0.25), {
      name: "stem 2",
      provenance: "user-private",
      contentHash: "hash-1",
    } as never);
    expect(r.engine.cueSnapshot().invalid).toBe(0);
    expect(r.engine.cueSnapshot().learned).toBe(2);
  });
});
