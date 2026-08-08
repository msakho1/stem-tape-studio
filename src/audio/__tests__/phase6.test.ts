import { describe, expect, it } from "vitest";
import { emptyGrid, tapGrid, decidePunch, nextBoundary } from "@/audio/grid";
import { wavHeader, wavTotalBytes, encodeChunk, parseWavHeader, exportPlan } from "@/audio/export/wavStream";
import { emptyManifest, verifyDurability } from "@/audio/input/takes";
import { initialRecordingModel, recordingReduce } from "@/machine/recordingState";
import { RECORDING_ROWS, STEM_TAPE_V1_MAP } from "@/machine/stemTapeV1Map";

const SR = 48000;
const TOL = 1e-9;

describe("phase 6 — tempo grid", () => {
  it("learns 120 BPM from evenly spaced taps", () => {
    let g = emptyGrid(SR);
    const step = SR / 2; // 0.5 s = 120 BPM
    let last: number | null = null;
    for (let i = 0; i < 5; i++) {
      const f = i * step;
      g = tapGrid(g, f, last);
      last = f;
    }
    expect(g.bpm).not.toBeNull();
    expect(Math.abs(g.bpm! - 120)).toBeLessThan(TOL + 1e-6);
    expect(g.source).toBe("tapped");
  });

  it("rejects a tap outside 40–220 BPM instead of half-following it", () => {
    let g = emptyGrid(SR);
    g = tapGrid(g, 0, null);
    g = tapGrid(g, 10 * SR, 0);
    expect(g.rejected).toBe(true);
    expect(g.bpm).toBeNull();
  });

  it("a late press recovers from look-back; an early press quantises forward", () => {
    let g = emptyGrid(SR);
    const step = SR / 2;
    let last: number | null = null;
    for (let i = 0; i < 5; i++) { g = tapGrid(g, i * step, last); last = i * step; }
    const late = decidePunch(g, 4 * step + 480, { maxLookBackFrames: 4 * SR });
    expect(late.mode).toBe("late-lookback");
    expect(late.startFrame).toBe(4 * step);
    expect(late.lookBackFrames).toBe(480);
    const early = decidePunch(g, 4 * step + step / 2, { maxLookBackFrames: 4 * SR });
    expect(early.mode).toBe("next-boundary");
    expect(early.startFrame).toBe(nextBoundary(g, 4 * step + step / 2));
    expect(early.startFrame % step).toBe(0);
  });
});

describe("phase 6 — WAV export math", () => {
  it("header declares exactly the payload size for 16- and 24-bit", () => {
    for (const bitDepth of [16, 24] as const) {
      const spec = { sampleRate: SR, channels: 2, bitDepth, frames: 1000 };
      const h = wavHeader(spec);
      const parsed = parseWavHeader(h);
      expect(parsed.sampleRate).toBe(SR);
      expect(parsed.channels).toBe(2);
      expect(parsed.bitDepth).toBe(bitDepth);
      expect(h.length + parsed.dataBytes).toBe(wavTotalBytes(spec));
      const pcm = encodeChunk([new Float32Array(1000), new Float32Array(1000)], 1000, bitDepth, false);
      expect(pcm.length).toBe(parsed.dataBytes);
    }
  });

  it("falls back to 16-bit when 24-bit exceeds the measured ceiling", () => {
    const spec = { sampleRate: SR, channels: 2, bitDepth: 24 as const, frames: SR * 600 };
    const plan = exportPlan(spec, 200 * 1024 * 1024);
    expect(["16-bit", "segmented"]).toContain(plan.mode);
  });

  it("full-scale samples encode without wrapping", () => {
    const pcm = encodeChunk([Float32Array.from([1, -1])], 2, 16, false);
    const view = new DataView(pcm.buffer, pcm.byteOffset, pcm.byteLength);
    expect(view.getInt16(0, true)).toBe(32767);
    expect(view.getInt16(2, true)).toBe(-32767);
  });
});

describe("phase 6 — take durability", () => {
  it("a take with a gap is never marked ready", () => {
    const t = emptyManifest({ id: "a", projectId: "p", trackId: 0, sampleRate: SR, channels: 1 });
    t.frames = 2000;
    t.chunks = [
      { index: 0, blobKey: "k0", startFrame: 0, frames: 1000, bytes: 4000 },
      { index: 1, blobKey: "k1", startFrame: 1500, frames: 500, bytes: 2000 },
    ];
    const v = verifyDurability(t);
    expect(v.ok).toBe(false);
    expect(v.detail).toMatch(/gap|contig|missing|frame/i);
  });

  it("a contiguous take verifies", () => {
    const t = emptyManifest({ id: "b", projectId: "p", trackId: 0, sampleRate: SR, channels: 1 });
    t.frames = 2000;
    t.chunks = [
      { index: 0, blobKey: "k0", startFrame: 0, frames: 1000, bytes: 4000 },
      { index: 1, blobKey: "k1", startFrame: 1000, frames: 1000, bytes: 4000 },
    ];
    expect(verifyDurability(t).ok).toBe(true);
  });
});

describe("phase 6 — recording state table", () => {
  it("hold arms an empty track and waits for sound; tap cancels", () => {
    let m = initialRecordingModel(["empty", "loaded", "empty", "empty"]);
    m = recordingReduce(m, { type: "rec.inputEnabled" });
    m = recordingReduce(m, { type: "rec.arm", track: 0 });
    expect(m.tracks[0]!.phase).toBe("waiting-for-sound");
    m = recordingReduce(m, { type: "rec.tap", track: 0 });
    expect(m.tracks[0]!.phase).toBe("idle");
  });

  it("only one external-input target is armed at a time", () => {
    let m = recordingReduce(initialRecordingModel(), { type: "rec.inputEnabled" });
    m = recordingReduce(m, { type: "rec.arm", track: 0 });
    m = recordingReduce(m, { type: "rec.arm", track: 2 });
    const armed = m.tracks.filter((t) => t.phase !== "idle").length;
    expect(armed).toBe(1);
    expect(m.target).toBe(2);
  });

  it("arming is refused before input is enabled", () => {
    const m = recordingReduce(initialRecordingModel(), { type: "rec.arm", track: 1 });
    expect(m.tracks[1]!.phase).toBe("idle");
    expect(m.lastAck).toMatch(/input/i);
  });
});

describe("phase 6 — mapping registry", () => {
  it("registers the recording rows with tutorial metadata and keeps ids unique", () => {
    const ids = STEM_TAPE_V1_MAP.map((r) => r.id);
    expect(new Set(ids).size).toBe(ids.length);
    for (const r of RECORDING_ROWS) {
      expect(r.tutorial?.plainLanguage.length).toBeGreaterThan(10);
      expect(r.tutorial?.highlight.length).toBeGreaterThan(0);
    }
  });

  it("PRINT stays reserved in the registry", () => {
    const print = RECORDING_ROWS.find((r) => r.id === "print.reserved")!;
    expect(print.command).toMatch(/RESERVED/);
  });
});
