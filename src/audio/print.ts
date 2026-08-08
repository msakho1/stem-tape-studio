/**
 * PRINT commit path (Phase 6 §4.2).
 *
 * A printed heads cycle becomes an ordinary stem: it is encoded to WAV, put
 * through the SAME single-decode ingest path as any user file, persisted as a
 * local blob and written into the session so it survives a reload. Nothing is
 * uploaded — the bytes never leave the tab.
 *
 * Commit order is deliberate: encode → persist+adopt via ingest → only then
 * report success. A failure at any step leaves the target track empty.
 */

import type { AudioEngine, TrackId } from "./engine";
import { ROLE_TRACK, ingestStem } from "./ingest";
import type { StemRole } from "./format";
import { encodeWav } from "./wav";

function roleForTrack(id: TrackId): StemRole | null {
  return (Object.keys(ROLE_TRACK) as StemRole[]).find((r) => ROLE_TRACK[r] === id) ?? null;
}

export function bufferToWav(buffer: AudioBuffer): Blob {
  const channels: Float32Array[] = [];
  for (let c = 0; c < buffer.numberOfChannels; c++) {
    const arr = new Float32Array(buffer.length);
    buffer.copyFromChannel(arr, c);
    channels.push(arr);
  }
  return encodeWav(channels, buffer.sampleRate);
}

export function installPrintCommit(engine: AudioEngine): void {
  engine.commitPrint = async (target: TrackId, buffer: AudioBuffer, detail: string) => {
    const role = roleForTrack(target);
    if (!role) return { ok: false, detail: `track ${target + 1} has no stem role` };
    let file: File;
    try {
      const blob = bufferToWav(buffer);
      file = new File([blob], `print-${role}-${Date.now()}.wav`, { type: "audio/wav" });
    } catch (err) {
      return { ok: false, detail: `WAV encode failed: ${err instanceof Error ? err.message : String(err)}` };
    }
    const res = await ingestStem(engine, role, file, "user-private");
    if (!res.ok) return { ok: false, detail: res.detail };
    void detail;
    return { ok: true, detail: `committed as ${file.name} (${(file.size / 1048576).toFixed(2)} MiB local blob) · ${res.detail}` };
  };
}
