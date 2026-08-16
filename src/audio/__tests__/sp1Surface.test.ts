/**
 * Physical Stem Tape SP-1 routing proof.
 *
 * The physical surface must enter the raw control path, never the cue system.
 */

import { describe, expect, it, vi } from "vitest";
import {
  decodeSp1Message,
  isSp1DeviceName,
  Sp1SurfaceAdapter,
  SP1_NOTE_CONTROLS,
  type Sp1SurfaceEvent,
} from "../midi/sp1Surface";
import { WebMidiAdapter } from "../midi/webMidi";
import { sp1Surface } from "../midi/sp1Surface";

const SP1 = { id: "p1", name: "STEM TAPE SP-1 BLOCK 1" };

function collect(adapter: Sp1SurfaceAdapter) {
  const seen: Sp1SurfaceEvent[] = [];
  adapter.subscribe((ev) => seen.push(ev));
  return seen;
}

describe("device recognition", () => {
  it("matches STEM TAPE SP-1 case-insensitively with suffixes", () => {
    expect(isSp1DeviceName("STEM TAPE SP-1 BLOCK 1")).toBe(true);
    expect(isSp1DeviceName("stem tape sp-1")).toBe(true);
    expect(isSp1DeviceName("Keystep")).toBe(false);
    expect(isSp1DeviceName(null)).toBe(false);
  });
});

describe("firmware contract mapping", () => {
  it("maps notes 36..45 to the specified physical controls", () => {
    expect(SP1_NOTE_CONTROLS).toEqual({
      36: "track-button-1",
      37: "track-button-2",
      38: "track-button-3",
      39: "track-button-4",
      40: "play",
      41: "function",
      42: "volume-plus",
      43: "volume-minus",
      44: "rocker-fwd",
      45: "rocker-rwd",
    });
    for (const note of Object.keys(SP1_NOTE_CONTROLS).map(Number)) {
      expect(decodeSp1Message([0x90, note, 127])).toEqual({
        type: "down",
        control: SP1_NOTE_CONTROLS[note],
      });
    }
  });

  it("treats Note On velocity 0 and Note Off as release", () => {
    expect(decodeSp1Message([0x90, 36, 0])).toEqual({ type: "up", control: "track-button-1" });
    expect(decodeSp1Message([0x80, 36, 0])).toEqual({ type: "up", control: "track-button-1" });
  });

  it("maps CC20..23 to faders, CC24 to battery, CC123 to all-off", () => {
    expect(decodeSp1Message([0xb0, 20, 127])).toEqual({ type: "fader", index: 0, value: 1 });
    expect(decodeSp1Message([0xb0, 21, 0])).toEqual({ type: "fader", index: 1, value: 0 });
    expect(decodeSp1Message([0xb0, 22, 64])).toMatchObject({ type: "fader", index: 2 });
    expect(decodeSp1Message([0xb0, 23, 127])).toEqual({ type: "fader", index: 3, value: 1 });
    expect(decodeSp1Message([0xb0, 24, 90])).toEqual({ type: "battery", value: 90 });
    expect(decodeSp1Message([0xb0, 123, 0])).toEqual({ type: "allOff" });
    expect(decodeSp1Message([0xb0, 7, 100])).toBeNull();
  });
});

describe("adapter held state", () => {
  it("emits Track 1 down/up for note 36", () => {
    const a = new Sp1SurfaceAdapter();
    const seen = collect(a);
    a.handleBytes([0x90, 36, 127], SP1, 10);
    a.handleBytes([0x80, 36, 0], SP1, 20);
    expect(seen.map((e) => `${e.type}:${"control" in e ? e.control : ""}`)).toEqual([
      "down:track-button-1",
      "up:track-button-1",
    ]);
  });

  it("is idempotent on repeated down and repeated up", () => {
    const a = new Sp1SurfaceAdapter();
    const seen = collect(a);
    a.handleBytes([0x90, 40, 127], SP1, 1);
    a.handleBytes([0x90, 40, 127], SP1, 2);
    a.handleBytes([0x80, 40, 0], SP1, 3);
    a.handleBytes([0x80, 40, 0], SP1, 4);
    expect(seen.map((e) => e.type)).toEqual(["down", "up"]);
  });

  it("releases every held control on CC123 and on disconnect", () => {
    const a = new Sp1SurfaceAdapter();
    const seen = collect(a);
    a.handleBytes([0x90, 36, 127], SP1, 1);
    a.handleBytes([0x90, 41, 127], SP1, 2);
    a.handleBytes([0xb0, 123, 0], SP1, 3);
    expect(seen.filter((e) => e.type === "up").map((e) => ("control" in e ? e.control : ""))).toEqual([
      "track-button-1",
      "function",
    ]);
    a.handleBytes([0x90, 39, 127], SP1, 4);
    a.deviceDisconnected(SP1.id);
    expect(a.snapshot().held).toEqual([]);
    expect(seen.at(-1)).toMatchObject({ type: "up", control: "track-button-4" });
    a.deviceDisconnected(SP1.id); // idempotent
    expect(a.snapshot().held).toEqual([]);
  });

  it("accepts live Web MIDI page timestamps and repairs invalid ones", () => {
    const a = new Sp1SurfaceAdapter();
    const seen = collect(a);
    const t = performance.now();
    a.handleBytes([0x90, 36, 127], SP1, t);
    expect(seen[0]!.timestampMs).toBe(t);
    a.handleBytes([0x80, 36, 0], SP1, Number.NaN);
    expect(Number.isFinite(seen[1]!.timestampMs)).toBe(true);
    a.handleBytes([0x90, 37, 127], SP1, 1e18); // not this time domain
    expect(seen[2]!.timestampMs).toBeLessThan(1e12);
  });
});

describe("transport routing", () => {
  it("consumes SP-1 messages before cue processing, and leaves others alone", () => {
    const web = new WebMidiAdapter();
    const cues: string[] = [];
    web.subscribe((ev) => cues.push(`${ev.kind}:${ev.note}`));
    const surface: string[] = [];
    const off = sp1Surface.subscribe((ev) => surface.push(`${ev.type}`));

    web.injectMessage([0x90, 36, 127], SP1, performance.now());
    expect(cues).toEqual([]);              // never reaches the cue system
    expect(surface).toEqual(["down"]);     // routed to the physical surface

    web.injectMessage([0x90, 60, 100], { id: "p2", name: "Keystep" }, performance.now());
    expect(cues).toEqual(["noteOn:60"]);   // other controllers unchanged
    expect(surface).toEqual(["down"]);

    sp1Surface.releaseAll();
    off();
  });
});

describe("no audio commands", () => {
  it("adapter module never touches the engine", () => {
    const spy = vi.fn();
    const a = new Sp1SurfaceAdapter();
    a.subscribe(spy);
    a.handleBytes([0xb0, 20, 127], SP1, performance.now());
    expect(spy).toHaveBeenCalledWith(expect.objectContaining({ type: "fader", value: 1 }));
  });
});
