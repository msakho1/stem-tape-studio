/**
 * Stem Tape v1.3 PLANAR song encode / decode.
 *
 * The transport unit is unchanged: one logical sector = 8,192 bytes = sixteen
 * consecutive 512-byte block writes, and block k of a sector carries bytes
 * [k*512, (k+1)*512).
 *
 * What changed in v1.3 is what those bytes contain. v1.1 interleaved all four
 * stems into every 24-byte frame, so fetching one stem at a divergent position
 * cost a whole 8,192-byte sector to obtain 2,048 useful bytes. v1.3 stores each
 * stem's entire timeline contiguously in its own quarter of the song region:
 *
 *   | stem 0 timeline | stem 1 timeline | stem 2 timeline | stem 3 timeline |
 *
 * The unit is a 2,048-byte GROUP (four blocks) holding 510 frames of ONE stem,
 * with an 8-byte self-validating header ('P','L', stemIndex, flags, groupIndex).
 * A sector is simply four consecutive groups of that stem-major stream, so
 *
 *   blockOf(stem, groupIndex) = songStartBlock + (stem * groups + groupIndex) * 4
 *
 * holds exactly, and a song occupies the same groups*16 blocks it did in v1.1.
 * Every STIX geometry field (frames, sectorCount, songBlockCount,
 * songStartBlock) is therefore unchanged, and so are all five checksums, which
 * are computed over each stem's own contiguous PCM, never over sector bytes.
 *
 * Stem order is unchanged: 0 = vocal, 1 = drums, 2 = bass, 3 = instrument.
 * A partial final group is zero-padded — silence, never stale bytes.
 */

import {
  BLOCKS_PER_SECTOR,
  BYTES_PER_FRAME_V11,
  BYTES_PER_SAMPLE_V11,
  BYTES_PER_STEM_FRAME,
  BYTES_PER_STEM_FRAME_V12,
  GROUP_FLAGS_V13,
  CHANNELS,
  FRAMES_PER_GROUP,
  GROUPS_PER_SECTOR,
  GROUP_BYTES,
  GROUP_HEADER_BYTES,
  GROUP_MAGIC_0,
  GROUP_MAGIC_1,
  GROUP_OFF,
  PHYSICAL_BLOCK_BYTES,
  SECTOR_BYTES,
  SECTOR_HEADER_BYTES,
  SECTOR_MAGIC,
  SECTOR_OFF,
  STEM_COUNT,
  groupsForFrames,
  sectorsForFrames,
} from "./stemTapeFormat";
import { type CanonicalSong } from "./song";
import { readInt16LE, stemPcm16 } from "./pcm16";

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

export interface GroupHeader {
  stemIndex: number;
  flags: number;
  groupIndex: number;
}

/**
 * Encode one 2,048-byte planar group: 510 frames of a single stem, starting at
 * frame groupIndex * 340. A partial final group is zero-padded with silence.
 */
export function encodeGroup(song: CanonicalSong, stemIndex: number, groupIndex: number): Uint8Array {
  const out = new Uint8Array(GROUP_BYTES);
  out[GROUP_OFF.magic0] = GROUP_MAGIC_0;
  out[GROUP_OFF.magic1] = GROUP_MAGIC_1;
  out[GROUP_OFF.stemIndex] = stemIndex & 255;
  out[GROUP_OFF.flags] = GROUP_FLAGS_V13;
  put32(out, GROUP_OFF.groupIndex, groupIndex);

  const firstFrame = groupIndex * FRAMES_PER_GROUP;
  const frameCount = Math.max(0, Math.min(FRAMES_PER_GROUP, song.frames - firstFrame));
  const pcm = stemPcm16(song.stems[stemIndex]!);
  for (let f = 0; f < frameCount; f++) {
    const dst = GROUP_HEADER_BYTES + f * BYTES_PER_STEM_FRAME;
    const src = (firstFrame + f) * BYTES_PER_STEM_FRAME;
    for (let b = 0; b < BYTES_PER_STEM_FRAME; b++) out[dst + b] = pcm[src + b]!;
  }
  return out;
}

export function readGroupHeader(group: Uint8Array): GroupHeader & { magicOk: boolean } {
  return {
    magicOk: group[GROUP_OFF.magic0] === GROUP_MAGIC_0 && group[GROUP_OFF.magic1] === GROUP_MAGIC_1,
    stemIndex: group[GROUP_OFF.stemIndex]!,
    flags: group[GROUP_OFF.flags]!,
    groupIndex: get32(group, GROUP_OFF.groupIndex),
  };
}

/**
 * Encode one 8,192-byte logical sector: four consecutive groups of the
 * stem-major group stream, i.e. global groups [i*4, i*4+4). Groups never
 * straddle a sector edge, and the stream length (4 stems * groups) is always a
 * multiple of 4, so sectorCount === groupsPerStem exactly as in v1.1.
 */
export function encodeSector(song: CanonicalSong, sectorIndex: number): Uint8Array {
  const groups = groupsForFrames(song.frames);
  const out = new Uint8Array(SECTOR_BYTES);
  for (let k = 0; k < GROUPS_PER_SECTOR; k++) {
    const g = sectorIndex * GROUPS_PER_SECTOR + k;
    const stem = Math.floor(g / groups);
    const groupIndex = g - stem * groups;
    if (stem >= STEM_COUNT) break;
    out.set(encodeGroup(song, stem, groupIndex), k * GROUP_BYTES);
  }
  return out;
}

export function encodeSong(song: CanonicalSong): Uint8Array[] {
  const n = sectorsForFrames(song.frames);
  const sectors: Uint8Array[] = [];
  for (let i = 0; i < n; i++) sectors.push(encodeSector(song, i));
  return sectors;
}

/** Split one logical sector into its four 2,048-byte planar groups. */
export function sectorToGroups(sector: Uint8Array): Uint8Array[] {
  if (sector.length !== SECTOR_BYTES) throw new Error(`sector is ${sector.length} bytes, expected ${SECTOR_BYTES}`);
  const out: Uint8Array[] = [];
  for (let k = 0; k < GROUPS_PER_SECTOR; k++) out.push(sector.subarray(k * GROUP_BYTES, (k + 1) * GROUP_BYTES));
  return out;
}

/** v1.1 mixed-sector header reader. Frozen-bundle audit only. */
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
  for (let s = 0; s < STEM_COUNT; s++) stems.push(new Uint8Array(frames * BYTES_PER_STEM_FRAME));
  const groupsPerStem = groupsForFrames(frames);

  for (let i = 0; i < sectors.length; i++) {
    const sector = sectors[i]!;
    if (sector.length !== SECTOR_BYTES) throw new Error(`sector is ${sector.length} bytes, expected ${SECTOR_BYTES}`);
    for (let k = 0; k < GROUPS_PER_SECTOR; k++) {
      const gIndex = i * GROUPS_PER_SECTOR + k;
      const stem = Math.floor(gIndex / groupsPerStem);
      if (stem >= STEM_COUNT) break;
      const expectGroup = gIndex - stem * groupsPerStem;
      const group = sector.subarray(k * GROUP_BYTES, (k + 1) * GROUP_BYTES);
      const h = readGroupHeader(group);
      if (!h.magicOk) throw new Error(`group ${gIndex} magic mismatch (expected 'PL')`);
      if (h.flags !== GROUP_FLAGS_V13) {
        throw new Error(`group ${gIndex} carries flags ${h.flags}, expected ${GROUP_FLAGS_V13} (v1.3)`);
      }
      if (h.stemIndex !== stem) throw new Error(`group ${gIndex} reports stem ${h.stemIndex}, expected ${stem}`);
      if (h.groupIndex !== expectGroup) {
        throw new Error(`group ${gIndex} reports groupIndex ${h.groupIndex}, expected ${expectGroup}`);
      }
      const firstFrame = expectGroup * FRAMES_PER_GROUP;
      const count = Math.max(0, Math.min(FRAMES_PER_GROUP, frames - firstFrame));
      for (let f = 0; f < count; f++) {
        const src = GROUP_HEADER_BYTES + f * BYTES_PER_STEM_FRAME;
        const dst = (firstFrame + f) * BYTES_PER_STEM_FRAME;
        for (let b = 0; b < BYTES_PER_STEM_FRAME; b++) stems[stem]![dst + b] = group[src + b]!;
      }
    }
  }
  // v1.3 song data carries no tempo: bpm and downbeat live in the STIX record.
  return { frames, stems, bpm: 0, downbeatFrame: 0 };
}

/** v1.1 mixed-frame decoder. Frozen v1.1 handoff-bundle audit only. */
export function decodeSectorsV11(sectors: Uint8Array[], frames: number): DecodedSong {
  const stems: Uint8Array[] = [];
  for (let s = 0; s < STEM_COUNT; s++) stems.push(new Uint8Array(frames * CHANNELS * BYTES_PER_SAMPLE_V11));
  let bpmQ8 = 0;
  let downbeatFrame = 0;
  for (const sector of sectors) {
    if (sector.length !== SECTOR_BYTES) throw new Error(`sector is ${sector.length} bytes, expected ${SECTOR_BYTES}`);
    const h = readSectorHeader(sector);
    if (h.magic !== SECTOR_MAGIC) throw new Error(`sector ${h.sectorIndex} magic mismatch`);
    bpmQ8 = h.bpmQ8;
    downbeatFrame = h.downbeatFrame;
    for (let f = 0; f < h.frameCount; f++) {
      const src = SECTOR_HEADER_BYTES + f * BYTES_PER_FRAME_V11;
      const dst = (h.firstFrame + f) * CHANNELS * BYTES_PER_SAMPLE_V11;
      for (let s = 0; s < STEM_COUNT; s++) {
        for (let b = 0; b < BYTES_PER_STEM_FRAME_V12; b++) {
          stems[s]![dst + b] = sector[src + s * BYTES_PER_STEM_FRAME_V12 + b]!;
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
  return readInt16LE(stem, frame * BYTES_PER_STEM_FRAME + channel * 2);
}
