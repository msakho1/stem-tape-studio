import { describe, it, expect } from "vitest";
import { MockSp1, type MockOptions } from "./mockSerial";
import { Sp1Transport, Sp1Session } from "../protocol";
import {
  prepareCanonicalSong,
  assertCanonicalSong,
  packStereo24,
  readInt24LE,
  checksum32,
  CANONICAL_SAMPLE_RATE,
  type CanonicalSong,
} from "../song";
import { evaluate, parseCapabilities, readOnlyVerdict, READ_ONLY_NOTICE } from "../compatibility";
import { ReadOnlyDeviceError, StemTapeTransport } from "../transport";
import {
  CAP_FLAG,
  BYTES_PER_STEM_FRAME,
  FRAMES_PER_GROUP,
  GROUP_FLAGS_V13,
  GROUP_BYTES,
  GROUP_HEADER_BYTES,
  REQUIRED_CAP_FLAGS,
  SECTOR_BYTES,
  groupsForFrames,
  sectorsForFrames,
} from "../stemTapeFormat";
import { blocksToSector, decodeSectors, encodeSong, readGroupHeader, sectorToBlocks, sectorToGroups } from "../sector";

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
    expect(Array.from(pcm.slice(0, 6))).toEqual([0xff, 0xff, 0x7f, 0x01, 0x00, 0x80]);
    expect(readInt24LE(pcm, 0)).toBe(8388607);
    expect(readInt24LE(pcm, 3)).toBe(-8388607); // symmetric ±8388607 scaling
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

async function connect(opts: MockOptions = {}) {
  const mock = new MockSp1(opts);
  const io = new Sp1Transport(mock.port());
  const session = new Sp1Session(io);
  await session.handshake(4);
  const raw = await session.queryCapabilities(400);
  const caps = raw ? parseCapabilities(raw) : null;
  return { mock, session, caps, t: new StemTapeTransport(session, caps, { kind: "mock" }) };
}

describe("logical 8 KiB sector mapping", () => {
  it("maps one sector onto sixteen ascending 512-byte blocks and round-trips", async () => {
    const song = await prep([FRAMES_PER_GROUP * 2 + 7]);
    const sectors = encodeSong(song);
    expect(sectors.length).toBe(sectorsForFrames(song.frames));
    expect(sectors[0]!.length).toBe(SECTOR_BYTES);
    const blocks = sectorToBlocks(sectors[0]!);
    expect(blocks).toHaveLength(16);
    expect(blocks.every((b) => b.length === 512)).toBe(true);
    expect(Array.from(blocksToSector(blocks))).toEqual(Array.from(sectors[0]!));

    const decodedSong = decodeSectors(sectors, song.frames);
    for (let t = 0; t < 4; t++) {
      expect(checksum32(decodedSong.stems[t]!)).toBe(checksum32(stemPcm16(song.stems[t]!)));
    }
    // v1.3 planar: three groups per stem, laid out stem-major. Global group
    // stream index g maps to stem = floor(g/3), groupIndex = g % 3.
    const groups = groupsForFrames(song.frames);
    expect(groups).toBe(3);
    const all = sectors.flatMap((s) => sectorToGroups(s));
    expect(all).toHaveLength(4 * groups);
    all.forEach((g, i) => {
      const h = readGroupHeader(g);
      expect(g.length).toBe(GROUP_BYTES);
      expect(h.magicOk).toBe(true);
      expect(h.flags).toBe(GROUP_FLAGS_V13);
      expect(h.stemIndex).toBe(Math.floor(i / groups));
      expect(h.groupIndex).toBe(i % groups);
    });

    // Every stem's last group is partial (7 of 510 frames) and zero-padded.
    for (let stem = 0; stem < 4; stem++) {
      const last = all[stem * groups + (groups - 1)]!;
      const tail = last.subarray(GROUP_HEADER_BYTES + 7 * BYTES_PER_STEM_FRAME);
      expect(tail.every((b) => b === 0)).toBe(true);
    }

    // A stem's timeline is contiguous: its quarter is exactly groups*2048 B.
    for (let stem = 0; stem < 4; stem++) {
      const flat = new Uint8Array(groups * (GROUP_BYTES - GROUP_HEADER_BYTES));
      for (let g = 0; g < groups; g++) {
        flat.set(all[stem * groups + g]!.subarray(GROUP_HEADER_BYTES), g * (GROUP_BYTES - GROUP_HEADER_BYTES));
      }
      expect(Array.from(flat.subarray(0, song.frames * BYTES_PER_STEM_FRAME))).toEqual(
        Array.from(stemPcm16(song.stems[stem]!)),
      );
    }
  });
});

describe("fail-closed compatibility", () => {
  it("SP1XFER alone cannot unlock writes", () => {
    const v = readOnlyVerdict();
    expect(v.writable).toBe(false);
    expect(v.summary).toBe(READ_ONLY_NOTICE);
    expect(v.requirements.filter((r: { satisfied: boolean }) => r.satisfied)).toHaveLength(0);
  });

  it("stock Tape Looper firmware never answers the capability query", async () => {
    const { caps, t, mock } = await connect();
    expect(caps).toBeNull();
    expect(mock.capQueries).toBe(1);
    expect(t.writable).toBe(false);
  });

  it("a missing capability flag keeps the device read-only", async () => {
    const { t } = await connect({ stemTape: true, capFlags: REQUIRED_CAP_FLAGS & ~CAP_FLAG.BPM_DOWNBEAT });
    expect(t.writable).toBe(false);
    expect(t.verdict.requirements.find((r) => r.id === "metadata")!.satisfied).toBe(false);
    const song = await prep([256]);
    await expect(t.uploadSong({ slot: 0, song })).rejects.toThrow(ReadOnlyDeviceError);
    await expect(t.initialiseLibrary()).rejects.toThrow(ReadOnlyDeviceError);
    await expect(t.deleteSong(0)).rejects.toThrow(ReadOnlyDeviceError);
  });

  it("a mismatched format version is refused", () => {
    const v = evaluate({
      firmwareId: 0x53544657,
      protoMajor: 1,
      protoMinor: 1,
      formatMajor: 9,
      formatMinor: 0,
      flags: REQUIRED_CAP_FLAGS,
      sampleRate: 48000,
      blockSize: 512,
      sectorBytes: 8192,
      alignment: 512,
      deviceBlocks: 4096,
      song: [
        { start: 16, blocks: 128 },
        { start: 144, blocks: 128 },
      ],
      index: [
        { start: 0, blocks: 1 },
        { start: 1, blocks: 1 },
      ],
      activeIndexSlot: 0,
      activeSongSlot: 0,
      activeGeneration: 0,
      stixVersion: 2,
    });
    expect(v.writable).toBe(false);
    expect(v.requirements.find((r) => r.id === "format")!.satisfied).toBe(false);
  });
});

describe("Stem Tape transport upload", () => {
  it("refuses to write before the index is explicitly initialised", async () => {
    const { t } = await connect({ stemTape: true });
    expect(t.writable).toBe(true);
    await t.readIndex();
    expect(t.indexInitialised).toBe(false);
    const song = await prep([256]);
    const out = await t.uploadSong({ song });
    expect(out.ok).toBe(false);
    expect(out.detail).toMatch(/initialize it explicitly/);
  });

  it("commits audio, verifies by read-back and re-reads both index slots", async () => {
    const { t, mock } = await connect({ stemTape: true });
    await t.initialiseLibrary();
    expect(t.indexInitialised).toBe(true);
    const song = await prep([FRAMES_PER_GROUP + 40]);
    const stages: string[] = [];
    const out = await t.uploadSong({ song, onProgress: (p) => stages.push(p.stage) });
    expect(out.ok).toBe(true);
    expect(out.verification.deviceReadbackVerification).toBe(false);
    expect(out.verification.physicalPlaybackVerification).toBe(false);
    expect(out.outcome).toBe("committed");
    expect(out.detail).toMatch(/no physical SP-1 involved/);
    expect(out.generation).toBe(2);
    expect(out.previousGeneration).toBe(1);

    expect(out.writtenBlocks).toBe(sectorsForFrames(song.frames) * 16);
    expect(out.verifiedBlocks).toBe(out.writtenBlocks);
    expect(stages).toContain("confirming");
    expect(mock.flushes).toBeGreaterThanOrEqual(2);

    const songs = await t.listSongs();
    const active = songs.find((s) => s.active)!;
    expect(active.occupied).toBe(true);
    expect(active.title).toBe("T");
    expect(active.bpm).toBeCloseTo(120, 3);
    expect(songs.filter((s) => s.occupied)).toHaveLength(1);
  });

  it("retries a NAKed block and still verifies", async () => {
    const { mock, t } = await connect({ stemTape: true, failWriteOnce: [16 + 3] });
    await t.initialiseLibrary();
    const song = await prep([256]);
    const out = await t.uploadSong({ song });
    expect(out.ok).toBe(true);
    expect(out.retries).toBeGreaterThan(0);
    expect(mock.activeLibrary().generation).toBe(2);
  });

  it("keeps the previous generation authoritative when writing is interrupted", async () => {
    const { t } = await connect({ stemTape: true });
    await t.initialiseLibrary();
    const before = (await t.readIndex())!.generation;
    const song = await prep([FRAMES_PER_GROUP * 2]);
    const out = await t.uploadSong({ song, signal: { aborted: true } });
    expect(out.ok).toBe(false);
    const after = await t.readIndex();
    expect(after!.generation).toBe(before);
    expect(after!.requiresInitialization).toBe(false);
  });

  it("refuses a song larger than the inactive staging slot and writes nothing", async () => {
    const { t, mock } = await connect({ stemTape: true, sectorsPerSong: 1 });
    await t.initialiseLibrary();
    const writesBefore = mock.writes;
    const song = await prep([FRAMES_PER_GROUP * 2]);
    const out = await t.uploadSong({ song });
    expect(out.ok).toBe(false);
    expect(out.detail).toMatch(/Insufficient safe staging capacity/);
    expect(out.detail).toMatch(/No data was written/);
    expect(mock.writes).toBe(writesBefore);
  });

  it("deletes the active song by committing a song-free next generation", async () => {
    const { t } = await connect({ stemTape: true });
    await t.initialiseLibrary();
    const song = await prep([256]);
    await t.uploadSong({ song });
    const gen = (await t.readIndex())!.generation;
    await t.deleteSong();
    const lib = (await t.readIndex())!;
    expect(lib.active!.songPresent).toBe(false);
    expect(lib.generation).toBe(gen + 1);
    expect(lib.requiresInitialization).toBe(false);
  });
});
