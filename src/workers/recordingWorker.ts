/**
 * Recording worker (plan §C1, M2).
 *
 * Owns bytes. Receives pooled 4096-frame transferable blocks straight from the
 * capture worklet (direct MessagePort) or relayed through the main thread when
 * the direct port is unavailable, aggregates them into ~2 s storage chunks, and
 * writes them locally. It never sees the audio graph and never uploads.
 */

import { chunkKey, chunkStore, packChunk } from "@/audio/input/chunkStore";

interface StartMsg {
  type: "start";
  takeId: string;
  channels: number;
  sampleRate: number;
  chunkFrames: number;
  relay: boolean;
}

let takeId = "";
let channels = 1;
let chunkFrames = 96000;
let acc: Float32Array[] = [];
let accFill = 0;
let chunkIndex = 0;
let framesWritten = 0;
let pendingWrites = 0;
let failed: string | null = null;
let workletPort: MessagePort | null = null;
let backend: "opfs" | "indexeddb" = "indexeddb";

function post(msg: unknown, transfer?: Transferable[]) {
  if (transfer) (self as unknown as Worker).postMessage(msg, transfer);
  else (self as unknown as Worker).postMessage(msg);
}

function resetAcc() {
  acc = [];
  for (let c = 0; c < channels; c++) acc.push(new Float32Array(chunkFrames));
  accFill = 0;
}

async function flush(final: boolean) {
  if (accFill === 0 && !final) return;
  if (accFill === 0) return;
  const frames = accFill;
  const index = chunkIndex++;
  const startFrame = framesWritten - frames;
  const bytes = packChunk(acc, frames);
  resetAcc();
  pendingWrites++;
  try {
    backend = await chunkStore.put(chunkKey(takeId, index), bytes);
    post({ type: "chunk", index, startFrame, frames, bytes: bytes.byteLength, blobKey: chunkKey(takeId, index), backend });
  } catch (e) {
    failed = `chunk ${index} write failed: ${(e as Error).message}`;
    post({ type: "failed", reason: failed, committedChunks: index, framesWritten: startFrame });
  } finally {
    pendingWrites--;
  }
}

function recycle(index: number, buffers: ArrayBuffer[]) {
  const target = workletPort ?? (self as unknown as Worker);
  const msg = { type: "recycle", index, channels: buffers };
  if (workletPort) workletPort.postMessage(msg, buffers);
  else (target as Worker).postMessage(msg, buffers);
}

async function onBlock(m: { index: number; frames: number; channels: ArrayBuffer[]; final?: boolean }) {
  if (failed) return;
  const views = m.channels.map((b) => new Float32Array(b));
  let read = 0;
  while (read < m.frames) {
    const room = chunkFrames - accFill;
    const n = Math.min(room, m.frames - read);
    for (let c = 0; c < channels; c++) {
      const src = views[Math.min(c, views.length - 1)]!.subarray(read, read + n);
      acc[c]!.set(src, accFill);
    }
    accFill += n;
    read += n;
    framesWritten += n;
    if (accFill >= chunkFrames) await flush(false);
  }
  // Return the pooled buffers so process() never allocates.
  recycle(m.index, m.channels);
  if (m.final) {
    await flush(true);
    post({ type: "finalized", takeId, frames: framesWritten, chunks: chunkIndex, backend });
  }
}

self.onmessage = async (e: MessageEvent) => {
  const m = e.data as StartMsg | { type: string; [k: string]: unknown };
  switch (m.type) {
    case "start": {
      const s = m as StartMsg;
      takeId = s.takeId;
      channels = Math.max(1, s.channels);
      chunkFrames = s.chunkFrames || Math.round(2 * s.sampleRate);
      chunkIndex = 0;
      framesWritten = 0;
      failed = null;
      resetAcc();
      post({ type: "started", takeId, chunkFrames, channels, relay: (m as StartMsg).relay });
      return;
    }
    case "attachWorklet": {
      workletPort = (m as unknown as { port: MessagePort }).port;
      workletPort.onmessage = (ev) => {
        const d = ev.data as { type: string };
        if (d.type === "block") void onBlock(ev.data as Parameters<typeof onBlock>[0]);
      };
      post({ type: "attached", direct: true });
      return;
    }
    case "block":
      await onBlock(m as unknown as Parameters<typeof onBlock>[0]);
      return;
    case "finalize":
      await flush(true);
      post({ type: "finalized", takeId, frames: framesWritten, chunks: chunkIndex, backend });
      return;
    case "abort":
      await flush(true);
      post({ type: "aborted", takeId, frames: framesWritten, chunks: chunkIndex, reason: m["reason"] ?? "aborted", backend });
      return;
    case "status":
      post({ type: "status", takeId, framesWritten, chunkIndex, pendingWrites, failed, backend });
      return;
  }
};
