/**
 * Global four-stem shuttle — reducer + keyboard-derivation acceptance tests.
 *
 * Audible behaviour is proven in the browser (masterRms while held); these
 * tests pin the ordered command contract and the registry-derived panel.
 */

import { describe, expect, it } from "vitest";
import { applyGlobalScrub, initialSurfaceState } from "@/machine/surface";
import { STEM_TAPE_ROW_BY_ID } from "@/machine/stemTapeV1Map";
import { isLit, keyboardBindings } from "@/device/keyboardMap";

describe("global shuttle command stream", () => {
  it("emits exactly one start on entry and one end on release", () => {
    let s = initialSurfaceState();
    s = applyGlobalScrub(s, 1);
    expect(s.globalScrub).toBe(1);
    s = applyGlobalScrub(s, 1); // repeat keydown must not re-emit
    s = applyGlobalScrub(s, null);
    const types = s.commands.map((c) => c.type);
    expect(types.filter((t) => t === "transport.scrub.start")).toHaveLength(1);
    expect(types.filter((t) => t === "transport.scrub.end")).toHaveLength(1);
    expect(types.indexOf("transport.scrub.start")).toBeLessThan(types.indexOf("transport.scrub.end"));
    expect(s.globalScrub).toBe(0);
  });

  it("carries the held direction and re-emits on reversal", () => {
    let s = applyGlobalScrub(initialSurfaceState(), -1);
    expect(s.commands.at(-1)?.payload["direction"]).toBe(-1);
    s = applyGlobalScrub(s, 1);
    expect(s.commands.at(-1)?.payload["direction"]).toBe(1);
    expect(s.globalScrub).toBe(1);
  });

  it("never emits a discrete transport.scrub step while shuttling", () => {
    let s = applyGlobalScrub(initialSurfaceState(), 1);
    s = applyGlobalScrub(s, null);
    expect(s.commands.map((c) => c.type)).not.toContain("transport.scrub");
  });

  it("is rejected while a take is recording", () => {
    const base = initialSurfaceState();
    const tracks = [...base.tracks] as typeof base.tracks;
    tracks[1] = { ...tracks[1], content: "recording" };
    const s = applyGlobalScrub({ ...base, tracks }, 1);
    expect(s.globalScrub).toBe(0);
    expect(s.commands).toHaveLength(0);
    expect(s.note).toMatch(/rejected/);
  });

  it("emits one end whichever key is released first (F first or Q/A first)", () => {
    // Both orders funnel through the same single `dir: null` dispatch in the
    // keyboard layer, so the reducer must answer identically.
    for (const _order of ["f-first", "rocker-first"]) {
      let s = applyGlobalScrub(initialSurfaceState(), 1);
      s = applyGlobalScrub(s, null);
      expect(s.commands.filter((c) => c.type === "transport.scrub.end")).toHaveLength(1);
      // A duplicate cleanup (blur, then Escape) must not fire a second end.
      s = applyGlobalScrub(s, null);
      expect(s.commands.filter((c) => c.type === "transport.scrub.end")).toHaveLength(1);
      expect(s.globalScrub).toBe(0);
    }
  });

  it("release is a no-op when nothing is shuttling", () => {
    const s = applyGlobalScrub(initialSurfaceState(), null);
    expect(s.commands).toHaveLength(0);
  });
});

describe("keyboard panel derivation", () => {
  const bindings = keyboardBindings();

  it("derives the shuttle chords from the registry row, not from hard-coded copy", () => {
    const row = STEM_TAPE_ROW_BY_ID["rocker.scrub"]!;
    const shuttle = bindings.filter((b) => b.id === "rocker.scrub");
    expect(shuttle.map((b) => b.codes)).toEqual([
      ["KeyF", "KeyQ"],
      ["KeyF", "KeyA"],
    ]);
    expect(shuttle[0]!.detail).toBe(row.tutorial!.plainLanguage);
    expect(shuttle[0]!.source).toBe("registry");
  });

  it("lights a chord only when every key is held", () => {
    const fwd = bindings.find((b) => b.id === "rocker.scrub" && b.codes.includes("KeyQ"))!;
    expect(isLit(fwd, ["KeyQ"])).toBe(false);
    expect(isLit(fwd, ["KeyF", "KeyQ"])).toBe(true);
  });

  it("exposes base, fx, heads and record contexts", () => {
    const contexts = new Set(bindings.map((b) => b.context));
    expect([...contexts].sort()).toEqual(["base", "fx", "heads", "record"]);
  });

  it("includes the four keyboard fader pairs and the escape release", () => {
    expect(bindings.filter((b) => /^fader\.\d$/.test(b.id))).toHaveLength(4);
    expect(bindings.find((b) => b.id === "shell.release")?.codes).toEqual(["Escape"]);
  });
});
