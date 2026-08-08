/**
 * WAV export worker (plan §L, M12).
 *
 * Streams stored take chunks through the pure encoder and assembles the file
 * without ever holding a second full copy of the project PCM on the main
 * thread. Everything stays local.
 */

import { chunkStore, unpackChunk } from "@/audio/input/chunkStore";
import { encodeChunk, wavHeader, type BitDepth } from "@/audio/export/wavStream";

interface ExportMsg {
  type: "export";
  chunks: { blobKey: string; frames: number }[];
  frames: number;
  channels: number;
  sampleRate: number;
  bitDepth: BitDepth;
  filename: string;
}

self.onmessage = async (e: MessageEvent) => {
  const m = e.data as ExportMsg | { type: string };
  if (m.type !== "export") return;
  const spec = m as ExportMsg;
  const parts: BlobPart[] = [wavHeader({ sampleRate: spec.sampleRate, channels: spec.channels, bitDepth: spec.bitDepth, frames: spec.frames }) as BlobPart];
  let written = 0;
  for (const ref of spec.chunks) {
    const buf = await chunkStore.get(ref.blobKey);
    if (!buf) {
      (self as unknown as Worker).postMessage({ type: "failed", reason: `chunk ${ref.blobKey} missing — export aborted rather than silently short` });
      return;
    }
    const planar = unpackChunk(buf, spec.channels);
    const frames = Math.min(ref.frames, planar[0]!.length);
    parts.push(encodeChunk(planar, frames, spec.bitDepth) as BlobPart);
    written += frames;
    (self as unknown as Worker).postMessage({ type: "progress", written, total: spec.frames });
  }
  if (written !== spec.frames) {
    (self as unknown as Worker).postMessage({ type: "failed", reason: `header declares ${spec.frames} frames, payload holds ${written}` });
    return;
  }
  const blob = new Blob(parts, { type: "audio/wav" });
  (self as unknown as Worker).postMessage({ type: "done", blob, bytes: blob.size, frames: written, filename: spec.filename });
};
