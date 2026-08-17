/**
 * Versioned Stem Tape index extension.
 *
 * Reuses Tape Looper's index discipline verbatim in shape (firmware/web/index.html
 * parseMeta/buildMeta): read the device's own block(s), keep the raw bytes so
 * fields this companion does not own survive untouched, rebuild in place, and
 * write the block carrying the authoritative magic LAST.
 *
 * All numeric offsets come from ./stemTapeFormat — nowhere else.
 */

import {
  ENTRY_OFF,
  INDEX_ENTRY_BYTES,
  INDEX_HEADER_BYTES,
  INDEX_MAGIC,
  INDEX_OFF,
  PHYSICAL_BLOCK_BYTES,
  TEXT_BYTES,
  indexBlockCount,
  indexByteLength,
} from "./stemTapeFormat";

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

export interface StemTapeSongEntry {
  committed: boolean;
  startSector: number;
  sectorCount: number;
  sampleRate: number;
  channels: number;
  bitDepth: number;
  frames: number;
  originalFrames: number[];
  stemChecksums: number[];
  songChecksum: number;
  bpm: number;
  downbeatFrame: number;
  title: string;
  artist: string;
}

export interface StemTapeIndex {
  magic: number;
  formatMajor: number;
  formatMinor: number;
  indexBytes: number;
  generation: number;
  slotCount: number;
  sectorsPerSong: number;
  currentSong: number;
  songs: StemTapeSongEntry[];
  /** The device's own bytes, so unmanaged fields survive a rebuild. */
  raw: Uint8Array;
}

export const EMPTY_ENTRY: StemTapeSongEntry = {
  committed: false,
  startSector: 0,
  sectorCount: 0,
  sampleRate: 0,
  channels: 0,
  bitDepth: 0,
  frames: 0,
  originalFrames: [0, 0, 0, 0],
  stemChecksums: [0, 0, 0, 0],
  songChecksum: 0,
  bpm: 0,
  downbeatFrame: 0,
  title: "",
  artist: "",
};

export function parseStemIndex(raw: Uint8Array, slotCount: number): StemTapeIndex {
  const songs: StemTapeSongEntry[] = [];
  for (let s = 0; s < slotCount; s++) {
    const o = INDEX_HEADER_BYTES + s * INDEX_ENTRY_BYTES;
    songs.push({
      committed: get32(raw, o + ENTRY_OFF.committed) === 1,
      startSector: get32(raw, o + ENTRY_OFF.startSector),
      sectorCount: get32(raw, o + ENTRY_OFF.sectorCount),
      sampleRate: get32(raw, o + ENTRY_OFF.sampleRate),
      channels: get16(raw, o + ENTRY_OFF.channels),
      bitDepth: get16(raw, o + ENTRY_OFF.bitDepth),
      frames: get32(raw, o + ENTRY_OFF.frames),
      originalFrames: [0, 1, 2, 3].map((t) => get32(raw, o + ENTRY_OFF.originalFrames + t * 4)),
      stemChecksums: [0, 1, 2, 3].map((t) => get32(raw, o + ENTRY_OFF.stemChecksums + t * 4)),
      songChecksum: get32(raw, o + ENTRY_OFF.songChecksum),
      bpm: get32(raw, o + ENTRY_OFF.bpmQ8) / 256,
      downbeatFrame: get32(raw, o + ENTRY_OFF.downbeatFrame),
      title: getText(raw, o + ENTRY_OFF.title),
      artist: getText(raw, o + ENTRY_OFF.artist),
    });
  }
  return {
    magic: get32(raw, INDEX_OFF.magic),
    formatMajor: get16(raw, INDEX_OFF.formatMajor),
    formatMinor: get16(raw, INDEX_OFF.formatMinor),
    indexBytes: get32(raw, INDEX_OFF.indexBytes),
    generation: get32(raw, INDEX_OFF.generation),
    slotCount,
    sectorsPerSong: get32(raw, INDEX_OFF.sectorsPerSong),
    currentSong: get32(raw, INDEX_OFF.currentSong),
    songs,
    raw: raw.slice(0),
  };
}

export function emptyStemIndex(
  slotCount: number,
  sectorsPerSong: number,
  formatMajor: number,
  formatMinor: number,
): StemTapeIndex {
  const raw = new Uint8Array(indexBlockCount(slotCount) * PHYSICAL_BLOCK_BYTES);
  return {
    magic: INDEX_MAGIC,
    formatMajor,
    formatMinor,
    indexBytes: indexByteLength(slotCount),
    generation: 1,
    slotCount,
    sectorsPerSong,
    currentSong: 0,
    songs: Array.from({ length: slotCount }, () => ({ ...EMPTY_ENTRY, originalFrames: [0, 0, 0, 0], stemChecksums: [0, 0, 0, 0] })),
    raw,
  };
}

/**
 * Serialize the index over the device's own bytes.
 * `withMagic=false` zeroes the authoritative magic so continuation blocks can be
 * written first without ever making a half-written index look valid.
 */
export function buildStemIndex(index: StemTapeIndex, withMagic = true): Uint8Array {
  const size = indexBlockCount(index.slotCount) * PHYSICAL_BLOCK_BYTES;
  const b = index.raw.length >= size ? Uint8Array.from(index.raw) : new Uint8Array(size);
  put32(b, INDEX_OFF.magic, withMagic ? INDEX_MAGIC : 0);
  put16(b, INDEX_OFF.formatMajor, index.formatMajor);
  put16(b, INDEX_OFF.formatMinor, index.formatMinor);
  put32(b, INDEX_OFF.indexBytes, indexByteLength(index.slotCount));
  put32(b, INDEX_OFF.generation, index.generation);
  put32(b, INDEX_OFF.slotCount, index.slotCount);
  put32(b, INDEX_OFF.sectorsPerSong, index.sectorsPerSong);
  put32(b, INDEX_OFF.currentSong, index.currentSong);
  for (let s = 0; s < index.slotCount; s++) {
    const o = INDEX_HEADER_BYTES + s * INDEX_ENTRY_BYTES;
    const e = index.songs[s]!;
    put32(b, o + ENTRY_OFF.committed, e.committed ? 1 : 0);
    put32(b, o + ENTRY_OFF.startSector, e.startSector);
    put32(b, o + ENTRY_OFF.sectorCount, e.sectorCount);
    put32(b, o + ENTRY_OFF.sampleRate, e.sampleRate);
    put16(b, o + ENTRY_OFF.channels, e.channels);
    put16(b, o + ENTRY_OFF.bitDepth, e.bitDepth);
    put32(b, o + ENTRY_OFF.frames, e.frames);
    for (let t = 0; t < 4; t++) put32(b, o + ENTRY_OFF.originalFrames + t * 4, e.originalFrames[t] ?? 0);
    for (let t = 0; t < 4; t++) put32(b, o + ENTRY_OFF.stemChecksums + t * 4, e.stemChecksums[t] ?? 0);
    put32(b, o + ENTRY_OFF.songChecksum, e.songChecksum);
    put32(b, o + ENTRY_OFF.bpmQ8, Math.round(e.bpm * 256));
    put32(b, o + ENTRY_OFF.downbeatFrame, e.downbeatFrame);
    putText(b, o + ENTRY_OFF.title, e.title);
    putText(b, o + ENTRY_OFF.artist, e.artist);
  }
  return b;
}

export function indexIsValid(index: StemTapeIndex, formatMajor: number): boolean {
  return index.magic === INDEX_MAGIC && index.formatMajor === formatMajor;
}

export function splitIndexBlocks(bytes: Uint8Array): Uint8Array[] {
  const out: Uint8Array[] = [];
  for (let o = 0; o < bytes.length; o += PHYSICAL_BLOCK_BYTES) {
    out.push(bytes.subarray(o, o + PHYSICAL_BLOCK_BYTES));
  }
  return out;
}
