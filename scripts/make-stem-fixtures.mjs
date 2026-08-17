/**
 * Deterministic four-stem WAV fixture generator.
 *
 * Run: bun scripts/make-stem-fixtures.mjs
 * Output: src/sp1/__tests__/fixtures/{vocal,drums,bass,instrument}.wav
 *
 * The fixtures deliberately differ in sample rate, channel count and length so
 * the end-to-end test exercises resampling, mono duplication and shared-length
 * zero padding. Signals are integer-exact so regenerating never changes bytes.
 */
import { mkdirSync, writeFileSync } from "node:fs";
import { createHash } from "node:crypto";

const OUT = new URL("../src/sp1/__tests__/fixtures/", import.meta.url);
mkdirSync(OUT, { recursive: true });

/** 16-bit PCM WAV writer (integer samples in, no float rounding). */
function wav(channels, sampleRate) {
  const numChannels = channels.length;
  const frames = channels[0].length;
  const dataBytes = frames * numChannels * 2;
  const b = Buffer.alloc(44 + dataBytes);
  b.write("RIFF", 0, "ascii");
  b.writeUInt32LE(36 + dataBytes, 4);
  b.write("WAVE", 8, "ascii");
  b.write("fmt ", 12, "ascii");
  b.writeUInt32LE(16, 16);
  b.writeUInt16LE(1, 20);
  b.writeUInt16LE(numChannels, 22);
  b.writeUInt32LE(sampleRate, 24);
  b.writeUInt32LE(sampleRate * numChannels * 2, 28);
  b.writeUInt16LE(numChannels * 2, 32);
  b.writeUInt16LE(16, 34);
  b.write("data", 36, "ascii");
  b.writeUInt32LE(dataBytes, 40);
  let o = 44;
  for (let i = 0; i < frames; i++) {
    for (let c = 0; c < numChannels; c++) {
      b.writeInt16LE(channels[c][i], o);
      o += 2;
    }
  }
  return b;
}

/** Deterministic integer sawtooth-ish tone, distinct per stem and channel. */
function tone(frames, seed, amp) {
  const a = new Int16Array(frames);
  for (let i = 0; i < frames; i++) a[i] = (((i * seed) % 2003) - 1001) * amp;
  return a;
}

const specs = [
  { name: "vocal", rate: 48000, frames: 14592, chans: [tone(14592, 7, 16), tone(14592, 11, 14)] },
  { name: "drums", rate: 44100, frames: 13000, chans: [tone(13000, 13, 12), tone(13000, 17, 10)] },
  { name: "bass", rate: 48000, frames: 14000, chans: [tone(14000, 19, 20)] },
  { name: "instrument", rate: 44100, frames: 11025, chans: [tone(11025, 23, 8)] },
];

for (const s of specs) {
  const buf = wav(s.chans, s.rate);
  writeFileSync(new URL(`${s.name}.wav`, OUT), buf);
  const sha = createHash("sha256").update(buf).digest("hex");
  console.log(`${s.name}.wav  ${s.rate} Hz  ${s.chans.length} ch  ${s.frames} frames  ${buf.length} B  sha256=${sha}`);
}
