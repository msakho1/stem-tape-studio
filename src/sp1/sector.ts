/**
 * Stem Tape logical song sector encode / decode.
 *
 * One logical sector = 8,192 bytes = sixteen consecutive 512-byte Tape Looper
 * block writes. Physical block order inside a sector is strictly ascending:
 * block k of the sector carries bytes [k*512, (k+1)*512).
 *
 * Frame order inside the payload is stem-major per frame:
 *   frame f -> vocal L, vocal R, drums L, drums R, bass L, bass R, inst L, inst R
 * each sample signed 24-bit little-endian. The final partial sector is padded
 * with digital silence.
 */

import {
  BLOCKS_PER_SECTOR,
  BYTES_PER_FRAME,
  BYTES_PER_SAMPLE,
  CHANNELS,
  FRAMES_PER_SECTOR,
  PHYSICAL_BLOCK_BYTES,
  SECTOR_BYTES,
  SECTOR_HEADER_BYTES,
  SECTOR_MAGIC,
  SECTOR_OFF,
  STEM_COUNT,
  sectorsForFrames,
} from "./stemTapeFormat";
import { readInt24LE, type CanonicalSong } from "./song";

function put32(a: Uint8Array, off: number, v: number) {
  a[off] = v & 255;
  a[off + 1] = (v >>> 8) & 255;
  a[off + 2] = (v >>> 16) & 255;
  a[off + 3] = (v >>> 24) & 255;
}
function get32(a: Uint8Array, off: number): number {
  return ((a[off]! | (a[off + 1]! << 8) | (a[off + 2]! << 16)) >>> 0) + a[off + 3]! * 0x1000000;
}

export interface SectorHeader {
  sectorIndex: number;
  firstFrame: number;
  frameCount: number;
  bpmQ8: number;
  downbeatFrame: number;
}

/** Encode one 8,192-byte logical sector from the canonical song. */
export function encodeSector(song: CanonicalSong, sectorIndex: number): Uint8Array {
  const out = new Uint8Array(SECTOR_BYTES);
  const firstFrame = sectorIndex * FRAMES_PER_SECTOR;
  const frameCount = Math.max(0, Math.min(FRAMES_PER_SECTOR, song.frames - firstFrame));
  put32(out, SECTOR_OFF.magic, SECTOR_MAGIC);
  put32(out, SECTOR_OFF.sectorIndex, sectorIndex);
  put32(out, SECTOR_OFF.firstFrame, firstFrame);
  put32(out, SECTOR_OFF.frameCount, frameCount);
  put32(out, SECTOR_OFF.bpmQ8, Math.round(song.metadata.bpm * 256));
  put32(out, SECTOR_OFF.downbeatFrame, Math.round(song.metadata.downbeatSeconds * song.sampleRate));
  // ledReserved + reserved stay zero: firmware-owned bytes, never invented here.

  for (let f = 0; f < frameCount; f++) {
    const dst = SECTOR_HEADER_BYTES + f * BYTES_PER_FRAME;
    for (let s = 0; s < STEM_COUNT; s++) {
      const pcm = song.stems[s]!.pcm24;
      const src = (firstFrame + f) * CHANNELS * BYTES_PER_SAMPLE;
      for (let b = 0; b < CHANNELS * BYTES_PER_SAMPLE; b++) {
        out[dst + s * CHANNELS * BYTES_PER_SAMPLE + b] = pcm[src + b]!;
      }
    }
  }
  return out;
}

export function encodeSong(song: CanonicalSong): Uint8Array[] {
  const n = sectorsForFrames(song.frames);
  const sectors: Uint8Array[] = [];
  for (let i = 0; i < n; i++) sectors.push(encodeSector(song, i));
  return sectors;
}

export function readSectorHeader(sector: Uint8Array): SectorHeader & { magic: number } {
  return {
    magic: get32(sector, SECTOR_OFF.magic),
    sectorIndex: get32(sector, SECTOR_OFF.sectorIndex),
    firstFrame: get32(sector, SECTOR_OFF.firstFrame),
    frameCount: get32(sector, SECTOR_OFF.frameCount),
    bpmQ8: get32(sector, SECTOR_OFF.bpmQ8),
    downbeatFrame: get32(sector, SECTOR_OFF.downbeatFrame),
  };
}

export interface DecodedSong {
  frames: number;
  /** Per stem: interleaved stereo signed 24-bit LE, frames*2*3 bytes. */
  stems: Uint8Array[];
  bpm: number;
  downbeatFrame: number;
}

/** Reverse of encodeSong: reproduces every sample and stem/channel position. */
export function decodeSectors(sectors: Uint8Array[], frames: number): DecodedSong {
  const stems: Uint8Array[] = [];
  for (let s = 0; s < STEM_COUNT; s++) stems.push(new Uint8Array(frames * CHANNELS * BYTES_PER_SAMPLE));
  let bpmQ8 = 0;
  let downbeatFrame = 0;
  for (const sector of sectors) {
    if (sector.length !== SECTOR_BYTES) throw new Error(`sector is ${sector.length} bytes, expected ${SECTOR_BYTES}`);
    const h = readSectorHeader(sector);
    if (h.magic !== SECTOR_MAGIC) throw new Error(`sector ${h.sectorIndex} magic mismatch`);
    bpmQ8 = h.bpmQ8;
    downbeatFrame = h.downbeatFrame;
    for (let f = 0; f < h.frameCount; f++) {
      const src = SECTOR_HEADER_BYTES + f * BYTES_PER_FRAME;
      const dst = (h.firstFrame + f) * CHANNELS * BYTES_PER_SAMPLE;
      for (let s = 0; s < STEM_COUNT; s++) {
        for (let b = 0; b < CHANNELS * BYTES_PER_SAMPLE; b++) {
          stems[s]![dst + b] = sector[src + s * CHANNELS * BYTES_PER_SAMPLE + b]!;
        }
      }
    }
  }
  return { frames, stems, bpm: bpmQ8 / 256, downbeatFrame };
}

/** Split one logical sector into its sixteen consecutive 512-byte block writes. */
export function sectorToBlocks(sector: Uint8Array): Uint8Array[] {
  if (sector.length !== SECTOR_BYTES) throw new Error(`sector is ${sector.length} bytes, expected ${SECTOR_BYTES}`);
  const out: Uint8Array[] = [];
  for (let k = 0; k < BLOCKS_PER_SECTOR; k++) {
    out.push(sector.subarray(k * PHYSICAL_BLOCK_BYTES, (k + 1) * PHYSICAL_BLOCK_BYTES));
  }
  return out;
}

export function blocksToSector(blocks: Uint8Array[]): Uint8Array {
  if (blocks.length !== BLOCKS_PER_SECTOR) throw new Error(`expected ${BLOCKS_PER_SECTOR} blocks`);
  const out = new Uint8Array(SECTOR_BYTES);
  blocks.forEach((b, k) => out.set(b, k * PHYSICAL_BLOCK_BYTES));
  return out;
}

/** Convenience for tests: one sample from a decoded stem. */
export function sampleAt(stem: Uint8Array, frame: number, channel: number): number {
  return readInt24LE(stem, frame * CHANNELS * BYTES_PER_SAMPLE + channel * BYTES_PER_SAMPLE);
}
