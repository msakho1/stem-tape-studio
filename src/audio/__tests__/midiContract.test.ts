/**
 * Checkpoint 1 proof: normalized MIDI contract, identity, clock conversion,
 * staleness, disconnect, batched native delivery and Web MIDI hot-plug.
 *
 * The whole surface under test is pure/adapter code — no AudioEngine import
 * exists in this file, which is the checkpoint's "zero audio commands" claim.
 */

import { beforeEach, describe, expect, it, vi } from "vitest";
import {
  allNotesOffEvent,
  describeEvent,
  eventKey,
  isStale,
  normalizeMidiBytes,
  normalizeNativeEvent,
  STALE_EVENT_MS,
} from "../midi/contract";
import { MidiClock } from "../midi/clock";
import { NativeMidiBridge } from "../midi/nativeBridge";
import { WebMidiAdapter } from "../midi/webMidi";

const origin = { timestampMs: 1000, source: "test" as const, deviceId: "d1", deviceName: "Keystep" };

describe("byte normalization", () => {
  it("maps note on / note off / velocity-zero note on", () => {
    expect(normalizeMidiBytes([0x90, 60, 100], origin)).toMatchObject({
      kind: "noteOn", note: 60, velocity: 100, channel: 0,
    });
    expect(normalizeMidiBytes([0x82, 64, 40], origin)).toMatchObject({
      kind: "noteOff", note: 64, velocity: 40, channel: 2,
    });
    expect(normalizeMidiBytes([0x95, 72, 0], origin)).toMatchObject({
      kind: "noteOff", note: 72, velocity: 0, channel: 5,
    });
  });

  it("maps CC123 to allNotesOff and drops other messages", () => {
    expect(normalizeMidiBytes([0xb3, 123, 0], origin)).toMatchObject({ kind: "allNotesOff", channel: 3 });
    expect(normalizeMidiBytes([0xb0, 7, 100], origin)).toBeNull();  // volume CC
    expect(normalizeMidiBytes([0xf8], origin)).toBeNull();          // clock
    expect(normalizeMidiBytes([0xc0, 5], origin)).toBeNull();       // program change
    expect(normalizeMidiBytes([], origin)).toBeNull();
  });

  it("carries origin metadata verbatim", () => {
    const ev = normalizeMidiBytes([0x90, 60, 1], origin)!;
    expect(ev.source).toBe("test");
    expect(ev.deviceId).toBe("d1");
    expect(ev.deviceName).toBe("Keystep");
    expect(ev.timestampMs).toBe(1000);
    expect(describeEvent(ev)).toBe("on 60 v1 · ch1");
  });
});

describe("channel + note identity", () => {
  it("keys by channel and note so overlapping pairs cannot collide", () => {
    const a = normalizeMidiBytes([0x90, 60, 100], origin)!;
    const b = normalizeMidiBytes([0x91, 60, 100], origin)!;
    const c = normalizeMidiBytes([0x80, 60, 0], origin)!;
    expect(eventKey(a)).toBe("0:60");
    expect(eventKey(b)).toBe("1:60");
    expect(eventKey(a)).toBe(eventKey(c));
    expect(eventKey(a)).not.toBe(eventKey(b));
  });
});

describe("timestamp conversion", () => {
  it("maps performance.now() timestamps into AudioContext time", () => {
    const clock = new MidiClock();
    expect(clock.isAnchored()).toBe(false);
    clock.anchor(5000, 12.5);
    expect(clock.ctxTimeOf({ timestampMs: 5000 })).toBeCloseTo(12.5, 9);
    expect(clock.ctxTimeOf({ timestampMs: 5250 })).toBeCloseTo(12.75, 9);
    expect(clock.ctxTimeOf({ timestampMs: 4900 })).toBeCloseTo(12.4, 9);
    // A late JS callback still resolves the frame the key was pressed at.
    expect(clock.latencyOf({ timestampMs: 5000 }, 12.53)).toBeCloseTo(0.03, 9);
    clock.anchor(6000, 20);
    expect(clock.ctxTimeOf({ timestampMs: 6100 })).toBeCloseTo(20.1, 9);
  });
});

describe("staleness", () => {
  it("flags events older than the stale window", () => {
    const ev = normalizeMidiBytes([0x90, 60, 100], { ...origin, timestampMs: 1000 })!;
    expect(isStale(ev, 1000 + STALE_EVENT_MS - 1)).toBe(false);
    expect(isStale(ev, 1000 + STALE_EVENT_MS + 1)).toBe(true);
  });
});

describe("native bridge", () => {
  let bridge: NativeMidiBridge;
  let win: { __stemTapeMidi?: unknown };
  beforeEach(() => {
    bridge = new NativeMidiBridge();
    win = {};
    bridge.install(win as unknown as Window);
  });

  it("installs window.__stemTapeMidi and accepts batched events", () => {
    const seen: string[] = [];
    bridge.subscribe((ev) => seen.push(`${ev.kind}:${ev.note}:${ev.source}`));
    const api = (win as { __stemTapeMidi: { push: (e: unknown[]) => number } }).__stemTapeMidi;
    const n = api.push([
      { kind: "noteOn", note: 60, velocity: 100, channel: 0, timestampMs: 10, deviceName: "Launchkey" },
      { kind: "noteOn", note: 62, velocity: 0, channel: 0, timestampMs: 11, deviceName: "Launchkey" },
      { bytes: [0xb0, 123, 0], timestampMs: 12, deviceName: "Launchkey" },
      { kind: "garbage" },
      null,
    ]);
    expect(n).toBe(3);
    expect(seen).toEqual([
      "noteOn:60:coremidi-bridge",
      "noteOff:62:coremidi-bridge",
      "allNotesOff:0:coremidi-bridge",
    ]);
    expect(bridge.snapshot()).toMatchObject({ present: true, deviceName: "Launchkey" });
  });

  it("buffers events that arrive before the first subscriber", () => {
    bridge.push([{ kind: "noteOn", note: 48, velocity: 90, channel: 1, timestampMs: 1 }]);
    const seen: number[] = [];
    bridge.subscribe((ev) => seen.push(ev.note));
    expect(seen).toEqual([48]);
  });

  it("normalizes disconnect into allNotesOff", () => {
    const seen: string[] = [];
    bridge.subscribe((ev) => seen.push(ev.kind));
    const api = (win as { __stemTapeMidi: { disconnect: (i?: unknown) => void } }).__stemTapeMidi;
    api.disconnect({ deviceId: "x", deviceName: "Launchkey" });
    expect(seen).toEqual(["allNotesOff"]);
  });
});

describe("web midi adapter", () => {
  it("normalizes injected port messages", () => {
    const adapter = new WebMidiAdapter();
    const seen: string[] = [];
    adapter.subscribe((ev) => seen.push(`${ev.kind}:${ev.note}:${ev.deviceName}`));
    adapter.injectMessage([0x90, 60, 100], { id: "p1", name: "SP-1 Key" }, 42);
    adapter.injectMessage([0x90, 60, 0], { id: "p1", name: "SP-1 Key" }, 43);
    expect(seen).toEqual(["noteOn:60:SP-1 Key", "noteOff:60:SP-1 Key"]);
  });

  it("enumerates inputs and emits allNotesOff on hot-unplug", async () => {
    const port = { id: "p1", name: "Keystep", state: "connected" as const, type: "input" as const };
    const inputs = new Map<string, typeof port>([["p1", port]]);
    const access = { inputs, onstatechange: null as null | (() => void) };
    const nav = { requestMIDIAccess: vi.fn(async () => access) };
    vi.stubGlobal("navigator", nav);
    const adapter = new WebMidiAdapter();
    const seen: string[] = [];
    adapter.subscribe((ev) => seen.push(ev.kind));
    const state = await adapter.connect();
    expect(state.status).toBe("connected");
    expect(state.devices).toEqual([{ id: "p1", name: "Keystep" }]);
    inputs.delete("p1");
    access.onstatechange?.();
    expect(adapter.snapshot().devices).toEqual([]);
    expect(seen).toEqual(["allNotesOff"]);
    vi.unstubAllGlobals();
  });
});

describe("checkpoint scope", () => {
  it("emits no audio commands: allNotesOffEvent is a pure value", () => {
    const ev = allNotesOffEvent({ ...origin, timestampMs: 7 }, 9);
    expect(ev).toEqual({
      kind: "allNotesOff", note: 0, velocity: 0, channel: 9,
      timestampMs: 7, source: "test", deviceId: "d1", deviceName: "Keystep",
    });
  });
});
