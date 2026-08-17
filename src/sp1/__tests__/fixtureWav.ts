/**
 * Test-only helpers for the four real WAV fixtures.
 *
 * decodeWavFixture is a strict 16-bit PCM WAV reader (the fixtures are written
 * by scripts/make-stem-fixtures.mjs). offlineStub is a deterministic linear
 * resampler standing in for OfflineAudioContext, which does not exist in the
 * node test runner. It is only used for sample-rate conversion; every byte-count
 * and packing assertion is still produced by the production code path.
 */
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import type { OfflineCtxFactory } from "../song";

export const FIXTURE_DIR = "src/sp1/__tests__/fixtures";

export function decodeWavFixture(name: string): AudioBuffer {
  const buf = readFileSync(resolve(process.cwd(), FIXTURE_DIR, `${name}.wav`));
  const v = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  if (buf.toString("ascii", 0, 4) !== "RIFF" || buf.toString("ascii", 8, 12) !== "WAVE") {
    throw new Error(`${name}.wav is not a RIFF/WAVE file`);
  }
  if (v.getUint16(20, true) !== 1 || v.getUint16(34, true) !== 16) {
    throw new Error(`${name}.wav is not 16-bit PCM`);
  }
  const channels = v.getUint16(22, true);
  const sampleRate = v.getUint32(24, true);
  const dataBytes = v.getUint32(40, true);
  const frames = dataBytes / (channels * 2);
  const data: Float32Array[] = Array.from({ length: channels }, () => new Float32Array(frames));
  let o = 44;
  for (let i = 0; i < frames; i++) {
    for (let c = 0; c < channels; c++) {
      data[c]![i] = v.getInt16(o, true) / 32768;
      o += 2;
    }
  }
  return {
    sampleRate,
    numberOfChannels: channels,
    length: frames,
    duration: frames / sampleRate,
    getChannelData: (c: number) => data[c]!,
  } as unknown as AudioBuffer;
}

/** Deterministic linear-interpolation stand-in for OfflineAudioContext. */
export const offlineStub: OfflineCtxFactory = (channels, length, rate) => {
  let source: AudioBuffer | null = null;
  const ctx = {
    createBufferSource() {
      return {
        set buffer(b: AudioBuffer) {
          source = b;
        },
        connect() {},
        start() {},
      };
    },
    destination: {},
    async startRendering(): Promise<AudioBuffer> {
      const src = source!;
      const ratio = src.sampleRate / rate;
      const out: Float32Array[] = Array.from({ length: channels }, () => new Float32Array(length));
      for (let c = 0; c < channels; c++) {
        const inCh = src.getChannelData(Math.min(c, src.numberOfChannels - 1));
        for (let i = 0; i < length; i++) {
          const x = i * ratio;
          const i0 = Math.floor(x);
          const i1 = Math.min(i0 + 1, src.length - 1);
          const f = x - i0;
          out[c]![i] = i0 >= src.length ? 0 : inCh[i0]! * (1 - f) + inCh[i1]! * f;
        }
      }
      return {
        sampleRate: rate,
        numberOfChannels: channels,
        length,
        duration: length / rate,
        getChannelData: (c: number) => out[c]!,
      } as unknown as AudioBuffer;
    },
  };
  return ctx as unknown as OfflineAudioContext;
};
