/**
 * Phase 6 §3/§4 acceptance: heads geometry, drift, reverse and PRINT.
 *
 * Pure math only — the same functions both engines and the PRINT renderer use.
 */

import { describe, expect, it } from "vitest";
import {
  chooseSource,
  defaultHead,
  emptyHeads,
  enterHeads,
  exitHeads,
  headOffsetFrames,
  headReadPosition,
  peakOf,
  renderHeadsCycle,
  scrubHead,
  toggleHeadReverse,
} from "../heads";

const SR = 48000;
const CYCLE = 96000; // 2 s
const START = 12000;

function ramp(n: number): Float32Array {
  const a = new Float32Array(n);
  for (let i = 0; i < n; i++) a[i] = i / n;
  return a;
}

describe("heads geometry (§3.1)", () => {
  it("places the four heads at exactly 0, 25, 50 and 75 % of the cycle", () => {
    const s = enterHeads(emptyHeads(), { source: 0, cycleFrames: CYCLE, cycleStartFrame: START, engine: "node", frame: 0 }).state;
    expect([0, 1, 2, 3].map((i) => headOffsetFrames(s, i))).toEqual([0, 24000, 48000, 72000]);
  });

  it("rejects entry when no loaded, playing, unmuted source exists", () => {
    const src = chooseSource([
      { index: 0, loaded: true, playing: false, muted: false },
      { index: 1, loaded: true, playing: true, muted: true },
    ]);
    expect(src).toBeNull();
    const r = enterHeads(emptyHeads(), { source: src, cycleFrames: CYCLE, cycleStartFrame: 0, engine: "node", frame: 0 });
    expect(r.ok).toBe(false);
    expect(r.detail).toContain("no loaded, playing, unmuted source");
  });

  it("keeps the 25 % spacing exact after ten minutes of playback (no accumulation)", () => {
    const s = enterHeads(emptyHeads(), { source: 0, cycleFrames: CYCLE, cycleStartFrame: START, engine: "node", frame: 0 }).state;
    const tenMinutes = START + 10 * 60 * SR; // 28 800 000 frames
    const p = s.heads.map((h) => headReadPosition(h, tenMinutes, START, CYCLE));
    const phase = (x: number) => (((x - START) % CYCLE) + CYCLE) % CYCLE;
    for (let i = 1; i < 4; i++) {
      const delta = (phase(p[i]!) - phase(p[0]!) + CYCLE) % CYCLE;
      expect(delta).toBe(i * 0.25 * CYCLE); // exact, 0 frames of drift
    }
  });

  it("reverse is a negative read step, mirrored about the head offset", () => {
    const s0 = enterHeads(emptyHeads(), { source: 0, cycleFrames: CYCLE, cycleStartFrame: START, engine: "node", frame: 0 }).state;
    const s = toggleHeadReverse(s0, 1);
    const h = s.heads[1]!;
    const a = headReadPosition(h, START + 1000, START, CYCLE);
    const b = headReadPosition(h, START + 2000, START, CYCLE);
    expect(a - b).toBe(1000); // reads backwards, one frame per frame
  });

  it("FUNCTION + fader scrub is absolute, not relative", () => {
    const s = scrubHead(emptyHeads(), 2, 0.1);
    expect(s.heads[2]!.offset).toBeCloseTo(0.1, 12);
    expect(scrubHead(s, 2, 0.9).heads[2]!.offset).toBeCloseTo(0.9, 12);
    expect(s.heads[2]!.scrubbed).toBe(true);
  });

  it("exit clears the head table so a new session always restarts at the quarters", () => {
    const s = toggleHeadReverse(scrubHead(emptyHeads(), 0, 0.42), 3);
    const out = exitHeads(s);
    expect(out.active).toBe(false);
    expect(out.heads).toEqual([0, 1, 2, 3].map(defaultHead));
  });
});

describe("PRINT render (§4.1)", () => {
  const n = 4800;
  const src = [ramp(n + START)];
  const heads = [0, 1, 2, 3].map(defaultHead);

  it("bakes exactly one cycle: no missing or duplicated frames", () => {
    const out = renderHeadsCycle(src, START, n, [{ ...defaultHead(0), level: 1 }]);
    expect(out[0]!.length).toBe(n);
    for (let i = 0; i < n; i++) expect(out[0]![i]!).toBeCloseTo(src[0]![START + i]!, 6);
  });

  it("sums four heads at their quarter offsets", () => {
    const out = renderHeadsCycle(src, START, n, heads.map((h) => ({ ...h, level: 0.25 })));
    const at = (i: number) => src[0]![START + (i % n)]!;
    for (const i of [0, 1200, 2400, 3600]) {
      const expected = 0.25 * (at(i) + at(i + n * 0.25) + at(i + n * 0.5) + at(i + n * 0.75));
      expect(out[0]![i]!).toBeCloseTo(Math.min(1, expected), 6);
    }
  });

  it("muted heads contribute nothing and an all-muted print is digital silence", () => {
    const out = renderHeadsCycle(src, START, n, heads.map((h) => ({ ...h, muted: true })));
    expect(peakOf(out)).toBe(0);
  });

  it("clamps the four-head sum to full scale instead of wrapping", () => {
    const loud = [new Float32Array(n + START).fill(0.9)];
    const out = renderHeadsCycle(loud, START, n, heads.map((h) => ({ ...h, level: 1 })));
    expect(peakOf(out)).toBeLessThanOrEqual(1);
    expect(peakOf(out)).toBeCloseTo(1, 12);
  });

  it("a reversed head prints the cycle backwards, frame for frame", () => {
    const out = renderHeadsCycle(src, START, n, [{ ...defaultHead(0), level: 1, reverse: true }]);
    for (const i of [1, 100, 4799]) expect(out[0]![i]!).toBeCloseTo(src[0]![START + ((n - i) % n)]!, 6);
  });
});
