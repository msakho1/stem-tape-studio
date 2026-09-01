/**
 * Canonical prepared-song model.
 *
 * This layer is deliberately independent of any device wire format: it
 * produces four synchronised stereo, 48 kHz, signed 24-bit little-endian PCM
 * streams plus complete metadata. Device sector size, padding, addressing and
 * commit behaviour belong to a StemTapeDeviceTransport, never here.
 *
 * Hard invariants for a song of N frames:
 *   channels === 2
 *   decoded PCM per stem      === N * 2 samples
 *   packed PCM per stem       === N * 2 * 3 bytes   (before device padding)
 *   packed PCM for four stems === N * 4 * 2 * 3 bytes
 */

import { downmixToMono, type StemSlotName } from "./prepare";

export const INT16_MAX = 32767;
export const INT16_MIN = -32768;

/**
 * Signed 24-bit (Q23) sample -> signed 16-bit, round-to-nearest, saturating,
 * NO dither. v1.3 storage is 16-bit; this host model stays 24-bit because that
 * is what decoding and resampling produce, so the conversion lives here, in
 * one place, and every checksum is taken from its output.
 *
 * Rounding rather than truncation is measured, not stylistic: truncation
 * biases every sample toward negative infinity (a DC offset across the whole
 * mix), and dither is wrong here because the device re-quantises after summing
 * four stems. There is deliberately no RNG in this path.
 */
export function to16(v: number): number {
  const q = (v + 128) >> 8;
  return q > INT16_MAX ? INT16_MAX : q < INT16_MIN ? INT16_MIN : q;
}

/** Interleaved stereo 24-bit LE (6 B/frame) -> interleaved 16-bit LE (4 B/frame). */
export function pcm24ToPcm16(pcm24: Uint8Array): Uint8Array {
  const samples = Math.floor(pcm24.length / 3);
  const out = new Uint8Array(samples * 2);
  for (let i = 0; i < samples; i++) {
    const s = i * 3;
    const raw = pcm24[s]! | (pcm24[s + 1]! << 8) | (pcm24[s + 2]! << 16);
    const q = to16(raw & 0x800000 ? raw - 0x1000000 : raw);
    const u = q < 0 ? q + 0x10000 : q;
    out[i * 2] = u & 0xff;
    out[i * 2 + 1] = (u >>> 8) & 0xff;
  }
  return out;
}

/** Read one signed 16-bit LE sample. */
export function readInt16LE(bytes: Uint8Array, off: number): number {
  const v = bytes[off]! | (bytes[off + 1]! << 8);
  return v & 0x8000 ? v - 0x10000 : v;
}

/** The stored 16-bit image of a prepared stem, converted once per buffer. */
const PCM16 = new WeakMap<Uint8Array, Uint8Array>();
export function stemPcm16(stem: { pcm24: Uint8Array }): Uint8Array {
  const hit = PCM16.get(stem.pcm24);
  if (hit) return hit;
  const made = pcm24ToPcm16(stem.pcm24);
  PCM16.set(stem.pcm24, made);
  return made;
}

export const CANONICAL_SAMPLE_RATE = 48000;
export const CANONICAL_CHANNELS = 2;
export const CANONICAL_PCM_DEPTH = 24;
export const BYTES_PER_SAMPLE = CANONICAL_PCM_DEPTH / 8;
export const SONG_SCHEMA_VERSION = "stem-tape-song/1";

export interface SongMetadata {
  title: string;
  artist: string;
  /** Beats per minute. Required — Gate, Delay and loops are tempo-locked. */
  bpm: number;
  /** Beat zero, in seconds from the start of the song. Required. */
  downbeatSeconds: number;
}

export interface CanonicalStem {
  name: StemSlotName;
  filename: string;
  source: { sampleRate: number; channels: number; duration: number; frames: number };
  /** Frames decoded from the source after resampling, before shared padding. */
  originalFrames: number;
  /** Shared song frame count N after zero padding. */
  frames: number;
  /** Interleaved signed 24-bit LE PCM: frames * 2 * 3 bytes exactly. */
  pcm24: Uint8Array;
  /** Peak measured on the final 24-bit values, before the 16-bit storage step. */
  peak: number;
  clipped: boolean;
  padFrames: number;
  checksum: number;
}

export interface CanonicalSong {
  schemaVersion: string;
  sampleRate: number;
  channels: number;
  pcmDepth: number;
  /** Shared frame count N. */
  frames: number;
  durationSeconds: number;
  stems: CanonicalStem[];
  metadata: SongMetadata;
  /** Difference between shortest and longest source, in seconds. */
  lengthSpreadSeconds: number;
  audioBytes: number;
  checksum: number;
}

/* ---------- byte helpers ---------- */

export function checksum32(bytes: Uint8Array): number {
  let h = 0x811c9dc5;
  for (let i = 0; i < bytes.length; i++) {
    h ^= bytes[i]!;
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h >>> 0;
}

export const INT24_MAX = 8388607;
export const INT24_MIN = -8388608;

export function floatToInt24(x: number): number {
  const v = Math.round(x * INT24_MAX);
  return v > INT24_MAX ? INT24_MAX : v < INT24_MIN ? INT24_MIN : v;
}

export function readInt24LE(bytes: Uint8Array, byteOffset: number): number {
  const v = bytes[byteOffset]! | (bytes[byteOffset + 1]! << 8) | (bytes[byteOffset + 2]! << 16);
  return v & 0x800000 ? v - 0x1000000 : v;
}

/** Interleave two float channels into signed 24-bit LE PCM of exactly N*2*3 bytes. */
export function packStereo24(left: Float32Array, right: Float32Array, frames: number): Uint8Array {
  const out = new Uint8Array(frames * CANONICAL_CHANNELS * BYTES_PER_SAMPLE);
  let o = 0;
  for (let i = 0; i < frames; i++) {
    for (let c = 0; c < 2; c++) {
      const src = c === 0 ? left : right;
      const v = floatToInt24(i < src.length ? src[i]! : 0);
      const u = v < 0 ? v + 0x1000000 : v;
      out[o++] = u & 0xff;
      out[o++] = (u >>> 8) & 0xff;
      out[o++] = (u >>> 16) & 0xff;
    }
  }
  return out;
}

/** Peak and clipping measured on the packed 24-bit values, not on the float source. */
export function peakOfPcm24(pcm: Uint8Array): { peak: number; clipped: boolean } {
  let peak = 0;
  let clipped = false;
  for (let o = 0; o + 2 < pcm.length; o += 3) {
    const v = readInt24LE(pcm, o);
    if (v >= INT24_MAX || v <= INT24_MIN) clipped = true;
    const a = Math.abs(v) / INT24_MAX;
    if (a > peak) peak = a;
  }
  return { peak, clipped };
}

/* ---------- preparation ---------- */

export type OfflineCtxFactory = (channels: number, length: number, rate: number) => OfflineAudioContext;
const defaultOffline: OfflineCtxFactory = (ch, len, rate) => new OfflineAudioContext(ch, len, rate);

/**
 * Resample any AudioBuffer to canonical 48 kHz stereo.
 *  1 ch  -> duplicated to both outputs
 *  2 ch  -> preserved
 *  >2 ch -> explicit equal-weight (1/N) average fed to both outputs
 */
export async function resampleToStereo48k(
  buffer: AudioBuffer,
  make: OfflineCtxFactory = defaultOffline,
): Promise<{ left: Float32Array; right: Float32Array }> {
  const chans: Float32Array[] = [];
  for (let c = 0; c < buffer.numberOfChannels; c++) chans.push(buffer.getChannelData(c));

  const pick = (source: Float32Array[]) => {
    if (source.length === 1) return { left: source[0]!, right: source[0]! };
    if (source.length === 2) return { left: source[0]!, right: source[1]! };
    const mixed = downmixToMono(source);
    return { left: mixed, right: mixed };
  };

  if (buffer.sampleRate === CANONICAL_SAMPLE_RATE) {
    const p = pick(chans);
    return { left: p.left.slice(0), right: p.right.slice(0) };
  }

  const frames = Math.max(1, Math.ceil(buffer.duration * CANONICAL_SAMPLE_RATE));
  const off = make(2, frames, CANONICAL_SAMPLE_RATE);
  const src = off.createBufferSource();
  src.buffer = buffer;
  src.connect(off.destination);
  src.start();
  const rendered = await off.startRendering();
  const l = rendered.getChannelData(0).slice(0);
  const r = rendered.numberOfChannels > 1 ? rendered.getChannelData(1).slice(0) : l.slice(0);
  return { left: l, right: r };
}

export interface PrepareSongInput {
  name: StemSlotName;
  filename: string;
  buffer: AudioBuffer;
}

export interface PrepareSongOptions {
  metadata: SongMetadata;
  make?: OfflineCtxFactory;
  onStage?: (stage: string, fraction: number) => void;
}

export async function prepareCanonicalSong(
  inputs: PrepareSongInput[],
  opts: PrepareSongOptions,
): Promise<CanonicalSong> {
  const rendered: { input: PrepareSongInput; left: Float32Array; right: Float32Array }[] = [];
  for (let i = 0; i < inputs.length; i++) {
    const input = inputs[i]!;
    opts.onStage?.("resampling", i / inputs.length);
    const { left, right } = await resampleToStereo48k(input.buffer, opts.make);
    rendered.push({ input, left, right });
  }

  const lengths = rendered.map((r) => Math.max(r.left.length, r.right.length));
  const N = Math.max(...lengths, 1);
  const shortest = Math.min(...lengths, N);

  const stems: CanonicalStem[] = rendered.map(({ input, left, right }, i) => {
    opts.onStage?.("packing", i / rendered.length);
    const original = Math.max(left.length, right.length);
    const pcm24 = packStereo24(left, right, N);
    const { peak, clipped } = peakOfPcm24(pcm24);
    return {
      name: input.name,
      filename: input.filename,
      source: {
        sampleRate: input.buffer.sampleRate,
        channels: input.buffer.numberOfChannels,
        duration: input.buffer.duration,
        frames: input.buffer.length,
      },
      originalFrames: original,
      frames: N,
      pcm24,
      peak,
      clipped,
      padFrames: N - original,
      checksum: checksum32(pcm24ToPcm16(pcm24)),
    };
  });

  const audioBytes = stems.reduce((a, s) => a + s.pcm24.length, 0);
  const digest = new Uint8Array(stems.length * 4);
  stems.forEach((s, i) => {
    digest[i * 4] = s.checksum & 0xff;
    digest[i * 4 + 1] = (s.checksum >>> 8) & 0xff;
    digest[i * 4 + 2] = (s.checksum >>> 16) & 0xff;
    digest[i * 4 + 3] = (s.checksum >>> 24) & 0xff;
  });

  const song: CanonicalSong = {
    schemaVersion: SONG_SCHEMA_VERSION,
    sampleRate: CANONICAL_SAMPLE_RATE,
    channels: CANONICAL_CHANNELS,
    pcmDepth: CANONICAL_PCM_DEPTH,
    frames: N,
    durationSeconds: N / CANONICAL_SAMPLE_RATE,
    stems,
    metadata: opts.metadata,
    lengthSpreadSeconds: (N - shortest) / CANONICAL_SAMPLE_RATE,
    audioBytes,
    checksum: checksum32(digest),
  };
  assertCanonicalSong(song);
  return song;
}

/** Hard invariants. These throw — they are not advisory. */
export function assertCanonicalSong(song: CanonicalSong): void {
  const fail = (m: string): never => {
    throw new Error(`canonical song invariant violated: ${m}`);
  };
  if (song.sampleRate !== CANONICAL_SAMPLE_RATE) fail(`sample rate ${song.sampleRate} !== 48000`);
  if (song.channels !== CANONICAL_CHANNELS) fail(`channel count ${song.channels} !== 2`);
  if (song.pcmDepth !== CANONICAL_PCM_DEPTH) fail(`pcm depth ${song.pcmDepth} !== 24`);
  if (song.stems.length !== 4) fail(`${song.stems.length} stems, expected exactly 4`);
  const N = song.frames;
  if (!Number.isInteger(N) || N <= 0) fail(`frame count ${N}`);
  for (const s of song.stems) {
    if (s.frames !== N) fail(`${s.name}: ${s.frames} frames !== shared ${N}`);
    const expect = N * CANONICAL_CHANNELS * BYTES_PER_SAMPLE;
    if (s.pcm24.length !== expect) fail(`${s.name}: ${s.pcm24.length} bytes !== ${expect}`);
    if (s.pcm24.length % 6 !== 0) fail(`${s.name}: packed bytes are not whole stereo 24-bit frames`);
    if (s.checksum !== checksum32(stemPcm16(s))) fail(`${s.name}: checksum does not match the stored 16-bit bytes`);
    if (s.padFrames < 0 || s.originalFrames + s.padFrames !== N) fail(`${s.name}: padding accounting`);
  }
  const total = N * 4 * CANONICAL_CHANNELS * BYTES_PER_SAMPLE;
  if (song.audioBytes !== total) fail(`song audio ${song.audioBytes} bytes !== ${total}`);
  if (!(song.metadata.bpm > 0)) fail("bpm is required and must be positive");
  if (!(song.metadata.downbeatSeconds >= 0)) fail("downbeat is required and must be >= 0");
}
