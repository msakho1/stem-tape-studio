/**
 * Streaming WAV encoder (plan §L, M12).
 *
 * Pure header + sample conversion so the byte layout is unit-tested exactly.
 * The worker streams chunk-by-chunk through `encodeChunk` — no whole-project
 * PCM copy is ever materialised on the main thread.
 */

export type BitDepth = 16 | 24;

export interface WavSpec {
  sampleRate: number;
  channels: number;
  bitDepth: BitDepth;
  /** Total frames the header will declare. Must match the streamed payload. */
  frames: number;
}

export function wavDataBytes(spec: WavSpec): number {
  return spec.frames * spec.channels * (spec.bitDepth / 8);
}

export function wavTotalBytes(spec: WavSpec): number {
  return 44 + wavDataBytes(spec);
}

/** Canonical 44-byte RIFF/WAVE PCM header. */
export function wavHeader(spec: WavSpec): Uint8Array {
  const dataBytes = wavDataBytes(spec);
  const buf = new ArrayBuffer(44);
  const v = new DataView(buf);
  const ascii = (off: number, s: string) => {
    for (let i = 0; i < s.length; i++) v.setUint8(off + i, s.charCodeAt(i));
  };
  const blockAlign = spec.channels * (spec.bitDepth / 8);
  ascii(0, "RIFF");
  v.setUint32(4, 36 + dataBytes, true);
  ascii(8, "WAVE");
  ascii(12, "fmt ");
  v.setUint32(16, 16, true);
  v.setUint16(20, 1, true); // PCM
  v.setUint16(22, spec.channels, true);
  v.setUint32(24, spec.sampleRate, true);
  v.setUint32(28, spec.sampleRate * blockAlign, true);
  v.setUint16(32, blockAlign, true);
  v.setUint16(34, spec.bitDepth, true);
  ascii(36, "data");
  v.setUint32(40, dataBytes, true);
  return new Uint8Array(buf);
}

function clamp(v: number): number {
  return v > 1 ? 1 : v < -1 ? -1 : v;
}

/**
 * Interleave and quantise one chunk. TPDF dither at 16-bit only; 24-bit is
 * transparent enough that dither would add noise for no benefit.
 */
export function encodeChunk(channels: Float32Array[], frames: number, bitDepth: BitDepth, dither = true): Uint8Array {
  const ch = channels.length;
  const bytes = bitDepth / 8;
  const out = new Uint8Array(frames * ch * bytes);
  const v = new DataView(out.buffer);
  let off = 0;
  for (let i = 0; i < frames; i++) {
    for (let c = 0; c < ch; c++) {
      let s = clamp(channels[c]![i] ?? 0);
      if (bitDepth === 16) {
        if (dither) s += ((Math.random() + Math.random() - 1) / 32768) * 0.5;
        v.setInt16(off, Math.max(-32768, Math.min(32767, Math.round(s * 32767))), true);
        off += 2;
      } else {
        const q = Math.max(-8388608, Math.min(8388607, Math.round(s * 8388607)));
        const u = q < 0 ? q + 0x1000000 : q;
        out[off] = u & 0xff;
        out[off + 1] = (u >> 8) & 0xff;
        out[off + 2] = (u >> 16) & 0xff;
        off += 3;
      }
    }
  }
  return out;
}

/** Reads back a header — used by the tests to assert exactness, not by the app. */
export function parseWavHeader(bytes: Uint8Array): {
  riff: string;
  wave: string;
  channels: number;
  sampleRate: number;
  bitDepth: number;
  dataBytes: number;
  riffSize: number;
} {
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const str = (o: number, n: number) => String.fromCharCode(...Array.from(bytes.slice(o, o + n)));
  return {
    riff: str(0, 4),
    wave: str(8, 4),
    channels: v.getUint16(22, true),
    sampleRate: v.getUint32(24, true),
    bitDepth: v.getUint16(34, true),
    dataBytes: v.getUint32(40, true),
    riffSize: v.getUint32(4, true),
  };
}

/**
 * Honest export-size verdict (M12): measured, not a fixed iOS constant. The
 * caller supplies the measured single-blob ceiling for this device.
 */
export function exportPlan(spec: WavSpec, measuredSafeBytes: number): { mode: "single" | "segmented" | "16-bit"; detail: string; segments: number } {
  const total = wavTotalBytes(spec);
  if (total <= measuredSafeBytes)
    return { mode: "single", detail: `${(total / 1048576).toFixed(1)} MiB single file, within the measured ${(measuredSafeBytes / 1048576).toFixed(0)} MiB safe ceiling`, segments: 1 };
  if (spec.bitDepth === 24) {
    const at16 = wavTotalBytes({ ...spec, bitDepth: 16 });
    if (at16 <= measuredSafeBytes)
      return { mode: "16-bit", detail: `24-bit export is ${(total / 1048576).toFixed(1)} MiB, above the measured ceiling — 16-bit fits at ${(at16 / 1048576).toFixed(1)} MiB`, segments: 1 };
  }
  const segments = Math.ceil(total / measuredSafeBytes);
  return { mode: "segmented", detail: `${(total / 1048576).toFixed(1)} MiB exceeds the measured ceiling — delivered as ${segments} segments`, segments };
}
