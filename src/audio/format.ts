/**
 * Format validation.
 *
 * Rule from the approved plan: never trust MIME type or filename. Mobile file
 * pickers hand back empty or wrong MIME strings routinely. Every file is
 * sniffed from its header bytes and then probed with a REAL decode before the
 * app claims it can play it.
 */

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
  /** WAV only: bytes in the data chunk, from the chunk header. */
  dataBytes?: number;
  /** WAV only: EXACT frame count = dataBytes / (channels * bitDepth/8). */
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
    // Walk chunks for `fmt `.
    let off = 12;
    while (off + 8 <= v.byteLength) {
      const id = ascii(v, off, 4);
      const size = v.getUint32(off + 4, true);
      if (id === "fmt " && off + 8 + 16 <= v.byteLength) {
        const tag = v.getUint16(off + 8, true);
        return {
          container: "wav",
          pcm: tag === 1 || tag === 3 || tag === 0xfffe,
          channels: v.getUint16(off + 10, true),
          sampleRate: v.getUint32(off + 12, true),
          bitDepth: v.getUint16(off + 22, true),
        };
      }
      off += 8 + size + (size % 2);
    }
    return { container: "wav", malformed: "RIFF/WAVE with no readable fmt chunk" };
  }

  if (magic === "fLaC") return { container: "flac" };
  if (magic === "FORM" && (ascii(v, 8, 4) === "AIFF" || ascii(v, 8, 4) === "AIFC")) return { container: "aiff" };
  if (ascii(v, 4, 4) === "ftyp") return { container: "mp4" };
  if (ascii(v, 0, 3) === "ID3") return { container: "mp3" };
  // Bare MPEG frame sync.
  if (v.getUint8(0) === 0xff && (v.getUint8(1) & 0xe0) === 0xe0) return { container: "mp3" };

  return { container: "unknown", malformed: "no recognised audio container header" };
}

export interface ProbeResult {
  verdict: FormatVerdict;
  sniff: SniffResult;
  /** Populated when the decode probe succeeded. */
  duration?: number;
  channels?: number;
  decodedSampleRate?: number;
  decodeMs?: number;
  error?: string;
  /** Compressed sources carry encoder padding — alignment caveat. */
  paddingWarning?: boolean;
}

/** Bytes an AudioBuffer costs once decoded: 32-bit float per sample per channel. */
export function decodedBytes(durationSeconds: number, sampleRate: number, channels: number): number {
  return Math.round(durationSeconds * sampleRate * channels * 4);
}

export function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} kB`;
  return `${(n / (1024 * 1024)).toFixed(1)} MB`;
}

/**
 * Real decode probe. `decodeAudioData` is the only honest authority on whether
 * this browser can play these bytes — extension and MIME are not.
 */
export async function probeFile(file: File, ctx: BaseAudioContext): Promise<ProbeResult> {
  const head = await file.slice(0, 4096).arrayBuffer();
  const sniff = sniffHeader(head);

  if (sniff.malformed) return { verdict: "malformed", sniff, error: sniff.malformed };
  if (file.size === 0) return { verdict: "malformed", sniff, error: "file contains no bytes" };
  if (sniff.container === "unknown") return { verdict: "unsupported", sniff, error: "unrecognised container" };

  const started = performance.now();
  try {
    const bytes = await file.arrayBuffer();
    const buffer = await ctx.decodeAudioData(bytes.slice(0));
    const decodeMs = performance.now() - started;
    const isContract =
      sniff.container === "wav" &&
      sniff.pcm === true &&
      (sniff.bitDepth === 16 || sniff.bitDepth === 24) &&
      (sniff.sampleRate === 44100 || sniff.sampleRate === 48000) &&
      (sniff.channels === 1 || sniff.channels === 2);

    const result: ProbeResult = {
      verdict: isContract ? "contract-supported" : "browser-decode-supported",
      sniff,
      duration: buffer.duration,
      channels: buffer.numberOfChannels,
      decodedSampleRate: buffer.sampleRate,
      decodeMs,
    };
    if (sniff.container === "mp3" || sniff.container === "mp4") result.paddingWarning = true;
    return result;
  } catch (err) {
    return {
      verdict: "decode-failed",
      sniff,
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
