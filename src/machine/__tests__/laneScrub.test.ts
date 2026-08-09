import { describe, expect, it } from "vitest";
import { applyGlobalScrub, initialSurfaceState, pressControl, releaseControl, type SurfaceState } from "@/machine/surface";

function commands(s: SurfaceState) {
  return s.commands.map((c: { type: string }) => c.type);
}

function hold(s: SurfaceState, control: string): SurfaceState {
  return pressControl(s, control as never);
}

describe("FUNCTION + Track + rocker → per-lane shuttle", () => {
  it("scopes the shuttle to the held lane and never moves the shared transport", () => {
    let s = initialSurfaceState();
    s = hold(s, "function");
    s = hold(s, "track-button-1");
    s = applyGlobalScrub(s, 1, 20);
    const cmds = commands(s);
    expect(cmds).toContain("lane.scrub.start");
    expect(cmds).not.toContain("transport.scrub.start");
    expect(s.laneScrub[0]).toBe(1);
  });

  it("emits exactly one end command per lane on release", () => {
    let s = initialSurfaceState();
    s = hold(s, "function");
    s = hold(s, "track-button-2");
    s = applyGlobalScrub(s, -1, 10);
    s = applyGlobalScrub(s, null, 400);
    const ends = s.commands.filter((c: { type: string }) => c.type === "lane.scrub.end");
    expect(ends).toHaveLength(1);
    expect(ends[0]!.payload["lane"]).toBe(1);
    expect(s.laneScrub.every((v: number) => v === 0)).toBe(true);
  });

  it("falls back to the global four-stem shuttle with no lane held", () => {
    let s = initialSurfaceState();
    s = hold(s, "function");
    s = applyGlobalScrub(s, 1, 10);
    expect(commands(s)).toContain("transport.scrub.start");
    expect(s.laneScrub.every((v: number) => v === 0)).toBe(true);
  });

  it("shuttles every held lane at once", () => {
    let s = initialSurfaceState();
    s = hold(s, "function");
    s = hold(s, "track-button-1");
    s = hold(s, "track-button-3");
    s = applyGlobalScrub(s, 1, 8);
    const starts = s.commands.filter((c: { type: string }) => c.type === "lane.scrub.start").map((c: { payload: Record<string, unknown> }) => c.payload["lane"]);
    expect(starts).toEqual([0, 2]);
  });

  it("releases a lane dropped from the chord while the other keeps shuttling", () => {
    let s = initialSurfaceState();
    s = hold(s, "function");
    s = hold(s, "track-button-1");
    s = hold(s, "track-button-2");
    s = applyGlobalScrub(s, 1, 6);
    s = releaseControl(s, "track-button-2" as never);
    s = applyGlobalScrub(s, 1, 21);
    expect(s.laneScrub[0]).toBe(1);
    expect(s.laneScrub[1]).toBe(0);
    expect(s.commands.some((c: { type: string; payload: Record<string, unknown> }) => c.type === "lane.scrub.end" && c.payload["lane"] === 1)).toBe(true);
  });

  it("never leaves a global and a lane shuttle sounding at the same time", () => {
    let s = initialSurfaceState();
    s = hold(s, "function");
    s = applyGlobalScrub(s, 1, 5);
    s = hold(s, "track-button-4");
    s = applyGlobalScrub(s, 1, 12);
    expect(s.globalScrub).toBe(0);
    expect(s.laneScrub[3]).toBe(1);
    expect(s.commands.some((c: { type: string; payload: Record<string, unknown> }) => c.type === "transport.scrub.end")).toBe(true);
  });
});
