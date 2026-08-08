/**
 * Take page worker (plan §C2, M1).
 *
 * Reads stored chunks and serves fixed-size PAGES to the per-track
 * `TrackTakeMixerProcessor`. Pages are cut across chunk boundaries so a page is
 * always complete; a page that cannot be fully assembled is reported as missing
 * rather than silently zero-filled.
 */

import { chunkStore, unpackChunk } from "@/audio/input/chunkStore";

interface ChunkRef {
  index: number;
  blobKey: string;
  startFrame: number;
  frames: number;
}

interface Layer {
  layerId: string;
  channels: number;
  chunks: ChunkRef[];
}

const layers = new Map<string, Layer>();
const chunkCache = new Map<string, Float32Array[]>();
const CHUNK_CACHE_MAX = 6;

function post(msg: unknown, transfer?: Transferable[]) {
  if (transfer) (self as unknown as Worker).postMessage(msg, transfer);
  else (self as unknown as Worker).postMessage(msg);
}

async function loadChunk(layer: Layer, ref: ChunkRef): Promise<Float32Array[] | null> {
  const cached = chunkCache.get(ref.blobKey);
  if (cached) return cached;
  const buf = await chunkStore.get(ref.blobKey);
  if (!buf) return null;
  const planar = unpackChunk(buf, layer.channels);
  chunkCache.set(ref.blobKey, planar);
  if (chunkCache.size > CHUNK_CACHE_MAX) chunkCache.delete(chunkCache.keys().next().value as string);
  return planar;
}

async function buildPage(layerId: string, pageIndex: number, pageFrames: number) {
  const layer = layers.get(layerId);
  if (!layer) return post({ type: "pageMissing", layerId, pageIndex, reason: "unknown layer" });
  const start = pageIndex * pageFrames;
  const out: Float32Array[] = [];
  for (let c = 0; c < layer.channels; c++) out.push(new Float32Array(pageFrames));
  let filled = 0;
  for (const ref of layer.chunks) {
    const cEnd = ref.startFrame + ref.frames;
    if (cEnd <= start || ref.startFrame >= start + pageFrames) continue;
    const planar = await loadChunk(layer, ref);
    if (!planar) return post({ type: "pageMissing", layerId, pageIndex, reason: `chunk ${ref.index} unreadable` });
    const from = Math.max(start, ref.startFrame);
    const to = Math.min(start + pageFrames, cEnd);
    const n = to - from;
    for (let c = 0; c < layer.channels; c++) {
      out[c]!.set(planar[Math.min(c, planar.length - 1)]!.subarray(from - ref.startFrame, to - ref.startFrame), from - start);
    }
    filled += n;
  }
  post(
    { type: "page", layerId, pageIndex, frames: filled, channels: out.map((o) => o.buffer) },
    out.map((o) => o.buffer),
  );
}

self.onmessage = async (e: MessageEvent) => {
  const m = e.data as { type: string; [k: string]: unknown };
  switch (m.type) {
    case "registerLayer":
      layers.set(m["layerId"] as string, {
        layerId: m["layerId"] as string,
        channels: m["channels"] as number,
        chunks: m["chunks"] as ChunkRef[],
      });
      post({ type: "layerRegistered", layerId: m["layerId"], chunks: (m["chunks"] as ChunkRef[]).length });
      return;
    case "dropLayer":
      layers.delete(m["layerId"] as string);
      post({ type: "layerDropped", layerId: m["layerId"] });
      return;
    case "requestPage":
      await buildPage(m["layerId"] as string, m["pageIndex"] as number, m["pageFrames"] as number);
      return;
    case "clearCache":
      chunkCache.clear();
      post({ type: "cacheCleared" });
      return;
  }
};
