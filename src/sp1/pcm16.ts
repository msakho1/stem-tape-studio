/**
 * v1.3 sample width: signed 16-bit little-endian.
 *
 * The preparation pipeline still produces signed 24-bit (Q23) PCM, because
 * that is what decoding and resampling naturally yield. The STORAGE format is
 * 16-bit, and this module owns the single conversion between the two.
 *
 * The conversion is round-to-nearest, saturating, and carries NO dither. That
 * is measured, not stylistic (residual against the device's own mixdown):
 *
 *   truncate (v >> 8)        -93.3 dBFS, 2 LSB peak, +0.44 LSB DC bias
 *   round ((v + 128) >> 8)   -93.5 dBFS, 1 LSB peak, no DC bias
 *   TPDF dither              -89.3 dBFS, 4 LSB peak, no DC bias
 *
 * Truncation biases every sample toward negative infinity — a DC offset on the
 * whole mix. Dither costs 4 dB here because this quantisation is not final:
 * the device re-quantises to int16 after summing four stems, and that
 * undithered stage sets the error floor at every level. There is deliberately
 * no RNG in the encoder.
 */

import { BYTES_PER_STEM_FRAME, CHANNELS, STEM_COUNT } from "./stemTapeFormat";
import { checksum32, type CanonicalSong, type CanonicalStem } from "./song";

export const INT16_MAX = 32767;
export const INT16_MIN = -32768;

/** Signed 24-bit (Q23) sample -> signed 16-bit, round-to-nearest, saturating. */
export function to16(v: number): number {
  const q = (v + 128) >> 8; // round half away from -infinity
  return q > INT16_MAX ? INT16_MAX : q < INT16_MIN ? INT16_MIN : q;
}

/** Interleaved stereo 24-bit LE (6 B/frame) -> interleaved 16-bit LE (4 B/frame). */
export function pcm24ToPcm16(pcm24: Uint8Array): Uint8Array {
  const frames = Math.floor(pcm24.length / (CHANNELS * 3));
  const out = new Uint8Array(frames * BYTES_PER_STEM_FRAME);
  let o = 0;
  for (let i = 0; i < frames; i++) {
    for (let c = 0; c < CHANNELS; c++) {
      const s = (i * CHANNELS + c) * 3;
      const raw = pcm24[s]! | (pcm24[s + 1]! << 8) | (pcm24[s + 2]! << 16);
      const signed = raw & 0x800000 ? raw - 0x1000000 : raw;
      const q = to16(signed);
      const u = q < 0 ? q + 0x10000 : q;
      out[o++] = u & 0xff;
      out[o++] = (u >>> 8) & 0xff;
    }
  }
  return out;
}

/** Read one signed 16-bit LE sample. */
export function readInt16LE(bytes: Uint8Array, off: number): number {
  const v = bytes[off]! | (bytes[off + 1]! << 8);
  return v & 0x8000 ? v - 0x10000 : v;
}

/**
 * The 16-bit image of one prepared stem, cached per pcm24 buffer. Encoding a
 * song walks every group of every stem, so the conversion runs exactly once.
 */
const cache = new WeakMap<Uint8Array, Uint8Array>();

export function stemPcm16(stem: CanonicalStem): Uint8Array {
  const hit = cache.get(stem.pcm24);
  if (hit) return hit;
  const made = pcm24ToPcm16(stem.pcm24);
  cache.set(stem.pcm24, made);
  return made;
}

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
