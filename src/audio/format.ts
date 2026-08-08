/**
 * Format validation (Phase 4.1).
 *
 * Rule: never trust MIME type or filename. Every file is sniffed from its
 * header bytes and then probed with a REAL decode — and that ONE decode
 * produces the AudioBuffer the engine adopts. There is no second decode.
 *
 * Frame-count contract:
 *  - WAV: the fmt + data chunk headers give an EXACT frame count, so the
 *    pre-decode estimate is exact (modulo resampling into the context rate).
 *  - MP3 / M4A / FLAC / AIFF-C: duration cannot be established reliably from
 *    the header alone (VBR, encoder padding, seek tables), so the pre-decode
 *    estimate is explicitly marked uncertain and computed conservatively.
 */

import { MiB } from "./memory";

export type FormatVerdict =
  | "contract-supported"
  | "browser-decode-supported"
  | "unsupported"
  | "malformed"
  | "decode-failed";

export type StemRole = "vocals" | "drums" | "bass" | "instruments";

export const STEM_ROLE_LIST: StemRole[] = ["vocals", "drums", "bass", "instruments"];

export const ROLE_LABEL: Record<StemRole, string> = {
  vocals: "vocals",
  drums: "drums",
  bass: "bass",
  instruments: "instruments / other",
};

/** Advisory only — sniffing decides. Bogus/empty MIME is expected and accepted. */
export const EXTENSION_ALLOWLIST = [".wav", ".mp3", ".m4a", ".aac", ".flac", ".aif", ".aiff"];
export const MIME_ALLOWLIST = [
  "audio/wav",
  "audio/x-wav",
  "audio/wave",
  "audio/vnd.wave",
  "audio/mpeg",
  "audio/mp4",
  "audio/aac",
  "audio/x-m4a",
  "audio/flac",
  "audio/x-flac",
  "audio/aiff",
  "audio/x-aiff",
];

export interface SniffResult {
  container: "wav" | "mp3" | "mp4" | "flac" | "aiff" | "unknown";
  /** WAV only, read from the fmt chunk. */
  bitDepth?: number;
  sampleRate?: number;
  channels?: number;
  /** WAV format tag: 1 = PCM, 3 = float, 0xFFFE = extensible. */
  pcm?: boolean;
  /** WAV only: bytes declared by the data chunk header. */
  dataBytes?: number;
  /** WAV only: EXACT frames = dataBytes / (channels * bitDepth/8). */
  frames?: number;
  malformed?: string;
}

const ascii = (v: DataView, off: number, len: number) => {
  let s = "";
  for (let i = 0; i < len; i++) s += String.fromCharCode(v.getUint8(off + i));
  return s;
};

/** Header sniffing over the first bytes of the file. No MIME involved. */
export function sniffHeader(head: ArrayBuffer): SniffResult {
  const v = new DataView(head);
  if (v.byteLength < 12) return { container: "unknown", malformed: "file shorter than any audio header" };

  const magic = ascii(v, 0, 4);

  if (magic === "RIFF" && ascii(v, 8, 4) === "WAVE") {
    const out: SniffResult = { container: "wav" };
    let off = 12;
    let sawFmt = false;
    while (off + 8 <= v.byteLength) {
      const id = ascii(v, off, 4);
      const size = v.getUint32(off + 4, true);
      if (id === "fmt " && off + 8 + 16 <= v.byteLength) {
        const tag = v.getUint16(off + 8, true);
        out.pcm = tag === 1 || tag === 3 || tag === 0xfffe;
        out.channels = v.getUint16(off + 10, true);
        out.sampleRate = v.getUint32(off + 12, true);
        out.bitDepth = v.getUint16(off + 22, true);
        sawFmt = true;
      } else if (id === "data") {
        out.dataBytes = size;
        const blockAlign = (out.channels ?? 0) * ((out.bitDepth ?? 0) / 8);
        if (blockAlign > 0) out.frames = Math.floor(size / blockAlign);
        break;
      }
      off += 8 + size + (size % 2);
    }
    if (!sawFmt) return { container: "wav", malformed: "RIFF/WAVE with no readable fmt chunk" };
    return out;
  }

  if (magic === "fLaC") return { container: "flac" };
  if (magic === "FORM" && (ascii(v, 8, 4) === "AIFF" || ascii(v, 8, 4) === "AIFC")) return { container: "aiff" };
  if (ascii(v, 4, 4) === "ftyp") return { container: "mp4" };
  if (ascii(v, 0, 3) === "ID3") return { container: "mp3" };
  if (v.getUint8(0) === 0xff && (v.getUint8(1) & 0xe0) === 0xe0) return { container: "mp3" };

  return { container: "unknown", malformed: "no recognised audio container header" };
}

export interface PreDecodeEstimate {
  /** Bytes the decoded AudioBuffer is expected to occupy. */
  decodedBytes: number;
  /** Encoded bytes resident during the decode. */
  encodedBytes: number;
  /** decodedBytes + encodedBytes. */
  peakBytes: number;
  durationSeconds: number | null;
  channels: number;
  /** true when the header could not give an exact frame count. */
  uncertain: boolean;
  basis: string;
}

/** Conservative bitrate floors for containers whose duration is not in the header. */
const ASSUMED_KBPS: Record<string, number> = { mp3: 128, mp4: 128, flac: 700, aiff: 1411 };

/**
 * Pre-decode estimate — stage 1 of the two-stage gate. It runs BEFORE any
 * AudioBuffer is allocated, so a clearly unsafe project is refused before the
 * allocation rather than after it.
 */
export function estimateFromSniff(sniff: SniffResult, fileBytes: number, contextRate: number): PreDecodeEstimate {
  if (sniff.container === "wav" && sniff.frames && sniff.sampleRate && sniff.channels) {
    // Exact: WAV frames are declared, not guessed. Resampling into the context
    // rate scales the retained buffer.
    const duration = sniff.frames / sniff.sampleRate;
    const framesInContext = Math.round(duration * contextRate);
    const decoded = framesInContext * sniff.channels * 4;
    return {
      decodedBytes: decoded,
      encodedBytes: fileBytes,
      peakBytes: decoded + fileBytes,
      durationSeconds: duration,
      channels: sniff.channels,
      uncertain: false,
      basis: `WAV data chunk: ${sniff.frames} frames @ ${sniff.sampleRate} Hz → ${contextRate} Hz context`,
    };
  }

  // Compressed / header-incomplete: assume the conservative (worst-case long)
  // duration from a low bitrate floor, and assume stereo unless told otherwise.
  const kbps = ASSUMED_KBPS[sniff.container] ?? 128;
  const duration = (fileBytes * 8) / (kbps * 1000);
  const channels = sniff.channels ?? 2;
  const decoded = Math.round(duration * contextRate * channels * 4);
  return {
    decodedBytes: decoded,
    encodedBytes: fileBytes,
    peakBytes: decoded + fileBytes,
    durationSeconds: duration,
    channels,
    uncertain: true,
    basis: `uncertain — ${sniff.container} duration is not in the header; conservative ${kbps} kbps / ${channels}ch assumption`,
  };
}

export interface ProbeResult {
  verdict: FormatVerdict;
  sniff: SniffResult;
  duration?: number;
  channels?: number;
  decodedSampleRate?: number;
  decodeMs?: number;
  error?: string;
  /** Compressed sources carry encoder padding — alignment caveat. */
  paddingWarning?: boolean;
  /** Exact bytes of the decoded buffer, once decoded. */
  decodedBytes?: number;
  estimate?: PreDecodeEstimate;
}

/** Bytes an AudioBuffer costs once decoded: 32-bit float per sample per channel. */
export function decodedBytes(durationSeconds: number, sampleRate: number, channels: number): number {
  return Math.round(durationSeconds * sampleRate * channels * 4);
}

export function bufferBytes(buffer: AudioBuffer): number {
  return buffer.length * buffer.numberOfChannels * 4;
}

/** Binary units, honestly labelled. */
export function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < MiB) return `${(n / 1024).toFixed(1)} KiB`;
  return `${(n / MiB).toFixed(1)} MiB`;
}

export interface ProbeOptions {
  /** Called with the pre-decode estimate; return false to refuse before allocating. */
  gate?: (estimate: PreDecodeEstimate, sniff: SniffResult) => { ok: boolean; detail: string };
}

/**
 * Sniff + ONE real decode. The decoded buffer is returned so the caller can
 * adopt it directly: `decodeAudioData` is the only honest authority on
 * playability, and decoding twice is pure waste.
 *
 * Note on copies: `file.arrayBuffer()` returns a fresh, owned ArrayBuffer and
 * `decodeAudioData` detaches it. The persistence source is the File itself, so
 * the old defensive `bytes.slice(0)` copy is removed — it doubled peak encoded
 * memory for no benefit.
 */
export async function probeFile(
  file: File,
  ctx: BaseAudioContext,
  opts: ProbeOptions = {},
): Promise<ProbeResult & { buffer?: AudioBuffer }> {
  const head = await file.slice(0, 65536).arrayBuffer();
  const sniff = sniffHeader(head);

  if (sniff.malformed) return { verdict: "malformed", sniff, error: sniff.malformed };
  if (file.size === 0) return { verdict: "malformed", sniff, error: "file contains no bytes" };
  if (sniff.container === "unknown") return { verdict: "unsupported", sniff, error: "unrecognised container" };

  const estimate = estimateFromSniff(sniff, file.size, ctx.sampleRate);
  if (opts.gate) {
    const gate = opts.gate(estimate, sniff);
    if (!gate.ok) return { verdict: "unsupported", sniff, estimate, error: gate.detail };
  }

  const started = performance.now();
  try {
    const bytes = await file.arrayBuffer();
    const buffer = await ctx.decodeAudioData(bytes);
    const decodeMs = performance.now() - started;
    const isContract =
      sniff.container === "wav" &&
      sniff.pcm === true &&
      (sniff.bitDepth === 16 || sniff.bitDepth === 24) &&
      (sniff.sampleRate === 44100 || sniff.sampleRate === 48000) &&
      (sniff.channels === 1 || sniff.channels === 2);

    const result: ProbeResult & { buffer?: AudioBuffer } = {
      verdict: isContract ? "contract-supported" : "browser-decode-supported",
      sniff,
      estimate,
      duration: buffer.duration,
      channels: buffer.numberOfChannels,
      decodedSampleRate: buffer.sampleRate,
      decodedBytes: bufferBytes(buffer),
      decodeMs,
      buffer,
    };
    if (sniff.container === "mp3" || sniff.container === "mp4") result.paddingWarning = true;
    return result;
  } catch (err) {
    return {
      verdict: "decode-failed",
      sniff,
      estimate,
      error: err instanceof Error ? err.message : String(err),
    };
  }
}

/** Suggestion only — the user always overrides. */
export function inferRole(filename: string): StemRole | null {
  const n = filename.toLowerCase();
  if (/(vocal|vocals|vox|lead|acapella)/.test(n)) return "vocals";
  if (/(drum|drums|percussion|perc|beat)/.test(n)) return "drums";
  if (/(bass|808|sub)/.test(n)) return "bass";
  if (/(instrument|instrumental|instruments|other|music|melody|keys|synth)/.test(n)) return "instruments";
  return null;
}
