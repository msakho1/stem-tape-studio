/**
 * Canonical audio preparation for the SP-1 companion uploader.
 *
 * The on-device format is fixed by the transfer contract, NOT by preference:
 *   - MONO int16 little-endian PCM
 *   - device native sample rate (24 kHz or 48 kHz, read from the index magic)
 *   - 256 samples per 512-byte block; the tail block is zero-filled
 * There is no stereo and no 24-bit variant in this protocol, so no such
 * packing is invented here.
 *
 * Everything runs locally: decode, resample, downmix, pack. No network I/O.
 */

import { SAMPLES_PER_BLOCK } from "./protocol";

export type StemSlotName = "vocal" | "drums" | "bass" | "instrument";
export const STEM_ORDER: StemSlotName[] = ["vocal", "drums", "bass", "instrument"];
export const STEM_LABEL: Record<StemSlotName, string> = {
  vocal: "Vocal",
  drums: "Drums",
  bass: "Bass",
  instrument: "Instrument",
};

export interface DecodedInfo {
  sampleRate: number;
  channels: number;
  duration: number;
  frames: number;
}

export interface PreparedStem {
  name: StemSlotName;
  filename: string;
  source: DecodedInfo;
  /** Mono float at the device rate, before block padding. */
  mono: Float32Array;
  /** Packed int16 LE, padded to whole 512-byte blocks. */
  bytes: Uint8Array;
  blocks: number;
  peak: number;
  clipped: boolean;
  outputBytes: number;
  /** Samples of digital silence appended to match the longest stem. */
  padSamples: number;
}

/**
 * Explicit documented downmix:
 *  1 ch  -> duplicated (mono is already the device format; the value is used as-is,
 *           and both device outputs hear it)
 *  2 ch  -> 0.5*L + 0.5*R
 *  >2 ch -> equal-weight average of every channel (1/N), the only downmix the
 *           firmware contract leaves to the host
 */
export function downmixToMono(channels: Float32Array[]): Float32Array {
  const n = channels[0]?.length ?? 0;
  if (channels.length === 1) return channels[0]!.slice(0);
  const out = new Float32Array(n);
  const w = 1 / channels.length;
  for (let c = 0; c < channels.length; c++) {
    const src = channels[c]!;
    for (let i = 0; i < n; i++) out[i]! += src[i]! * w;
  }
  return out;
}

export function peakOf(samples: Float32Array): number {
  let p = 0;
  for (let i = 0; i < samples.length; i++) {
    const a = Math.abs(samples[i]!);
    if (a > p) p = a;
  }
  return p;
}

/** int16 LE packing, padded with digital silence to whole blocks. */
export function packInt16Blocks(mono: Float32Array, totalSamples?: number) {
  const blocks = Math.max(1, Math.ceil((totalSamples ?? mono.length) / SAMPLES_PER_BLOCK));
  const total = blocks * SAMPLES_PER_BLOCK;
  const i16 = new Int16Array(total);
  const n = Math.min(mono.length, total);
  for (let i = 0; i < n; i++) {
    const x = Math.round(mono[i]! * 32767);
    i16[i] = x > 32767 ? 32767 : x < -32768 ? -32768 : x;
  }
  return { bytes: new Uint8Array(i16.buffer), blocks, totalSamples: total };
}

export type OfflineCtxFactory = (channels: number, length: number, rate: number) => OfflineAudioContext;

const defaultOffline: OfflineCtxFactory = (ch, len, rate) => new OfflineAudioContext(ch, len, rate);

/** Resample any AudioBuffer to the device rate, mono, using OfflineAudioContext. */
export async function resampleToDeviceRate(
  buffer: AudioBuffer,
  deviceRate: number,
  make: OfflineCtxFactory = defaultOffline,
): Promise<Float32Array> {
  const chans: Float32Array[] = [];
  for (let c = 0; c < buffer.numberOfChannels; c++) chans.push(buffer.getChannelData(c));
  if (buffer.sampleRate === deviceRate) return downmixToMono(chans);

  const frames = Math.max(1, Math.ceil(buffer.duration * deviceRate));
  const off = make(1, frames, deviceRate);
  const src = off.createBufferSource();
  src.buffer = buffer;
  src.connect(off.destination);
  src.start();
  const rendered = await off.startRendering();
  return rendered.getChannelData(0).slice(0);
}

export interface PrepareInput {
  name: StemSlotName;
  filename: string;
  buffer: AudioBuffer;
}

export interface PrepareOptions {
  deviceRate: number;
  /** Hard cap from the device layout — never silently truncate past this. */
  maxBlocks: number;
  make?: OfflineCtxFactory;
  onStage?: (stage: string, fraction: number) => void;
}

export interface PrepareResult {
  stems: PreparedStem[];
  /** Longest stem length in samples; every stem is zero-padded to it. */
  alignedSamples: number;
  blocks: number;
  /** Difference between shortest and longest source, in seconds. */
  lengthSpreadSeconds: number;
  truncated: boolean;
}

/**
 * Decode -> device rate -> mono -> equal length (zero padded) -> int16 blocks.
 * Never time-stretched. Never normalized. Truncation only when a stem exceeds
 * the device's per-track region, and it is reported, never silent.
 */
export async function prepareFourStems(inputs: PrepareInput[], opts: PrepareOptions): Promise<PrepareResult> {
  const monos: { input: PrepareInput; mono: Float32Array }[] = [];
  for (let i = 0; i < inputs.length; i++) {
    const input = inputs[i]!;
    opts.onStage?.("resampling", i / inputs.length);
    monos.push({ input, mono: await resampleToDeviceRate(input.buffer, opts.deviceRate, opts.make) });
  }

  const lengths = monos.map((m) => m.mono.length);
  const longest = Math.max(...lengths, 1);
  const shortest = Math.min(...lengths, longest);
  const maxSamples = opts.maxBlocks * SAMPLES_PER_BLOCK;
  const aligned = Math.min(longest, maxSamples);
  const truncated = longest > maxSamples;

  const stems: PreparedStem[] = monos.map(({ input, mono }, i) => {
    opts.onStage?.("packing", i / monos.length);
    const clip = mono.length > aligned ? mono.subarray(0, aligned) : mono;
    const packed = packInt16Blocks(clip, aligned);
    const peak = peakOf(clip);
    return {
      name: input.name,
      filename: input.filename,
      source: {
        sampleRate: input.buffer.sampleRate,
        channels: input.buffer.numberOfChannels,
        duration: input.buffer.duration,
        frames: input.buffer.length,
      },
      mono: clip as Float32Array,
      bytes: packed.bytes,
      blocks: packed.blocks,
      peak,
      clipped: peak >= 0.999,
      outputBytes: packed.bytes.length,
      padSamples: Math.max(0, aligned - clip.length),
    };
  });

  return {
    stems,
    alignedSamples: aligned,
    blocks: stems[0]?.blocks ?? 0,
    lengthSpreadSeconds: (longest - shortest) / opts.deviceRate,
    truncated,
  };
}

/**
 * Pre-upload package validation: decode the packed bytes back and compare a
 * set of known frames against the float source, plus block/length invariants.
 */
export function validatePackage(result: PrepareResult, probes = 16): { ok: boolean; detail: string } {
  if (!result.stems.length) return { ok: false, detail: "no stems prepared" };
  const blocks = result.stems[0]!.blocks;
  for (const stem of result.stems) {
    if (stem.blocks !== blocks) return { ok: false, detail: `${stem.name}: block count ${stem.blocks} != ${blocks}` };
    if (stem.bytes.length !== blocks * 512) {
      return { ok: false, detail: `${stem.name}: ${stem.bytes.length} bytes is not ${blocks} whole blocks` };
    }
    const view = new Int16Array(stem.bytes.buffer, stem.bytes.byteOffset, stem.bytes.length / 2);
    for (let p = 0; p < probes; p++) {
      const idx = Math.min(stem.mono.length - 1, Math.floor((p / probes) * stem.mono.length));
      if (idx < 0) break;
      const expect = Math.max(-32768, Math.min(32767, Math.round(stem.mono[idx]! * 32767)));
      if (view[idx] !== expect) {
        return { ok: false, detail: `${stem.name}: frame ${idx} decoded ${view[idx]}, expected ${expect}` };
      }
    }
    for (let i = stem.mono.length; i < view.length; i++) {
      if (view[i] !== 0) return { ok: false, detail: `${stem.name}: pad frame ${i} is not silent` };
    }
  }
  return { ok: true, detail: `${result.stems.length} stems · ${blocks} blocks each · frames verified` };
}

/** Tap-tempo helper: median-of-intervals BPM from tap timestamps (ms). */
export function bpmFromTaps(taps: number[]): number | null {
  if (taps.length < 2) return null;
  const gaps: number[] = [];
  for (let i = 1; i < taps.length; i++) gaps.push(taps[i]! - taps[i - 1]!);
  gaps.sort((a, b) => a - b);
  const mid = gaps.length >> 1;
  const median = gaps.length % 2 ? gaps[mid]! : (gaps[mid - 1]! + gaps[mid]!) / 2;
  if (median <= 0) return null;
  return Math.round((60000 / median) * 10) / 10;
}
