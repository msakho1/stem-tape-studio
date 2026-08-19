/**
 * StemTapeTransport — the only module that owns device addressing, command
 * sequencing, index serialization and commit ordering.
 *
 * The byte-level transport underneath is the unchanged Tape Looper block
 * protocol (see src/sp1/protocol.ts): SP1XFER!, P/R/W/F/X, 512-byte framing,
 * inherited encodings, inherited response parsing, inherited timing. Stem Tape
 * v1.1 adds, strictly ABOVE that layer:
 *   - 8,192-byte logical song sectors written as sixteen 512-byte blocks
 *   - true A/B song-data and A/B STIX v2 index storage
 *   - generation-based commit with the validity magic written last
 *   - the 'Q' capability gate that must pass before any mutation
 *
 * Crash-safety guarantee implemented here: a replacement upload never touches
 * the active song region or the active index record. At every interruption
 * point either the previous generation or the new generation is a complete,
 * CRC-valid record, so an interrupted replacement can never require
 * reinitialization.
 */

import {
  evaluate,
  parseCapabilities,
  readOnlyVerdict,
  sameCapabilities,
  type CompatibilityVerdict,
  type StemTapeCapabilities,
} from "./compatibility";
import { readSlot, selectActiveIndex, type LibraryState } from "./activeIndex";
import { sha256Hex } from "./digest";
import { BLOCK_BYTES, type Sp1Session } from "./protocol";
import {
  BLOCKS_PER_SECTOR,
  CHANNELS,
  PCM_BIT_DEPTH,
  SAMPLE_RATE,
  SLOT_A,
  SLOT_B,
  otherSlot,
  sectorsForFrames,
  sectorsInRegion,
  slotName,
  type AbSlot,
} from "./stemTapeFormat";
import { blocksToSector, decodeSectors, encodeSong, sectorToBlocks } from "./sector";
import {
  blankIndexDraft,
  buildIndexRecord,
  indexRecordBlock,
  recordsEqualIgnoringMagic,
  type RegionContext,
  type StemTapeIndexDraft,
} from "./stemIndex";
import { checksum32, type CanonicalSong } from "./song";
import { crc32 } from "./crc32";
import {
  BULK_STATUS,
  bulkDestBlock,
  bulkStatusIsRetryable,
  describeBulkStatus,
  type BulkResponse,
} from "./bulkTransfer";

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
  /** Verified 8 KiB sectors so far (bulk path reports one per round trip). */
  sectorsDone?: number;
  sectorsTotal?: number;
  bytesDone?: number;
  bytesTotal?: number;
  /** Automatic retries spent so far. */
  retries?: number;
}

export interface DeviceSongSlot {
  index: number;
  slot: AbSlot;
  name: "A" | "B";
  occupied: boolean;
  active: boolean;
  generation: number;
  title: string;
  artist: string;
  bpm: number;
  frames: number;
  durationSeconds: number;
  bytes: number;
  sectorCount: number;
}

/**
 * Three independent verification facts. There is deliberately no single
 * "hardwareVerified" boolean: simulation, device read-back and physical
 * playback are different claims and are never conflated.
 */
export interface VerificationState {
  simulatedVerification: boolean;
  deviceReadbackVerification: boolean;
  physicalPlaybackVerification: boolean;
}

export type UploadOutcome = "committed" | "failed" | "unknown" | "corrupt";

export interface UploadResult {
  ok: boolean;
  outcome: UploadOutcome;
  detail: string;
  writtenBlocks: number;
  verifiedBlocks: number;
  totalBlocks: number;
  bytesWritten: number;
  sectorCount: number;
  retries: number;
  elapsedMs: number;
  /** SHA-256 of the canonical audio actually transmitted. */
  songSha256: string;
  /** SHA-256 of the committed index record image (256 bytes). */
  indexSha256: string;
  stemChecksums: number[];
  songChecksum: number;
  /** Which A/B regions this upload used. */
  targetSongSlot: AbSlot | null;
  targetIndexSlot: AbSlot | null;
  /** Generation the new record claims. */
  generation: number;
  /** Generation that remained valid as the rollback copy. */
  previousGeneration: number;
  verification: VerificationState;
  failure?: { operation: string; block: number } | undefined;
}

/**
 * One recorded bulk round trip, kept for the diagnostic report. Successful
 * sectors are sampled (first, last, every 64th) and every non-OK response is
 * kept verbatim: the report carries the exact firmware answers without an
 * array of tens of thousands of per-sector CRCs.
 */
export interface BulkTransactionRecord {
  seq: number;
  destBlock: number;
  attempt: number;
  status: number;
  statusText: string;
  declaredCrc32: number;
  verifiedCrc32: number;
  retryable: boolean;
  /** Present instead of a status when no response arrived at all. */
  transportError?: string;
  atMs: number;
  /** Host -> stream write() handoff time (ms). Near-zero unless real
   * backpressure exists; absent on the no-response (transportError) path,
   * since there is no completed round trip to time. */
  writeMs?: number;
  /** Time from write-resolved to the 14-byte response actually arriving
   * (ms) — the real device-side receive + eMMC write + read-back + CRC
   * cost. Absent on the no-response path for the same reason as writeMs. */
  ackMs?: number;
}

/** A bulk refusal that resending the identical request can never fix. */
class FatalBulkError extends Error {}

export class ReadOnlyDeviceError extends Error {
  constructor(op: string) {
    super(`${op} is refused: this device is not a compatible Stem Tape v1.1 device and stays read-only.`);
  }
}

/** Refusal raised before any byte is written when staging capacity is short. */
export class InsufficientStagingCapacityError extends Error {
  constructor(
    readonly requiredSectors: number,
    readonly availableSectors: number,
    slot: AbSlot,
  ) {
    super(
      `Insufficient safe staging capacity: this song needs ${requiredSectors} logical sectors and the inactive song slot ${slotName(slot)} holds ${availableSectors}. The active song is never overwritten to make a replacement fit. No data was written.`,
    );
  }
}

export const MAX_CHUNK_RETRIES = 3;

export interface TransportMode {
  /** "mock" = in-process fixture; "physical" = real Web Serial port. */
  kind: "mock" | "physical";
}

export class StemTapeTransport {
  readonly verdict: CompatibilityVerdict;
  library: LibraryState | null = null;
  /** True once the authoritative magic block may have reached the device. */
  magicAttempted = false;
  /**
   * Test-only override for the bulk acknowledgement timeout. Left undefined in
   * production so `Sp1Session.writeSectorBulk`'s 80,000 ms default (matching
   * the firmware's 64-second payload-receive ceiling) applies unchanged.
   */
  bulkTimeoutMs?: number;

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

  private get regions(): RegionContext {
    const c = this.caps!;
    return { song: c.song, index: c.index };
  }

  /** Whole logical sectors the smaller song region can hold. */
  get sectorsPerSongSlot(): number {
    const c = this.caps;
    if (!c) return 0;
    return Math.min(sectorsInRegion(c.song[0]), sectorsInRegion(c.song[1]));
  }

  describe() {
    const l = this.session.layout!;
    const c = this.caps;
    return {
      deviceName: this.writable ? "Stem Tape SP-1 (v1.1 A/B)" : "SP-1 (stock / Tape Looper firmware)",
      transport: "Tape Looper block protocol · 512-byte blocks @ 115200",
      audioFormat: this.writable
        ? "48 kHz · stereo · signed 24-bit · 8 KiB logical sectors"
        : "mono int16 (Tape Looper)",
      slots: 2,
      songRegions: c ? [c.song[0], c.song[1]] : [],
      indexRegions: c ? [c.index[0], c.index[1]] : [],
      libraryBase: c?.song[0].start ?? l.slot0,
      indexBlocks: c ? c.index[0].blocks : 0,
      sectorsPerSong: this.sectorsPerSongSlot,
      generation: this.library?.generation ?? c?.activeGeneration ?? 0,
      activeIndexSlot: this.library?.activeIndexSlot ?? null,
      activeSongSlot: this.library?.activeSongSlot ?? null,
      capacityBytesPerSong: this.sectorsPerSongSlot * BLOCKS_PER_SECTOR * BLOCK_BYTES,
      staging: this.staging,
      writable: this.writable,
    };
  }

  /* ---------- addressing (device-reported only) ---------- */

  private songBlock(slot: AbSlot, sectorIndex: number): number {
    return this.caps!.song[slot].start + sectorIndex * BLOCKS_PER_SECTOR;
  }
  private indexBlock(slot: AbSlot): number {
    return this.caps!.index[slot].start;
  }

  /* ---------- index ---------- */

  /** Steps 2-4: read index A, read index B, run the one shared selector. */
  async readLibrary(): Promise<LibraryState | null> {
    if (!this.caps) return null;
    const [a, b] = await this.session.lock.run(async () => [
      await this.session.readBlock(this.indexBlock(SLOT_A)),
      await this.session.readBlock(this.indexBlock(SLOT_B)),
    ]);
    const state = selectActiveIndex(
      readSlot(SLOT_A, a!, this.regions),
      readSlot(SLOT_B, b!, this.regions),
    );
    this.library = state;
    return state;
  }

  /** Back-compat alias used by the route. */
  async readIndex(): Promise<LibraryState | null> {
    return this.readLibrary();
  }

  get indexInitialised(): boolean {
    return !!this.library && !this.library.requiresInitialization;
  }

  async listSongs(): Promise<DeviceSongSlot[]> {
    const lib = this.library ?? (await this.readLibrary());
    if (!lib || lib.requiresInitialization) return [];
    return ([SLOT_A, SLOT_B] as AbSlot[]).map((slot) => {
      const readings = lib.slots.filter((s) => s.validation.valid && s.record.songPresent && s.record.songSlot === slot);
      const best = readings.sort((x, y) => y.record.generation - x.record.generation)[0];
      const r = best?.record;
      return {
        index: slot,
        slot,
        name: slotName(slot),
        occupied: !!r,
        active: lib.activeSongSlot === slot,
        generation: r?.generation ?? 0,
        title: r?.title ?? "",
        artist: r?.artist ?? "",
        bpm: r?.bpm ?? 0,
        frames: r?.frames ?? 0,
        durationSeconds: r && r.sampleRate ? r.frames / r.sampleRate : 0,
        bytes: (r?.sectorCount ?? 0) * BLOCKS_PER_SECTOR * BLOCK_BYTES,
        sectorCount: r?.sectorCount ?? 0,
      };
    });
  }

  /**
   * Explicit, user-confirmed initialization only. Never called from connect or
   * upload, and only legal when BOTH index slots are blank or invalid.
   *
   * Creates one valid empty STIX v2 record at generation 1 in index slot A and
   * leaves index slot B explicitly empty/invalid. No false committed song entry
   * is created, and both song regions stay available for the first upload.
   */
  async initialiseLibrary(): Promise<LibraryState> {
    if (!this.writable) throw new ReadOnlyDeviceError("initialization");
    const lib = this.library ?? (await this.readLibrary());
    if (lib && !lib.requiresInitialization) {
      throw new Error(
        "initialization refused: this device already holds a valid index. Initialization would discard a valid song.",
      );
    }
    const draft = blankIndexDraft(SLOT_A, SLOT_A, 1);
    await this.session.lock.run(async () => {
      // Secondary slot first, explicitly zeroed (invalid, never selectable).
      await this.session.writeBlock(this.indexBlock(SLOT_B), new Uint8Array(BLOCK_BYTES));
      await this.session.writeBlock(this.indexBlock(SLOT_A), indexRecordBlock(draft, false));
      await this.session.flush();
      await this.session.writeBlock(this.indexBlock(SLOT_A), indexRecordBlock(draft, true));
      await this.session.flush();
    });
    const after = await this.readLibrary();
    if (!after || after.requiresInitialization) throw new Error("initialization did not produce a valid index");
    return after;
  }

  /**
   * True only when the device's own 'Q' reply carried the "STBC" extension
   * with the supported flag set. Never inferred from a version number.
   */
  get bulkSupported(): boolean {
    return this.session.bulkCaps?.supported === true;
  }

  private readonly bulkRecords: BulkTransactionRecord[] = [];

  private record(r: BulkTransactionRecord) {
    this.bulkRecords.push(r);
    if (this.bulkRecords.length > 400) this.bulkRecords.splice(0, this.bulkRecords.length - 400);
  }

  /** Sampled successes plus every verbatim non-OK firmware response. */
  get bulkTransactions(): readonly BulkTransactionRecord[] {
    return this.bulkRecords;
  }

  /**
   * One bulk sector with bounded automatic retries. A lost acknowledgement or
   * a transient device failure is retried by resending the IDENTICAL request:
   * the wire contract makes that idempotent (same seq, same destination, same
   * bytes), and the device answers the repeat as a legal retry. A structural
   * refusal is never retried — resending it verbatim can never succeed.
   */
  private async bulkWithRetry(
    seq: number,
    destBlock: number,
    payload: Uint8Array,
    expectCrc: number,
    counter: { retries: number },
    total = 0,
  ): Promise<BulkResponse> {
    let last = "";
    for (let attempt = 0; attempt <= MAX_CHUNK_RETRIES; attempt++) {
      try {
        const resp = await this.session.writeSectorBulk(
          seq,
          destBlock,
          payload,
          ...(this.bulkTimeoutMs === undefined ? [] : [this.bulkTimeoutMs]),
        );
        const sampled = attempt > 0 || seq === 0 || seq === total - 1 || seq % 64 === 0;
        if (resp.status !== BULK_STATUS.OK || sampled) {
          this.record({
            seq,
            destBlock,
            attempt,
            status: resp.status,
            statusText: describeBulkStatus(resp.status),
            declaredCrc32: expectCrc,
            verifiedCrc32: resp.verifiedCrc32,
            retryable: resp.retryable,
            atMs: Date.now(),
            writeMs: resp.writeMs,
            ackMs: resp.ackMs,
          });
        }
        if (resp.status === BULK_STATUS.OK && resp.verifiedCrc32 === expectCrc) return resp;
        last =
          resp.status === BULK_STATUS.OK
            ? "the sector did not read back correctly on the SP-1"
            : describeBulkStatus(resp.status);
        const retryable = resp.status === BULK_STATUS.OK ? true : resp.retryable && bulkStatusIsRetryable(resp.status);
        if (!retryable) throw new FatalBulkError(last);
        counter.retries++;
      } catch (e) {
        if (e instanceof FatalBulkError) throw new Error(e.message);
        // A timeout means the acknowledgement (or the request) was lost. The
        // identical request is safe to resend: same seq, same block, same bytes.
        last = e instanceof Error ? e.message : String(e);
        this.record({
          seq,
          destBlock,
          attempt,
          status: -1,
          statusText: "no acknowledgement — the identical transaction is being resent",
          declaredCrc32: expectCrc,
          verifiedCrc32: 0,
          retryable: true,
          transportError: last,
          atMs: Date.now(),
        });
        counter.retries++;
      }
    }
    throw new Error(`sector ${seq + 1} failed after ${MAX_CHUNK_RETRIES} retries: ${last}`);
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

  /**
   * Immediately-before-write re-negotiation (step 1). Re-queries 'Q', re-parses
   * STCP and requires every immutable capability field to be identical to the
   * negotiated set, plus enough inactive-slot capacity. Any difference aborts
   * before any write.
   */
  async revalidate(requiredSectors: number): Promise<CompatibilityVerdict> {
    const raw = await this.session.queryCapabilities();
    const fresh = raw ? parseCapabilities(raw) : null;
    if (!sameCapabilities(fresh, this.caps)) {
      throw new Error("the device's reported capabilities changed since connection — nothing was written");
    }
    // Capacity is NOT judged here: an insufficient inactive slot has its own
    // explicit refusal (step 6) so the message can never be mistaken for an
    // incompatible device.
    const verdict = evaluate(fresh);
    if (!verdict.writable) {
      throw new Error(`the device no longer satisfies the Stem Tape requirements — nothing was written`);
    }
    void requiredSectors;
    return verdict;
  }

  /**
   * Reconnection recovery (steps 19-21 after a lost connection). Reads BOTH
   * index slots, runs the shared selector and resolves the outcome. A completed
   * read never stays "unknown" just because the NEW record is invalid: if the
   * previous generation is valid, the replacement is simply not committed.
   */
  async resolveOutcome(expected: {
    frames: number;
    songChecksum: number;
    generation?: number;
  }): Promise<{ outcome: UploadOutcome; library: LibraryState | null; detail: string }> {
    let lib: LibraryState | null = null;
    try {
      lib = await this.readLibrary();
    } catch (e) {
      return {
        outcome: "unknown",
        library: null,
        detail: `Neither index could be read (${e instanceof Error ? e.message : String(e)}).`,
      };
    }
    if (!lib) return { outcome: "unknown", library: null, detail: "no capability data for this device" };
    if (lib.requiresInitialization) {
      return {
        outcome: lib.status === "blank" ? "corrupt" : "corrupt",
        library: lib,
        detail: lib.explanation,
      };
    }
    const a = lib.active!;
    const isNew =
      a.songPresent &&
      a.frames === expected.frames &&
      a.songChecksum === expected.songChecksum &&
      (expected.generation === undefined || a.generation === expected.generation);
    return {
      outcome: isNew ? "committed" : "failed",
      library: lib,
      detail: isNew
        ? `Generation ${a.generation} in index ${slotName(lib.activeIndexSlot!)} matches this song.`
        : `Generation ${a.generation} in index ${slotName(lib.activeIndexSlot!)} is the previous song; the replacement was not committed.`,
    };
  }

  /* ---------- safe replacement ---------- */

  /**
   * The exact 22-step safe replacement sequence.
   * `slot` is ignored: the destination is always the inactive A/B pair.
   */
  async uploadSong(args: {
    slot?: number;
    song: CanonicalSong;
    signal?: { aborted: boolean };
    onProgress?: (p: UploadProgress) => void;
  }): Promise<UploadResult> {
    if (!this.writable) throw new ReadOnlyDeviceError("upload");
    const { song } = args;
    const report = (
      stage: UploadStage,
      fraction: number,
      detail: string,
      extra?: Partial<UploadProgress>,
    ) => args.onProgress?.({ stage, fraction, detail, ...extra });
    const counter = { retries: 0 };
    const started = Date.now();
    this.magicAttempted = false;
    let songSha256 = "";
    let indexSha256 = "";
    let sectorCount = 0;
    let totalBlocks = 0;
    let written = 0;
    let verified = 0;
    let targetSongSlot: AbSlot | null = null;
    let targetIndexSlot: AbSlot | null = null;
    let generation = 0;
    let previousGeneration = 0;
    let failure: { operation: string; block: number } | undefined;
    const abort = () => {
      if (args.signal?.aborted) throw new Error("cancelled before commit — the previous song is still authoritative");
    };

    try {
      // 1. Query and validate capabilities immediately before anything else.
      report("preparing", 0.02, "re-checking device capabilities before writing");
      const sectors = encodeSong(song);
      const need = sectorsForFrames(song.frames);
      sectorCount = sectors.length;
      songSha256 = await sha256Hex(sectors);
      await this.revalidate(need);

      // 2-4. Read index A, read index B, select the current valid generation.
      report("preparing", 0.04, "reading index A and index B");
      const lib = await this.readLibrary();
      if (!lib || lib.requiresInitialization) {
        throw new Error(
          lib?.status === "corrupt"
            ? "both index slots are unreadable — this is corrupt storage and needs explicit initialization"
            : "this Stem Tape device has no valid index — initialize it explicitly first",
        );
      }
      previousGeneration = lib.generation;

      // 5. The other song slot and the other index slot are the destination.
      targetSongSlot = lib.inactiveSongSlot;
      targetIndexSlot = lib.inactiveIndexSlot;
      generation = lib.generation + 1;

      // 6. Verify the inactive song slot has sufficient capacity.
      const available = sectorsInRegion(this.caps!.song[targetSongSlot]);
      report(
        "capacity",
        0.06,
        `staging into song slot ${slotName(targetSongSlot)} / index slot ${slotName(targetIndexSlot)}`,
      );
      if (need > available) throw new InsufficientStagingCapacityError(need, available, targetSongSlot);

      // 7. The active song and index are never touched from here on.
      if (lib.activeSongSlot !== null && lib.activeSongSlot === targetSongSlot) {
        throw new Error("internal safety check failed: the destination equals the active song slot — nothing written");
      }
      if (lib.activeIndexSlot === targetIndexSlot) {
        throw new Error("internal safety check failed: the destination equals the active index slot — nothing written");
      }

      // 8. Write the new song to the inactive song slot.
      totalBlocks = sectors.length * BLOCKS_PER_SECTOR;
      const bulk = this.bulkSupported;
      const readBack: Uint8Array[] = [];

      if (bulk) {
        // 8/9/10 in one pass. Each 'U' round trip writes one whole sector AND
        // returns the CRC-32 the device computed from the bytes it read back
        // off its own storage, so there is no separate 512-byte read-back pass.
        const regionStart = this.caps!.song[targetSongSlot].start;
        for (let s = 0; s < sectors.length; s++) {
          abort();
          const payload = sectors[s]!;
          const expectCrc = crc32(payload);
          const dest = bulkDestBlock(regionStart, s);
          failure = { operation: "bulk-write", block: dest };
          const resp = await this.bulkWithRetry(s, dest, payload, expectCrc, counter, sectors.length);
          if (resp.seq !== s || resp.destBlock !== dest) {
            throw new Error(`the SP-1 answered for a different sector (sector ${resp.seq}, block ${resp.destBlock})`);
          }
          if (resp.verifiedCrc32 !== expectCrc) {
            throw new Error(`sector ${s + 1} did not read back correctly on the SP-1`);
          }
          written += BLOCKS_PER_SECTOR;
          verified += BLOCKS_PER_SECTOR;
          readBack.push(payload);
          report("writing", written / totalBlocks, `sector ${s + 1}/${sectors.length} written and verified`, {
            sectorsDone: s + 1,
            sectorsTotal: sectors.length,
            bytesDone: written * BLOCK_BYTES,
            bytesTotal: totalBlocks * BLOCK_BYTES,
            retries: counter.retries,
          });
        }
      } else {
        for (let s = 0; s < sectors.length; s++) {
          const blocks = sectorToBlocks(sectors[s]!);
          for (let k = 0; k < BLOCKS_PER_SECTOR; k++) {
            abort();
            const blk = this.songBlock(targetSongSlot, s) + k;
            failure = { operation: "write", block: blk };
            await this.writeWithRetry(blk, blocks[k]!, counter);
            written++;
            if (written % 16 === 0 || written === totalBlocks) {
              report("writing", written / totalBlocks, `sector ${s + 1}/${sectors.length} · block ${blk}`, {
                sectorsDone: s,
                sectorsTotal: sectors.length,
                bytesDone: written * BLOCK_BYTES,
                bytesTotal: totalBlocks * BLOCK_BYTES,
                retries: counter.retries,
              });
            }
          }
        }

        // 9/10. Read the entire new song back and verify every byte.
        for (let s = 0; s < sectors.length; s++) {
          const got: Uint8Array[] = [];
          for (let k = 0; k < BLOCKS_PER_SECTOR; k++) {
            abort();
            const blk = this.songBlock(targetSongSlot, s) + k;
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
      }

      // 10 (checksums). Recomputed from what the device holds.
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

      // 11/12. Next-generation record for the inactive index slot, magic absent.
      const draft: StemTapeIndexDraft = {
        slotIdentity: targetIndexSlot,
        songSlot: targetSongSlot,
        songPresent: true,
        generation,
        songStartBlock: this.caps!.song[targetSongSlot].start,
        songBlockCount: sectors.length * BLOCKS_PER_SECTOR,
        frames: song.frames,
        sectorCount: sectors.length,
        sampleRate: SAMPLE_RATE,
        channels: CHANNELS,
        bitDepth: PCM_BIT_DEPTH,
        bpm: song.metadata.bpm,
        downbeatFrame: Math.round(song.metadata.downbeatSeconds * SAMPLE_RATE),
        originalFrames: song.stems.map((s) => s.originalFrames),
        stemChecksums,
        songChecksum,
        title: song.metadata.title,
        artist: song.metadata.artist,
      };
      const uncommitted = indexRecordBlock(draft, false);
      const committed = indexRecordBlock(draft, true);
      const indexBlk = this.indexBlock(targetIndexSlot);

      await this.session.lock.run(async () => {
        // 13. Write the complete uncommitted index.
        report("metadata", 0.93, `writing the uncommitted index into slot ${slotName(targetIndexSlot!)}`);
        failure = { operation: "index-write", block: indexBlk };
        await this.session.writeBlock(indexBlk, uncommitted);
        // 14. Flush.
        failure = { operation: "flush", block: indexBlk };
        await this.session.flush();
        // 15/16. Read the uncommitted index back and verify every byte except
        // the intentionally absent magic.
        report("metadata", 0.94, "verifying the uncommitted index");
        failure = { operation: "index-read-back", block: indexBlk };
        const back = await this.session.readBlock(indexBlk);
        const cmp = recordsEqualIgnoringMagic(back, uncommitted);
        if (!cmp.equal) throw new Error(`uncommitted index read-back mismatch at byte ${cmp.byte}`);
        for (let i = 256; i < BLOCK_BYTES; i++) {
          if (back[i] !== 0) throw new Error(`uncommitted index padding is not zero at byte ${i}`);
        }
        // 17. Validity magic last.
        report("committing", 0.96, "writing the validity magic");
        this.magicAttempted = true;
        failure = { operation: "magic", block: indexBlk };
        await this.session.writeBlock(indexBlk, committed);
        // 18. Flush.
        failure = { operation: "final-flush", block: indexBlk };
        await this.session.flush();
      });
      indexSha256 = await sha256Hex([buildIndexRecord(draft, true)]);

      // 19-21. Re-read BOTH index slots, run the shared selector, confirm the
      // new generation is the one selected.
      report("confirming", 0.98, "re-reading index A and index B");
      failure = { operation: "confirm", block: indexBlk };
      const after = await this.readLibrary();
      if (!after || after.requiresInitialization) throw new Error("neither index slot was valid after the commit");
      if (after.activeIndexSlot !== targetIndexSlot || after.generation !== generation) {
        throw new Error(
          `the new generation was not selected after the commit (active: index ${after.activeIndexSlot === null ? "none" : slotName(after.activeIndexSlot)} generation ${after.generation})`,
        );
      }
      const a = after.active!;
      const matches =
        a.songPresent &&
        a.songSlot === targetSongSlot &&
        a.frames === song.frames &&
        a.songChecksum === songChecksum &&
        a.stemChecksums.every((v, i) => v === stemChecksums[i]) &&
        a.title === song.metadata.title &&
        a.artist === song.metadata.artist &&
        Math.abs(a.bpm - song.metadata.bpm) < 1 / 256;
      if (!matches) throw new Error("the committed index did not match after re-read");
      failure = undefined;

      // 22. Report committed only now. The previous record is left intact as
      // the rollback copy and becomes the destination of the next replacement.
      const physical = this.mode.kind === "physical";
      report(
        "complete",
        1,
        physical
          ? `Generation ${generation} selected from index ${slotName(targetIndexSlot)} on the SP-1.`
          : "Simulated device: A/B commit sequence passed. No physical SP-1 was written.",
      );
      return {
        ok: true,
        outcome: "committed",
        detail: physical
          ? `Upload committed as generation ${generation} in index slot ${slotName(targetIndexSlot)} and independently re-read on the SP-1. Generation ${previousGeneration} remains valid as the rollback copy.`
          : `Simulated A/B commit passed · generation ${generation} · no physical SP-1 involved`,
        writtenBlocks: written,
        verifiedBlocks: verified,
        totalBlocks,
        bytesWritten: written * BLOCK_BYTES,
        sectorCount,
        retries: counter.retries,
        elapsedMs: Date.now() - started,
        songSha256,
        indexSha256,
        stemChecksums,
        songChecksum,
        targetSongSlot,
        targetIndexSlot,
        generation,
        previousGeneration,
        verification: {
          simulatedVerification: !physical,
          deviceReadbackVerification: physical,
          physicalPlaybackVerification: false,
        },
        failure: undefined,
      };
    } catch (err) {
      const detail = err instanceof Error ? err.message : String(err);
      // Only a failure that happened after the magic may have reached the
      // device is unknown. Everything earlier leaves the previous generation
      // authoritative — and even the unknown case cannot destroy it.
      const outcome: UploadOutcome = this.magicAttempted ? "unknown" : "failed";
      return {
        ok: false,
        outcome,
        detail:
          outcome === "unknown"
            ? `${detail} — the validity magic may already have been written to index slot ${targetIndexSlot === null ? "?" : slotName(targetIndexSlot)}. Outcome unknown: reconnect to check which verified song is active. Generation ${previousGeneration} is still intact either way.`
            : `${detail} — no validity magic was sent, so generation ${previousGeneration} remains the active song.`,
        writtenBlocks: written,
        verifiedBlocks: verified,
        totalBlocks,
        bytesWritten: written * BLOCK_BYTES,
        sectorCount,
        retries: counter.retries,
        elapsedMs: Date.now() - started,
        songSha256,
        indexSha256,
        stemChecksums: [],
        songChecksum: 0,
        targetSongSlot,
        targetIndexSlot,
        generation,
        previousGeneration,
        verification: {
          simulatedVerification: false,
          deviceReadbackVerification: false,
          physicalPlaybackVerification: false,
        },
        failure,
      };
    }
  }

  /**
   * Clear the library by committing a next-generation, song-free record into
   * the inactive index slot. The song data itself is left in place; only the
   * index stops referencing it, and the previous generation remains valid until
   * the following replacement overwrites it.
   */
  async deleteSong(_slot?: number): Promise<LibraryState> {
    if (!this.writable) throw new ReadOnlyDeviceError("delete");
    const lib = this.library ?? (await this.readLibrary());
    if (!lib || lib.requiresInitialization) throw new Error("no valid index");
    const target = lib.inactiveIndexSlot;
    const draft = blankIndexDraft(target, otherSlot(lib.activeSongSlot ?? SLOT_B), lib.generation + 1);
    const blk = this.indexBlock(target);
    await this.session.lock.run(async () => {
      await this.session.writeBlock(blk, indexRecordBlock(draft, false));
      await this.session.flush();
      await this.session.writeBlock(blk, indexRecordBlock(draft, true));
      await this.session.flush();
    });
    return (await this.readLibrary())!;
  }

  /** 'X' clean exit, then every stream/port lock released. */
  async disconnect(): Promise<void> {
    this.session.stopKeepalive();
    await this.session.exit();
    await this.session.io.close();
  }
}
