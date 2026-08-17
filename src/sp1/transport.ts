/**
 * StemTapeDeviceTransport — the only module that owns device addressing,
 * command sequencing, index serialization and commit ordering.
 *
 * The byte-level transport underneath is the unchanged Tape Looper block
 * protocol (see src/sp1/protocol.ts). Stem Tape adds, above it:
 *   - 8,192-byte logical song sectors written as sixteen 512-byte blocks
 *   - the versioned Stem Tape index extension (magic-last, multi-block)
 *   - the 'Q' capability gate that must pass before any mutation
 *
 * No React route and no audio-preparation module constructs command bytes,
 * block addresses or index bytes.
 */

import {
  evaluate,
  readOnlyVerdict,
  type CompatibilityVerdict,
  type StemTapeCapabilities,
} from "./compatibility";
import { BLOCK_BYTES, type Sp1Session } from "./protocol";
import {
  BLOCKS_PER_SECTOR,
  CHANNELS,
  FORMAT_MAJOR,
  PCM_BIT_DEPTH,
  SAMPLE_RATE,
  sectorsForFrames,
} from "./stemTapeFormat";
import { blocksToSector, decodeSectors, encodeSong, sectorToBlocks } from "./sector";
import {
  buildStemIndex,
  emptyStemIndex,
  indexIsValid,
  parseStemIndex,
  splitIndexBlocks,
  EMPTY_ENTRY,
  type StemTapeIndex,
} from "./stemIndex";
import { checksum32, type CanonicalSong } from "./song";

export type UploadStage =
  | "preparing"
  | "capacity"
  | "writing"
  | "verifying"
  | "checksums"
  | "metadata"
  | "committing"
  | "confirming"
  | "complete";

export interface UploadProgress {
  stage: UploadStage;
  fraction: number;
  detail: string;
}

export interface DeviceSongSlot {
  index: number;
  occupied: boolean;
  title: string;
  artist: string;
  bpm: number;
  frames: number;
  durationSeconds: number;
  bytes: number;
  sectorCount: number;
}

export interface UploadResult {
  ok: boolean;
  detail: string;
  writtenBlocks: number;
  verifiedBlocks: number;
  retries: number;
  /** True only when a physical device acknowledged the durable commit AND the
   *  independent re-read matched. Mock runs never set this. */
  hardwareVerified: boolean;
  failure?: { operation: string; block: number } | undefined;
}

export class ReadOnlyDeviceError extends Error {
  constructor(op: string) {
    super(`${op} is refused: this device is not a compatible Stem Tape device and stays read-only.`);
  }
}

export const MAX_CHUNK_RETRIES = 3;

export interface TransportMode {
  /** "mock" = in-process fixture; "physical" = real Web Serial port. */
  kind: "mock" | "physical";
}

export class StemTapeTransport {
  readonly verdict: CompatibilityVerdict;
  private index: StemTapeIndex | null = null;

  constructor(
    readonly session: Sp1Session,
    readonly caps: StemTapeCapabilities | null,
    readonly mode: TransportMode,
  ) {
    this.verdict = caps ? evaluate(caps) : readOnlyVerdict();
  }

  get writable(): boolean {
    return this.verdict.writable;
  }

  get staging(): boolean {
    return this.verdict.staging;
  }

  describe() {
    const l = this.session.layout!;
    const c = this.caps;
    return {
      deviceName: this.writable ? "Stem Tape SP-1" : "SP-1 (stock / Tape Looper firmware)",
      transport: "Tape Looper block protocol · 512-byte blocks @ 115200",
      audioFormat: this.writable ? "48 kHz · stereo · signed 24-bit · 8 KiB logical sectors" : "mono int16 (Tape Looper)",
      slots: c?.songSlots ?? l.numSlots,
      libraryBase: c?.libraryBase ?? l.slot0,
      indexBlocks: c?.indexBlocks ?? 0,
      sectorsPerSong: c?.sectorsPerSong ?? 0,
      generation: this.index?.generation ?? c?.generation ?? 0,
      capacityBytesPerSong: (c?.sectorsPerSong ?? 0) * BLOCKS_PER_SECTOR * BLOCK_BYTES,
      staging: this.staging,
      writable: this.writable,
    };
  }

  /* ---------- addressing (device-reported only) ---------- */

  private songBlock(slot: number, sectorIndex: number): number {
    const c = this.caps!;
    return c.libraryBase + (slot * c.sectorsPerSong + sectorIndex) * BLOCKS_PER_SECTOR;
  }

  /* ---------- index ---------- */

  async readIndex(): Promise<StemTapeIndex | null> {
    const c = this.caps;
    if (!c) return null;
    const raw = await this.session.lock.run(async () => {
      const out = new Uint8Array(c.indexBlocks * BLOCK_BYTES);
      for (let i = 0; i < c.indexBlocks; i++) out.set(await this.session.readBlock(i), i * BLOCK_BYTES);
      return out;
    });
    const parsed = parseStemIndex(raw, c.songSlots);
    this.index = parsed;
    return parsed;
  }

  get indexInitialised(): boolean {
    return !!this.index && indexIsValid(this.index, FORMAT_MAJOR);
  }

  async listSongs(): Promise<DeviceSongSlot[]> {
    const idx = this.index ?? (await this.readIndex());
    if (!idx || !this.indexInitialised) return [];
    return idx.songs.map((e, index) => ({
      index,
      occupied: e.committed,
      title: e.title,
      artist: e.artist,
      bpm: e.bpm,
      frames: e.frames,
      durationSeconds: e.sampleRate ? e.frames / e.sampleRate : 0,
      bytes: e.sectorCount * BLOCKS_PER_SECTOR * BLOCK_BYTES,
      sectorCount: e.sectorCount,
    }));
  }

  /**
   * Explicit, user-confirmed initialization only. Never called from connect or
   * upload. Writes continuation blocks first, authoritative magic block last.
   */
  async initialiseLibrary(): Promise<void> {
    if (!this.writable) throw new ReadOnlyDeviceError("initialization");
    const c = this.caps!;
    const fresh = emptyStemIndex(c.songSlots, c.sectorsPerSong, FORMAT_MAJOR, c.formatMinor);
    await this.commitIndex(fresh);
    await this.readIndex();
  }

  /** Continuation blocks first, flush, then the block holding magic + generation, flush. */
  private async commitIndex(index: StemTapeIndex): Promise<void> {
    const session = this.session;
    await session.lock.run(async () => {
      const pending = splitIndexBlocks(buildStemIndex(index, false));
      for (let i = 1; i < pending.length; i++) await session.writeBlock(i, pending[i]!);
      await session.flush();
      const authoritative = splitIndexBlocks(buildStemIndex(index, true));
      await session.writeBlock(0, authoritative[0]!);
      await session.flush();
    });
  }

  private async writeWithRetry(blk: number, data: Uint8Array, counter: { retries: number }) {
    let last: unknown = null;
    for (let attempt = 0; attempt <= MAX_CHUNK_RETRIES; attempt++) {
      try {
        await this.session.writeBlock(blk, data);
        return;
      } catch (e) {
        last = e;
        counter.retries++;
      }
    }
    throw new Error(`block ${blk} failed after ${MAX_CHUNK_RETRIES} retries: ${String(last)}`);
  }

  /* ---------- upload ---------- */

  async uploadSong(args: {
    slot: number;
    song: CanonicalSong;
    signal?: { aborted: boolean };
    onProgress?: (p: UploadProgress) => void;
  }): Promise<UploadResult> {
    if (!this.writable) throw new ReadOnlyDeviceError("upload");
    const { slot, song } = args;
    const c = this.caps!;
    const report = (stage: UploadStage, fraction: number, detail: string) =>
      args.onProgress?.({ stage, fraction, detail });
    const counter = { retries: 0 };
    let written = 0;
    let verified = 0;
    let failure: { operation: string; block: number } | undefined;
    const abort = () => {
      if (args.signal?.aborted) throw new Error("cancelled before commit — the previous song is still authoritative");
    };

    try {
      // 3. Read and preserve the existing index.
      const previous = this.index ?? (await this.readIndex());
      if (!previous || !indexIsValid(previous, FORMAT_MAJOR)) {
        throw new Error("this Stem Tape device has no valid index — initialize it explicitly first");
      }

      // 4/5. Sectors and capacity.
      report("preparing", 0.05, "encoding logical sectors");
      const sectors = encodeSong(song);
      const need = sectorsForFrames(song.frames);
      report("capacity", 0.1, `${need} of ${c.sectorsPerSong} sectors per song slot`);
      if (need > c.sectorsPerSong) {
        throw new Error(`song needs ${need} logical sectors; this slot holds ${c.sectorsPerSong}`);
      }
      if (previous.songs[slot]?.committed && !this.staging) {
        report(
          "capacity",
          0.1,
          "device reports no staging/copy-on-write: replacing this song is NOT interruption-safe",
        );
      }

      // 6. Audio through the unchanged 512-byte block primitive.
      const totalBlocks = sectors.length * BLOCKS_PER_SECTOR;
      for (let s = 0; s < sectors.length; s++) {
        const blocks = sectorToBlocks(sectors[s]!);
        for (let k = 0; k < BLOCKS_PER_SECTOR; k++) {
          abort();
          const blk = this.songBlock(slot, s) + k;
          failure = { operation: "write", block: blk };
          await this.writeWithRetry(blk, blocks[k]!, counter);
          written++;
          if (written % 16 === 0 || written === totalBlocks) {
            report("writing", written / totalBlocks, `sector ${s + 1}/${sectors.length} · block ${blk}`);
          }
        }
      }

      // 7. Read every written block back and verify.
      const readBack: Uint8Array[] = [];
      for (let s = 0; s < sectors.length; s++) {
        const got: Uint8Array[] = [];
        for (let k = 0; k < BLOCKS_PER_SECTOR; k++) {
          abort();
          const blk = this.songBlock(slot, s) + k;
          failure = { operation: "read-back", block: blk };
          const back = await this.session.readBlock(blk);
          const expect = sectors[s]!.subarray(k * BLOCK_BYTES, (k + 1) * BLOCK_BYTES);
          for (let i = 0; i < BLOCK_BYTES; i++) {
            if (back[i] !== expect[i]) throw new Error(`read-back mismatch at block ${blk}, byte ${i}`);
          }
          got.push(back);
          verified++;
          report("verifying", verified / totalBlocks, `block ${blk}`);
        }
        readBack.push(blocksToSector(got));
      }

      // 8. Per-stem and song checksums, recomputed from what the device holds.
      failure = undefined;
      report("checksums", 0.9, "recomputing per-stem checksums from device data");
      const decoded = decodeSectors(readBack, song.frames);
      const stemChecksums = decoded.stems.map((s) => checksum32(s));
      for (let t = 0; t < 4; t++) {
        if (stemChecksums[t] !== song.stems[t]!.checksum) {
          throw new Error(`checksum mismatch on ${song.stems[t]!.name}: device data does not match the prepared stem`);
        }
      }
      const songChecksum = checksum32(
        Uint8Array.from(stemChecksums.flatMap((v) => [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255])),
      );
      if (songChecksum !== song.checksum) throw new Error("song-level checksum mismatch");

      // 9-12. Metadata continuation blocks, flush, authoritative magic last, flush.
      report("metadata", 0.93, "writing metadata continuation blocks");
      const next: StemTapeIndex = {
        ...previous,
        generation: previous.generation + 1,
        currentSong: slot,
        songs: previous.songs.map((e, i) =>
          i === slot
            ? {
                ...EMPTY_ENTRY,
                committed: true,
                startSector: slot * c.sectorsPerSong,
                sectorCount: sectors.length,
                sampleRate: SAMPLE_RATE,
                channels: CHANNELS,
                bitDepth: PCM_BIT_DEPTH,
                frames: song.frames,
                originalFrames: song.stems.map((s) => s.originalFrames),
                stemChecksums,
                songChecksum,
                bpm: song.metadata.bpm,
                downbeatFrame: Math.round(song.metadata.downbeatSeconds * SAMPLE_RATE),
                title: song.metadata.title,
                artist: song.metadata.artist,
              }
            : e,
        ),
      };
      report("committing", 0.96, "authoritative index block written last");
      await this.commitIndex(next);

      // 13/14. Re-read the committed index and confirm the slot matches.
      report("confirming", 0.98, "re-reading the committed index");
      const after = await this.readIndex();
      const e = after?.songs[slot];
      const matches =
        !!after &&
        indexIsValid(after, FORMAT_MAJOR) &&
        !!e &&
        e.committed &&
        e.frames === song.frames &&
        e.songChecksum === songChecksum &&
        e.stemChecksums.every((v, i) => v === stemChecksums[i]) &&
        e.title === song.metadata.title &&
        e.artist === song.metadata.artist &&
        Math.abs(e.bpm - song.metadata.bpm) < 1 / 256 &&
        e.downbeatFrame === Math.round(song.metadata.downbeatSeconds * SAMPLE_RATE);
      if (!matches) throw new Error("the committed index did not match after re-read");

      const hardwareVerified = this.mode.kind === "physical";
      report(
        "complete",
        1,
        hardwareVerified
          ? "Committed and re-read on hardware."
          : "Mock device: protocol smoke passed. Physical SP-1 upload NOT verified.",
      );
      return {
        ok: true,
        detail: hardwareVerified
          ? "Upload committed and independently re-read on the SP-1."
          : "Mock protocol smoke passed · physical SP-1 upload not verified",
        writtenBlocks: written,
        verifiedBlocks: verified,
        retries: counter.retries,
        hardwareVerified,
      };
    } catch (err) {
      const detail = err instanceof Error ? err.message : String(err);
      return {
        ok: false,
        detail,
        writtenBlocks: written,
        verifiedBlocks: verified,
        retries: counter.retries,
        hardwareVerified: false,
        failure,
      };
    }
  }

  async deleteSong(slot: number): Promise<void> {
    if (!this.writable) throw new ReadOnlyDeviceError("delete");
    const idx = this.index ?? (await this.readIndex());
    if (!idx) throw new Error("no index");
    const next: StemTapeIndex = {
      ...idx,
      generation: idx.generation + 1,
      songs: idx.songs.map((e, i) => (i === slot ? { ...EMPTY_ENTRY, originalFrames: [0, 0, 0, 0], stemChecksums: [0, 0, 0, 0] } : e)),
    };
    await this.commitIndex(next);
    await this.readIndex();
  }

  /** 'X' clean exit, then every stream/port lock released. */
  async disconnect(): Promise<void> {
    this.session.stopKeepalive();
    await this.session.exit();
    await this.session.io.close();
  }
}
