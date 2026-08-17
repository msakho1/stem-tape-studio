/**
 * C. END-TO-END WITH FOUR REAL WAV FILES
 *
 * Four deterministic WAV fixtures on disk (differing sample rate, channel count
 * and length) are decoded, prepared and uploaded through the production path to
 * a mock SP-1. Every byte count, block count, command ordering fact and index
 * field is asserted against the bytes the mock device actually holds.
 */
import { createHash } from "node:crypto";
import { describe, expect, it } from "vitest";
import { MockSp1 } from "./mockSerial";
import { Recorder, recordPort } from "./recordingPort";
import { decodeWavFixture, offlineStub } from "./fixtureWav";
import { Sp1Session, Sp1Transport, type SerialLikePort } from "../protocol";
import { parseCapabilities } from "../compatibility";
import { StemTapeTransport } from "../transport";
import { prepareCanonicalSong, assertCanonicalSong, checksum32 } from "../song";
import { encodeSong } from "../sector";
import { parseStemIndex } from "../stemIndex";
import {
  BLOCKS_PER_SECTOR,
  FRAMES_PER_SECTOR,
  INDEX_MAGIC,
  PHYSICAL_BLOCK_BYTES,
  SECTOR_BYTES,
  indexBlockCount,
  sectorsForFrames,
} from "../stemTapeFormat";

const NAMES = ["vocal", "drums", "bass", "instrument"] as const;
const META = { title: "Fixture Song", artist: "Stem Tape Tests", bpm: 96, downbeatSeconds: 0.25 };
const SECTORS_PER_SONG = 64;

async function prepared() {
  const inputs = NAMES.map((name) => ({
    name,
    filename: `${name}.wav`,
    buffer: decodeWavFixture(name),
  }));
  return {
    inputs,
    song: await prepareCanonicalSong(inputs, { metadata: META, make: offlineStub }),
  };
}

async function connect(mock: MockSp1, rec: Recorder) {
  const io = new Sp1Transport(recordPort(mock.port(), rec) as SerialLikePort);
  const session = new Sp1Session(io);
  await session.handshake();
  const caps = parseCapabilities((await session.queryCapabilities())!);
  const t = new StemTapeTransport(session, caps, { kind: "mock" });
  await t.initialiseLibrary();
  return { session, t };
}

describe("four real WAV fixtures, end to end", () => {
  it("decodes fixtures with the declared source properties", () => {
    const props = NAMES.map((n) => {
      const b = decodeWavFixture(n);
      return { n, rate: b.sampleRate, ch: b.numberOfChannels, frames: b.length };
    });
    expect(props).toEqual([
      { n: "vocal", rate: 48000, ch: 2, frames: 14592 },
      { n: "drums", rate: 44100, ch: 2, frames: 13000 },
      { n: "bass", rate: 48000, ch: 1, frames: 14000 },
      { n: "instrument", rate: 44100, ch: 1, frames: 11025 },
    ]);
  });

  it("prepares one canonical song: shared N, exact byte counts, per-stem padding", async () => {
    const { song } = await prepared();
    expect(song.sampleRate).toBe(48000);
    expect(song.channels).toBe(2);
    expect(song.pcmDepth).toBe(24);
    // Longest source after resampling: vocal, 14592 frames @ 48 kHz.
    expect(song.frames).toBe(14592);
    for (const s of song.stems) {
      expect(s.pcm24.length).toBe(song.frames * 2 * 3);
      expect(s.pcm24.length).toBe(87552);
    }
    expect(song.audioBytes).toBe(350208);
    // 44.1 kHz sources were resampled, mono sources duplicated, all padded to N.
    const pad = Object.fromEntries(song.stems.map((s) => [s.name, s.padFrames]));
    expect(pad["vocal"]).toBe(0);
    expect(pad["drums"]).toBe(14592 - Math.ceil((13000 / 44100) * 48000));
    expect(pad["bass"]).toBe(592);
    expect(pad["instrument"]).toBe(14592 - Math.ceil((11025 / 44100) * 48000));
    expect(() => assertCanonicalSong(song)).not.toThrow();
  });

  it("uploads to a mock SP-1 and every device byte matches the prepared song", async () => {
    const { song } = await prepared();
    const mock = new MockSp1({ stemTape: true, sectorsPerSong: SECTORS_PER_SONG });
    const rec = new Recorder();
    const { t } = await connect(mock, rec);

    const sectors = encodeSong(song);
    const expectedSectors = sectorsForFrames(song.frames);
    expect(sectors.length).toBe(expectedSectors);
    expect(expectedSectors).toBe(Math.ceil(14592 / FRAMES_PER_SECTOR));
    expect(expectedSectors).toBe(43);
    for (const s of sectors) expect(s.length).toBe(SECTOR_BYTES);

    rec.op("--upload--");
    const res = await t.uploadSong({ slot: 1, song });

    expect(res.ok).toBe(true);
    expect(res.outcome).toBe("committed");
    expect(res.sectorCount).toBe(43);
    expect(res.totalBlocks).toBe(43 * BLOCKS_PER_SECTOR);
    expect(res.totalBlocks).toBe(688);
    expect(res.writtenBlocks).toBe(688);
    expect(res.verifiedBlocks).toBe(688);
    expect(res.bytesWritten).toBe(688 * PHYSICAL_BLOCK_BYTES);
    expect(res.retries).toBe(0);
    // Mock run: simulated only. No readback/playback claim.
    expect(res.verification).toEqual({
      simulatedVerification: true,
      deviceReadbackVerification: false,
      physicalPlaybackVerification: false,
    });

    // Device memory equals the encoded sectors, byte for byte.
    const caps = t.caps!;
    const base = caps.libraryBase + 1 * caps.sectorsPerSong * BLOCKS_PER_SECTOR;
    const deviceAudio = new Uint8Array(43 * SECTOR_BYTES);
    for (let i = 0; i < 688; i++) deviceAudio.set(mock.block(base + i), i * PHYSICAL_BLOCK_BYTES);
    const encoded = new Uint8Array(43 * SECTOR_BYTES);
    sectors.forEach((s, i) => encoded.set(s, i * SECTOR_BYTES));
    const digest = (u: Uint8Array) => createHash("sha256").update(u).digest("hex");
    expect(digest(deviceAudio)).toBe(digest(encoded));

    // The index the device holds names this song and carries the metadata.
    const idxBlocks = indexBlockCount(caps.songSlots);
    const rawIndex = new Uint8Array(idxBlocks * PHYSICAL_BLOCK_BYTES);
    for (let i = 0; i < idxBlocks; i++) rawIndex.set(mock.block(i), i * PHYSICAL_BLOCK_BYTES);
    expect(new DataView(rawIndex.buffer).getUint32(0, true)).toBe(INDEX_MAGIC);
    const idx = parseStemIndex(rawIndex, caps.songSlots)!;
    const entry = idx.songs[1]!;
    expect(entry.committed).toBe(true);
    expect(entry.title).toBe(META.title);
    expect(entry.artist).toBe(META.artist);
    expect(entry.bpm).toBe(META.bpm);
    expect(entry.frames).toBe(14592);
    expect(entry.sampleRate).toBe(48000);
    expect(entry.channels).toBe(2);
    expect(entry.bitDepth).toBe(24);
    expect(entry.sectorCount).toBe(43);
    expect(entry.stemChecksums).toEqual(song.stems.map((s) => s.checksum));
    expect(idx.songs[0]!.committed).toBe(false);

    // Checksums recomputed from the device bytes match the prepared stems.
    expect(res.stemChecksums).toEqual(song.stems.map((s) => s.checksum));
    expect(res.songChecksum).toBe(
      checksum32(
        Uint8Array.from(res.stemChecksums.flatMap((v) => [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255])),
      ),
    );

    // Ordering: the authoritative index magic is the LAST write on the wire.
    const uploadTx = rec.entries
      .slice(rec.entries.findIndex((e) => e.note === "--upload--"))
      .filter((e) => e.dir === "tx")
      .map((e) => e.hex!);
    const writes = uploadTx.filter((h) => h.startsWith("57"));
    // 688 audio + 3 continuation index blocks + index block 0 (magic zeroed) + magic block 0
    expect(writes.length).toBe(688 + idxBlocks + 1);
    const lastWrite = writes.at(-1)!;
    expect(lastWrite.slice(0, 10)).toBe("5700000000"); // block 0
    expect(lastWrite.slice(10, 18)).toBe("58495453"); // 'STIX' LE
    expect(uploadTx.at(-1)).toBe("46"); // final flush after the magic
  }, 60000);

  it("listSongs reports the uploaded slot from the device index only", async () => {
    const { song } = await prepared();
    const mock = new MockSp1({ stemTape: true, sectorsPerSong: SECTORS_PER_SONG });
    const { t } = await connect(mock, new Recorder());
    await t.uploadSong({ slot: 0, song });
    await t.readIndex();
    const songs = await t.listSongs();
    expect(songs[0]).toMatchObject({
      index: 0,
      occupied: true,
      title: META.title,
      artist: META.artist,
      frames: 14592,
      sectorCount: 43,
    });
    expect(songs[0]!.durationSeconds).toBeCloseTo(0.304, 3);
    expect(songs.filter((s) => s.occupied)).toHaveLength(1);
  }, 60000);
});
