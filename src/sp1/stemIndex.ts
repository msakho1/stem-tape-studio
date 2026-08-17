/**
 * STIX v2 index record — build, parse and independent validation.
 *
 * One record fully describes ONE song and the song-data region that holds it,
 * so a record can be validated on its own without consulting the other slot.
 * The record is 256 bytes and lives at offset 0 of block 0 of its index region,
 * which makes the validity-magic write exactly one 512-byte block write.
 *
 * Every numeric offset comes from ./stemTapeFormat — nowhere else.
 */

import { crc32 } from "./crc32";
import {
  CRC_RANGE,
  CRC_ZEROED,
  FORMAT_MAJOR,
  FORMAT_MINOR,
  INDEX_MAGIC,
  INDEX_RECORD_BYTES,
  IX_FLAG,
  IX_OFF,
  PHYSICAL_BLOCK_BYTES,
  SAMPLE_RATE,
  STIX_VERSION,
  TEXT_BYTES,
  type AbSlot,
  type BlockRegion,
  CHANNELS,
  PCM_BIT_DEPTH,
  regionEnd,
  sectorsForFrames,
  BLOCKS_PER_SECTOR,
} from "./stemTapeFormat";

/* ---------- little-endian primitives ---------- */

function put32(a: Uint8Array, o: number, v: number) {
  a[o] = v & 255;
  a[o + 1] = (v >>> 8) & 255;
  a[o + 2] = (v >>> 16) & 255;
  a[o + 3] = (v >>> 24) & 255;
}
function get32(a: Uint8Array, o: number): number {
  return ((a[o]! | (a[o + 1]! << 8) | (a[o + 2]! << 16)) >>> 0) + a[o + 3]! * 0x1000000;
}
function put16(a: Uint8Array, o: number, v: number) {
  a[o] = v & 255;
  a[o + 1] = (v >>> 8) & 255;
}
function get16(a: Uint8Array, o: number): number {
  return a[o]! | (a[o + 1]! << 8);
}
function putText(a: Uint8Array, o: number, s: string) {
  const bytes = new TextEncoder().encode(s).slice(0, TEXT_BYTES - 1);
  a.fill(0, o, o + TEXT_BYTES);
  a.set(bytes, o);
}
function getText(a: Uint8Array, o: number): string {
  const slice = a.subarray(o, o + TEXT_BYTES);
  const end = slice.indexOf(0);
  return new TextDecoder().decode(end >= 0 ? slice.subarray(0, end) : slice);
}

/** Generation is u64: lo + hi * 2^32, compared numerically. */
export function splitGeneration(g: number): { lo: number; hi: number } {
  return { lo: g >>> 0, hi: Math.floor(g / 0x100000000) >>> 0 };
}
export function joinGeneration(lo: number, hi: number): number {
  return hi * 0x100000000 + (lo >>> 0);
}

/* ---------- record ---------- */

export interface StemTapeIndexRecord {
  /** True when the validity magic is present in the stored bytes. */
  committed: boolean;
  indexVersion: number;
  formatMajor: number;
  formatMinor: number;
  slotIdentity: AbSlot;
  songSlot: AbSlot;
  songPresent: boolean;
  generation: number;
  songStartBlock: number;
  songBlockCount: number;
  frames: number;
  sectorCount: number;
  sampleRate: number;
  channels: number;
  bitDepth: number;
  bpm: number;
  downbeatFrame: number;
  originalFrames: number[];
  stemChecksums: number[];
  songChecksum: number;
  songSha256?: string;
  title: string;
  artist: string;
  /** CRC stored in the record. */
  crc: number;
  /** CRC recomputed over the stored bytes. */
  crcComputed: number;
}

export interface StemTapeIndexDraft {
  slotIdentity: AbSlot;
  songSlot: AbSlot;
  songPresent: boolean;
  generation: number;
  songStartBlock: number;
  songBlockCount: number;
  frames: number;
  sectorCount: number;
  sampleRate?: number;
  channels?: number;
  bitDepth?: number;
  bpm: number;
  downbeatFrame: number;
  originalFrames: number[];
  stemChecksums: number[];
  songChecksum: number;
  title: string;
  artist: string;
  formatMajor?: number;
  formatMinor?: number;
}

/** An empty (song-free) but structurally valid index for a fresh library. */
export function blankIndexDraft(slotIdentity: AbSlot, songSlot: AbSlot, generation = 1): StemTapeIndexDraft {
  return {
    slotIdentity,
    songSlot,
    songPresent: false,
    generation,
    songStartBlock: 0,
    songBlockCount: 0,
    frames: 0,
    sectorCount: 0,
    sampleRate: 0,
    channels: 0,
    bitDepth: 0,
    bpm: 0,
    downbeatFrame: 0,
    originalFrames: [0, 0, 0, 0],
    stemChecksums: [0, 0, 0, 0],
    songChecksum: 0,
    title: "",
    artist: "",
  };
}

/**
 * Serialize one STIX v2 record. `withMagic=false` leaves the validity magic at
 * zero so an uncommitted record can be written, flushed and verified first; the
 * CRC is byte-identical in both images because the magic is normalized out of
 * CRC coverage.
 */
export function buildIndexRecord(d: StemTapeIndexDraft, withMagic: boolean): Uint8Array {
  const b = new Uint8Array(INDEX_RECORD_BYTES);
  put32(b, IX_OFF.magic, withMagic ? INDEX_MAGIC : 0);
  put16(b, IX_OFF.indexVersion, STIX_VERSION);
  put16(b, IX_OFF.formatMajor, d.formatMajor ?? FORMAT_MAJOR);
  put16(b, IX_OFF.formatMinor, d.formatMinor ?? FORMAT_MINOR);
  b[IX_OFF.slotIdentity] = d.slotIdentity;
  b[IX_OFF.songSlot] = d.songSlot;
  put16(b, IX_OFF.flags, d.songPresent ? IX_FLAG.SONG_PRESENT : 0);
  const g = splitGeneration(d.generation);
  put32(b, IX_OFF.generationLo, g.lo);
  put32(b, IX_OFF.generationHi, g.hi);
  put32(b, IX_OFF.songStartBlock, d.songStartBlock);
  put32(b, IX_OFF.songBlockCount, d.songBlockCount);
  put32(b, IX_OFF.frames, d.frames);
  put32(b, IX_OFF.sectorCount, d.sectorCount);
  put32(b, IX_OFF.sampleRate, d.sampleRate ?? 0);
  put16(b, IX_OFF.channels, d.channels ?? 0);
  put16(b, IX_OFF.bitDepth, d.bitDepth ?? 0);
  put32(b, IX_OFF.bpmQ8, Math.round(d.bpm * 256));
  put32(b, IX_OFF.downbeatFrame, d.downbeatFrame);
  for (let t = 0; t < 4; t++) put32(b, IX_OFF.originalFrames + t * 4, d.originalFrames[t] ?? 0);
  for (let t = 0; t < 4; t++) put32(b, IX_OFF.stemChecksums + t * 4, d.stemChecksums[t] ?? 0);
  put32(b, IX_OFF.songChecksum, d.songChecksum);
  putText(b, IX_OFF.title, d.title);
  putText(b, IX_OFF.artist, d.artist);
  put32(b, IX_OFF.crc32, crc32(b, CRC_RANGE.from, CRC_RANGE.to, CRC_ZEROED));
  return b;
}

/** The full 512-byte block that carries a record (record + zero padding). */
export function indexRecordBlock(d: StemTapeIndexDraft, withMagic: boolean): Uint8Array {
  const blk = new Uint8Array(PHYSICAL_BLOCK_BYTES);
  blk.set(buildIndexRecord(d, withMagic));
  return blk;
}

export function parseIndexRecord(bytes: Uint8Array): StemTapeIndexRecord {
  const b = bytes.subarray(0, INDEX_RECORD_BYTES);
  return {
    committed: get32(b, IX_OFF.magic) === INDEX_MAGIC,
    indexVersion: get16(b, IX_OFF.indexVersion),
    formatMajor: get16(b, IX_OFF.formatMajor),
    formatMinor: get16(b, IX_OFF.formatMinor),
    slotIdentity: (b[IX_OFF.slotIdentity]! & 1) as AbSlot,
    songSlot: (b[IX_OFF.songSlot]! & 1) as AbSlot,
    songPresent: (get16(b, IX_OFF.flags) & IX_FLAG.SONG_PRESENT) !== 0,
    generation: joinGeneration(get32(b, IX_OFF.generationLo), get32(b, IX_OFF.generationHi)),
    songStartBlock: get32(b, IX_OFF.songStartBlock),
    songBlockCount: get32(b, IX_OFF.songBlockCount),
    frames: get32(b, IX_OFF.frames),
    sectorCount: get32(b, IX_OFF.sectorCount),
    sampleRate: get32(b, IX_OFF.sampleRate),
    channels: get16(b, IX_OFF.channels),
    bitDepth: get16(b, IX_OFF.bitDepth),
    bpm: get32(b, IX_OFF.bpmQ8) / 256,
    downbeatFrame: get32(b, IX_OFF.downbeatFrame),
    originalFrames: [0, 1, 2, 3].map((t) => get32(b, IX_OFF.originalFrames + t * 4)),
    stemChecksums: [0, 1, 2, 3].map((t) => get32(b, IX_OFF.stemChecksums + t * 4)),
    songChecksum: get32(b, IX_OFF.songChecksum),
    title: getText(b, IX_OFF.title),
    artist: getText(b, IX_OFF.artist),
    crc: get32(b, IX_OFF.crc32),
    crcComputed: crc32(b, CRC_RANGE.from, CRC_RANGE.to, CRC_ZEROED),
  };
}

/** The raw byte-level check used before the magic is written. */
export function recordsEqualIgnoringMagic(a: Uint8Array, b: Uint8Array): { equal: boolean; byte?: number } {
  for (let i = 0; i < INDEX_RECORD_BYTES; i++) {
    if (i >= CRC_ZEROED.from && i < CRC_ZEROED.to) continue;
    if (a[i] !== b[i]) return { equal: false, byte: i };
  }
  return { equal: true };
}

export interface IndexValidation {
  valid: boolean;
  reason: string;
}

export interface RegionContext {
  song: [BlockRegion, BlockRegion];
  index: [BlockRegion, BlockRegion];
}

/**
 * Independent validation of one parsed record read from index slot `slot`.
 * Rejects invalid magic, CRC, version, slot identity, bounds and song metadata.
 */
export function validateIndexRecord(
  rec: StemTapeIndexRecord,
  slot: AbSlot,
  regions: RegionContext,
): IndexValidation {
  const bad = (reason: string): IndexValidation => ({ valid: false, reason });
  if (!rec.committed) return bad("validity magic absent (uncommitted record)");
  if (rec.crc !== rec.crcComputed) {
    return bad(`CRC mismatch (stored 0x${rec.crc.toString(16)}, computed 0x${rec.crcComputed.toString(16)})`);
  }
  if (rec.indexVersion !== STIX_VERSION) return bad(`unsupported STIX index version ${rec.indexVersion}`);
  if (rec.formatMajor !== FORMAT_MAJOR) return bad(`unsupported format major ${rec.formatMajor}`);
  if (rec.formatMinor !== FORMAT_MINOR) return bad(`unsupported format minor ${rec.formatMinor}`);
  if (rec.slotIdentity !== slot) return bad(`slot identity ${rec.slotIdentity} does not match index slot ${slot}`);
  if (rec.generation < 1) return bad("generation must be >= 1");
  if (!Number.isSafeInteger(rec.generation)) return bad("generation is not a safe integer");

  if (!rec.songPresent) {
    const clean =
      rec.songStartBlock === 0 &&
      rec.songBlockCount === 0 &&
      rec.frames === 0 &&
      rec.sectorCount === 0 &&
      rec.songChecksum === 0;
    return clean ? { valid: true, reason: "valid, no song" } : bad("song-free record carries song metadata");
  }

  const region = regions.song[rec.songSlot];
  if (!region) return bad(`song slot ${rec.songSlot} is not reported by the device`);
  if (rec.frames < 1) return bad("song present but frame count is zero");
  if (rec.sectorCount !== sectorsForFrames(rec.frames)) {
    return bad(`sector count ${rec.sectorCount} does not match ${rec.frames} frames`);
  }
  if (rec.songBlockCount !== rec.sectorCount * BLOCKS_PER_SECTOR) {
    return bad(`song block count ${rec.songBlockCount} does not match ${rec.sectorCount} logical sectors`);
  }
  if (rec.songStartBlock !== region.start) {
    return bad(`song start block ${rec.songStartBlock} is not the start of song slot ${rec.songSlot}`);
  }
  if (rec.songStartBlock + rec.songBlockCount > regionEnd(region)) {
    return bad(`song data extends past song slot ${rec.songSlot}`);
  }
  if (rec.sampleRate !== SAMPLE_RATE) return bad(`sample rate ${rec.sampleRate} is not 48000`);
  if (rec.channels !== CHANNELS) return bad(`channel count ${rec.channels} is not 2`);
  if (rec.bitDepth !== PCM_BIT_DEPTH) return bad(`bit depth ${rec.bitDepth} is not 24`);
  if (rec.bpm <= 0) return bad("bpm metadata missing");
  if (rec.downbeatFrame > rec.frames) return bad("downbeat lies past the end of the song");
  return { valid: true, reason: "valid" };
}
