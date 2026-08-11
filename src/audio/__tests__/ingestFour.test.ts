/**
 * Four-stem load regression: both entry points (bulk sequential pick and
 * four individual picks) must produce four decoded cells, roles intact.
 */
import { describe, it, expect, beforeEach } from "vitest";
import { ingestSequential, ingestStem, ROLE_TRACK } from "../ingest";
import { session } from "../session";
import { STEM_ROLE_LIST, type StemRole } from "../format";
import type { AudioEngine, TrackId } from "../engine";

const SR = 44100;

function wavFile(name: string, seconds = 1): File {
  const frames = Math.round(seconds * SR);
  const dataBytes = frames * 2 * 2;
  const buf = new ArrayBuffer(44 + dataBytes);
  const v = new DataView(buf);
  const tag = (off: number, s: string) => {
    for (let i = 0; i < s.length; i++) v.setUint8(off + i, s.charCodeAt(i));
  };
  tag(0, "RIFF");
  v.setUint32(4, 36 + dataBytes, true);
  tag(8, "WAVE");
  tag(12, "fmt ");
  v.setUint32(16, 16, true);
  v.setUint16(20, 1, true);
  v.setUint16(22, 2, true);
  v.setUint32(24, SR, true);
  v.setUint32(28, SR * 4, true);
  v.setUint16(32, 4, true);
  v.setUint16(34, 16, true);
  tag(36, "data");
  v.setUint32(40, dataBytes, true);
  return new File([buf], name, { type: "audio/wav" });
}

function fakeBuffer(seconds = 1) {
  const length = Math.round(seconds * SR);
  return {
    numberOfChannels: 2,
    length,
    sampleRate: SR,
    duration: seconds,
    getChannelData: () => new Float32Array(length),
  } as unknown as AudioBuffer;
}

function fakeEngine() {
  const adopted: TrackId[] = [];
  const engine = {
    ctx: {
      sampleRate: SR,
      decodeAudioData: async () => fakeBuffer(),
    } as unknown as BaseAudioContext,
    unlock: async () => {},
    preDecodeGate: () => ({ ok: true, detail: "ok" }),
    adoptBuffer: (id: TrackId) => {
      adopted.push(id);
      return { ok: true, detail: "adopted", bytes: 1000 };
    },
    clearScrubCandidates: () => {},
    analyzeGrid: async () => null,
  } as unknown as AudioEngine;
  return { engine, adopted };
}

function loadedRoles(): StemRole[] {
  const stems = session.get().stems;
  return STEM_ROLE_LIST.filter((r) => stems[r]?.loaded && !stems[r]?.trashed);
}

describe("four-stem ingest", () => {
  beforeEach(() => session.reset());

  it("bulk pick decodes 4/4 with correct roles", async () => {
    const { engine, adopted } = fakeEngine();
    const results = await ingestSequential(
      engine,
      STEM_ROLE_LIST.map((role) => ({ role, file: wavFile(`${role}.wav`), provenance: "user-private" as const })),
    );
    expect(results.filter((r) => r.ok)).toHaveLength(4);
    expect(adopted).toEqual(STEM_ROLE_LIST.map((r) => ROLE_TRACK[r]));
    expect(loadedRoles()).toEqual(["vocals", "drums", "bass", "instruments"]);
  });

  it("four individual picks decode 4/4 with correct roles", async () => {
    const { engine, adopted } = fakeEngine();
    for (const role of STEM_ROLE_LIST) {
      const r = await ingestStem(engine, role, wavFile(`${role}.wav`), "user-private");
      expect(r.ok, `${role}: ${r.detail}`).toBe(true);
    }
    expect(adopted).toHaveLength(4);
    expect(loadedRoles()).toEqual(["vocals", "drums", "bass", "instruments"]);
  });
});
