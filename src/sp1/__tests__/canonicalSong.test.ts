import { describe, it, expect } from "vitest";
import { MockSp1 } from "./mockSerial";
import { Sp1Transport, Sp1Session } from "../protocol";
import { parseMeta, metaBlockCount } from "../meta";
import {
  prepareCanonicalSong,
  assertCanonicalSong,
  packStereo24,
  readInt24LE,
  checksum32,
  CANONICAL_SAMPLE_RATE,
  type CanonicalSong,
} from "../song";
import { legacyVerdict, negotiate, LOCK_NOTICE } from "../compatibility";
import { LegacyProvisionalTransport, MutationLockedError, portIsMock } from "../transport";
import { GENERATED_PROTOCOL } from "../generatedProtocol";

const STEMS = ["vocal", "drums", "bass", "instrument"] as const;

function fakeBuffer(frames: number, channels: number, sampleRate: number, fill: (i: number, c: number) => number) {
  const data: Float32Array[] = [];
  for (let c = 0; c < channels; c++) {
    const arr = new Float32Array(frames);
    for (let i = 0; i < frames; i++) arr[i] = fill(i, c);
    data.push(arr);
  }
  return {
    sampleRate,
    numberOfChannels: channels,
    length: frames,
    duration: frames / sampleRate,
    getChannelData: (c: number) => data[c]!,
  } as unknown as AudioBuffer;
}

const meta = { title: "T", artist: "A", bpm: 120, downbeatSeconds: 0 };

async function prep(frames: number[], channels = 2) {
  return prepareCanonicalSong(
    STEMS.map((name, i) => ({
      name,
      filename: `${name}.wav`,
      buffer: fakeBuffer(frames[i] ?? frames[0]!, channels, CANONICAL_SAMPLE_RATE, (n, c) =>
        Math.sin((n / 40) * (c + 1)) * 0.5,
      ),
    })),
    { metadata: meta },
  );
}

describe("canonical stereo 24-bit song", () => {
  it("14,592 frames produce 87,552 unpadded bytes per stem and 350,208 total", async () => {
    const song = await prep([14592, 14592, 14592, 14592]);
    expect(song.frames).toBe(14592);
    for (const s of song.stems) expect(s.pcm24.length).toBe(87552);
    expect(song.audioBytes).toBe(350208);
    expect(song.audioBytes).toBe(14592 * 4 * 2 * 3);
    expect(song.durationSeconds).toBeCloseTo(0.304, 3);
    expect(() => assertCanonicalSong(song)).not.toThrow();
  });

  it("packs exact signed 24-bit little-endian boundaries", () => {
    const l = new Float32Array([1, -1, 0, 0.5]);
    const r = new Float32Array([-1, 1, 0, -0.5]);
    const pcm = packStereo24(l, r, 4);
    expect(pcm.length).toBe(4 * 6);
    expect(Array.from(pcm.slice(0, 6))).toEqual([0xff, 0xff, 0x7f, 0x00, 0x00, 0x80]);
    expect(readInt24LE(pcm, 0)).toBe(8388607);
    expect(readInt24LE(pcm, 3)).toBe(-8388608);
    expect(readInt24LE(pcm, 12)).toBe(0);
    expect(readInt24LE(pcm, 18)).toBe(Math.round(0.5 * 8388607));
  });

  it("preserves stereo: left and right differ frame by frame", async () => {
    const song = await prep([256, 256, 256, 256]);
    const pcm = song.stems[0]!.pcm24;
    let differing = 0;
    for (let i = 0; i < 256; i++) if (readInt24LE(pcm, i * 6) !== readInt24LE(pcm, i * 6 + 3)) differing++;
    expect(differing).toBeGreaterThan(200);
  });

  it("zero-pads unequal lengths to a shared N", async () => {
    const song = await prep([1000, 700, 512, 300]);
    expect(song.frames).toBe(1000);
    for (const s of song.stems) expect(s.pcm24.length).toBe(1000 * 6);
    const shortest = song.stems[3]!;
    expect(shortest.padFrames).toBe(700);
    for (let i = 300; i < 1000; i++) expect(readInt24LE(shortest.pcm24, i * 6)).toBe(0);
    expect(song.lengthSpreadSeconds).toBeCloseTo(700 / 48000, 6);
  });

  it("checksums and peak are computed on the final 24-bit values", async () => {
    const song = await prep([512, 512, 512, 512]);
    for (const s of song.stems) {
      expect(s.checksum).toBe(checksum32(s.pcm24));
      expect(s.peak).toBeGreaterThan(0);
      expect(s.peak).toBeLessThanOrEqual(1);
      expect(s.clipped).toBe(false);
    }
  });

  it("rejects incomplete metadata", async () => {
    const bad = (await prep([128, 128, 128, 128])) as CanonicalSong;
    bad.metadata = { ...bad.metadata, bpm: 0 };
    expect(() => assertCanonicalSong(bad)).toThrow(/bpm is required/);
    const noDownbeat = (await prep([128, 128, 128, 128])) as CanonicalSong;
    noDownbeat.metadata = { ...noDownbeat.metadata, downbeatSeconds: Number.NaN };
    expect(() => assertCanonicalSong(noDownbeat)).toThrow(/downbeat is required/);
  });

  it("rejects a mono or wrong-length packing", async () => {
    const song = await prep([128, 128, 128, 128]);
    song.stems[0]!.pcm24 = song.stems[0]!.pcm24.slice(0, 128 * 2);
    expect(() => assertCanonicalSong(song)).toThrow(/bytes !== 768/);
  });
});

describe("fail-closed compatibility", () => {
  it("has no generated protocol package yet", () => {
    expect(GENERATED_PROTOCOL).toBeNull();
  });

  it("classic SP1XFER alone cannot unlock writes", () => {
    const v = legacyVerdict();
    expect(v.physicalMutationAllowed).toBe(false);
    expect(v.summary).toBe(LOCK_NOTICE);
    expect(v.requirements.filter((r) => r.satisfied)).toHaveLength(0);
  });

  it("a partial capability response stays read-only", () => {
    const v = negotiate({
      deviceIdentity: "STEM TAPE SP-1",
      protocolVersion: "stem-tape-transfer/1",
      storageLayoutVersion: "stem-tape-storage/1",
      formatIdentifier: "pcm-s24le-48000-2ch",
      capabilities: {
        stereo48k24bit: true,
        fourStems: true,
        transactionalCommit: true,
        resume: true,
        metadata: true,
        metadataBpm: false, // missing BPM storage
        metadataDownbeat: false,
        safeInitialise: true,
      },
      addressUnits: { sectorBytes: 4096, transportChunkBytes: 512, resumeUnit: "sector" },
      maxTransferBytes: 65536,
      libraryGeneration: 3,
      songCapacity: 8,
    });
    expect(v.physicalMutationAllowed).toBe(false);
    expect(v.requirements.find((r) => r.id === "metadata")!.satisfied).toBe(false);
  });
});

describe("transport mutation lock", () => {
  async function transport(kind: "mock" | "physical") {
    const mock = new MockSp1();
    const port = mock.port();
    const io = new Sp1Transport(port);
    const session = new Sp1Session(io);
    const layout = await session.handshake(4);
    const raw = await session.readBlock(0);
    const joined =
      metaBlockCount(layout) === 1
        ? raw
        : (() => {
            const j = new Uint8Array(1024);
            j.set(raw, 0);
            return j;
          })();
    const m = parseMeta(joined, layout);
    return { mock, port, t: new LegacyProvisionalTransport(session, m, { kind }, legacyVerdict()) };
  }

  it("a physical device is read-only for every mutating operation", async () => {
    const { t, mock } = await transport("physical");
    expect(t.mutationLocked).toBe(true);
    expect(t.provisional).toBe(true);
    const song = await prep([256, 256, 256, 256]);
    await expect(t.initialiseLibrary()).rejects.toThrow(MutationLockedError);
    await expect(t.writeSong({ slot: 0, song })).rejects.toThrow(MutationLockedError);
    await expect(t.deleteSong(0)).rejects.toThrow(MutationLockedError);
    expect(mock.writes).toBe(0);
    expect(mock.flushes).toBe(0);
    await expect(t.listSongs()).resolves.toHaveLength(8);
  });

  it("mock transport writes and reports mock-only verification language", async () => {
    const { t } = await transport("mock");
    expect(t.mutationLocked).toBe(false);
    const song = await prep([256, 256, 256, 256]);
    const out = await t.writeSong({ slot: 0, song });
    expect(out.ok).toBe(true);
    expect(out.detail).toBe("Mock protocol smoke passed");
    expect(out.durableCommitAcknowledged).toBe(false);
    expect(out.independentReReadMatches).toBe(false);
  });

  it("a real serial port cannot claim mock status", () => {
    expect(portIsMock({} as never)).toBe(false);
  });
});
