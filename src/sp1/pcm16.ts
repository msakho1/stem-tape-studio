/**
 * v1.3 sample width helpers.
 *
 * The conversion itself lives in `song.ts` beside the canonical PCM model;
 * this module is the device-side view of it: per-stem and whole-song
 * checksums taken from the bytes that are actually stored.
 */

import { STEM_COUNT } from "./stemTapeFormat";
import { checksum32, stemPcm16, type CanonicalSong } from "./song";

export { pcm24ToPcm16, readInt16LE, stemPcm16, to16, INT16_MAX, INT16_MIN } from "./song";

/** FNV-1a over each stem's own contiguous 16-bit PCM, in playback order. */
export function stemChecksums16(song: CanonicalSong): number[] {
  return song.stems.map((s) => checksum32(stemPcm16(s)));
}

/** FNV-1a over the 16-byte digest of the four stem checksums, u32 LE, in order. */
export function songChecksumFromStems(stemChecksums: number[]): number {
  const digest = new Uint8Array(STEM_COUNT * 4);
  for (let i = 0; i < STEM_COUNT; i++) {
    const v = stemChecksums[i] ?? 0;
    digest[i * 4] = v & 0xff;
    digest[i * 4 + 1] = (v >>> 8) & 0xff;
    digest[i * 4 + 2] = (v >>> 16) & 0xff;
    digest[i * 4 + 3] = (v >>> 24) & 0xff;
  }
  return checksum32(digest);
}

export function songChecksum16(song: CanonicalSong): number {
  return songChecksumFromStems(stemChecksums16(song));
}
