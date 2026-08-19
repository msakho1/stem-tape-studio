/**
 * Chunked, cacheable song preparation.
 *
 * Preparation happens BEFORE the upload clock starts, so it is allowed to be
 * slow — but it may not be wasteful. Instead of materialising one monolithic
 * encoded image (a real four-stem song is ~248 MiB) the canonical song is
 * encoded into logical 8 KiB sectors incrementally, grouped into chunk
 * records, and persisted with its manifest.
 *
 * The cache key is a deterministic fingerprint of the SOURCE FILES plus the
 * timing metadata that is baked into every sector header. Change any file or
 * any timing value and the fingerprint changes, so stale prepared data can
 * never be reused for a different song.
 *
 * Storage is injectable: the browser uses IndexedDB, tests use memory.
 */

import { crc32 } from "./crc32";
import { encodeSector } from "./sector";
import { sectorsForFrames } from "./stemTapeFormat";
import { STEM_ORDER, type StemSlotName } from "./prepare";
import type { CanonicalSong } from "./song";
import type { SongTiming } from "./autoTiming";

/** Sectors per stored chunk record (64 × 8 KiB = 512 KiB). */
export const SECTORS_PER_CHUNK = 64;

export interface SourceFingerprintInput {
  files: Partial<Record<StemSlotName, { name: string; size: number; lastModified: number }>>;
  timing: Pick<SongTiming, "bpm" | "downbeatSeconds">;
  title: string;
}

/** Deterministic, collision-resistant enough for a local cache key. */
export function fingerprintSources(input: SourceFingerprintInput): string {
  const parts: string[] = [
    `t=${input.title}`,
    `bpm=${input.timing.bpm.toFixed(4)}`,
    `db=${input.timing.downbeatSeconds.toFixed(6)}`,
  ];
  for (const role of STEM_ORDER) {
    const f = input.files[role];
    parts.push(f ? `${role}:${f.name}:${f.size}:${f.lastModified}` : `${role}:-`);
  }
  const text = parts.join("|");
  const bytes = new TextEncoder().encode(text);
  const a = crc32(bytes).toString(16).padStart(8, "0");
  const b = crc32(bytes.slice().reverse()).toString(16).padStart(8, "0");
  return `${a}${b}-${bytes.length.toString(16)}`;
}

export interface PreparedManifest {
  fingerprint: string;
  title: string;
  frames: number;
  durationSeconds: number;
  sectorCount: number;
  chunkCount: number;
  audioBytes: number;
  totalBytes: number;
  /** CRC-32 of every logical sector, in ascending sector order. */
  sectorCrc: number[];
  bpm: number;
  downbeatSeconds: number;
  stems: {
    name: StemSlotName;
    filename: string;
    padFrames: number;
    peak: number;
    clipped: boolean;
  }[];
  preparedAt: number;
}

export interface PrepStorage {
  getManifest(fingerprint: string): Promise<PreparedManifest | null>;
  putManifest(m: PreparedManifest): Promise<void>;
  putChunk(fingerprint: string, index: number, bytes: Uint8Array): Promise<void>;
  getChunk(fingerprint: string, index: number): Promise<Uint8Array | null>;
  /** Drop everything that is not the given fingerprint. */
  prune(keep: string): Promise<void>;
}

export function memoryStorage(): PrepStorage {
  const manifests = new Map<string, PreparedManifest>();
  const chunks = new Map<string, Uint8Array>();
  return {
    async getManifest(f) {
      return manifests.get(f) ?? null;
    },
    async putManifest(m) {
      manifests.set(m.fingerprint, m);
    },
    async putChunk(f, i, bytes) {
      chunks.set(`${f}#${i}`, bytes);
    },
    async getChunk(f, i) {
      return chunks.get(`${f}#${i}`) ?? null;
    },
    async prune(keep) {
      for (const k of [...manifests.keys()]) if (k !== keep) manifests.delete(k);
      for (const k of [...chunks.keys()]) if (!k.startsWith(`${keep}#`)) chunks.delete(k);
    },
  };
}

const DB_NAME = "stem-tape-prep";
const DB_VERSION = 1;

function idbOpen(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, DB_VERSION);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains("manifests")) db.createObjectStore("manifests");
      if (!db.objectStoreNames.contains("chunks")) db.createObjectStore("chunks");
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

function idbRun<T>(
  db: IDBDatabase,
  store: string,
  mode: IDBTransactionMode,
  fn: (s: IDBObjectStore) => IDBRequest,
): Promise<T> {
  return new Promise((resolve, reject) => {
    const tx = db.transaction(store, mode);
    const req = fn(tx.objectStore(store));
    req.onsuccess = () => resolve(req.result as T);
    req.onerror = () => reject(req.error);
  });
}

/** IndexedDB-backed store; falls back to memory when IndexedDB is unavailable. */
export function browserStorage(): PrepStorage {
  if (typeof indexedDB === "undefined") return memoryStorage();
  let dbp: Promise<IDBDatabase> | null = null;
  const db = () => (dbp ??= idbOpen());
  return {
    async getManifest(f) {
      return (
        (await idbRun<PreparedManifest | undefined>(await db(), "manifests", "readonly", (s) =>
          s.get(f),
        )) ?? null
      );
    },
    async putManifest(m) {
      await idbRun(await db(), "manifests", "readwrite", (s) => s.put(m, m.fingerprint));
    },
    async putChunk(f, i, bytes) {
      await idbRun(await db(), "chunks", "readwrite", (s) => s.put(bytes, `${f}#${i}`));
    },
    async getChunk(f, i) {
      const v = await idbRun<Uint8Array | undefined>(await db(), "chunks", "readonly", (s) =>
        s.get(`${f}#${i}`),
      );
      return v ? new Uint8Array(v) : null;
    },
    async prune(keep) {
      const d = await db();
      const mk = await idbRun<IDBValidKey[]>(d, "manifests", "readonly", (s) => s.getAllKeys());
      for (const k of mk)
        if (k !== keep) await idbRun(d, "manifests", "readwrite", (s) => s.delete(k));
      const ck = await idbRun<IDBValidKey[]>(d, "chunks", "readonly", (s) => s.getAllKeys());
      for (const k of ck)
        if (!String(k).startsWith(`${keep}#`))
          await idbRun(d, "chunks", "readwrite", (s) => s.delete(k));
    },
  };
}

export interface PrepareChunkedOptions {
  song: CanonicalSong;
  fingerprint: string;
  storage: PrepStorage;
  onProgress?: (fraction: number) => void;
  signal?: { aborted: boolean };
}

/**
 * Encode the canonical song into logical sectors incrementally and persist
 * them chunk by chunk. Only one chunk (512 KiB) of encoded bytes is held at a
 * time; the whole encoded image is never assembled in memory.
 */
export async function prepareChunked(opts: PrepareChunkedOptions): Promise<PreparedManifest> {
  const { song, fingerprint, storage } = opts;
  const sectorCount = sectorsForFrames(song.frames);
  const sectorCrc: number[] = new Array(sectorCount);
  let chunkIndex = 0;
  let written = 0;

  for (let base = 0; base < sectorCount; base += SECTORS_PER_CHUNK) {
    if (opts.signal?.aborted) throw new Error("preparation cancelled");
    const count = Math.min(SECTORS_PER_CHUNK, sectorCount - base);
    const chunk = new Uint8Array(count * 8192);
    for (let i = 0; i < count; i++) {
      const sector = encodeSector(song, base + i);
      sectorCrc[base + i] = crc32(sector);
      chunk.set(sector, i * 8192);
    }
    await storage.putChunk(fingerprint, chunkIndex, chunk);
    chunkIndex++;
    written += count;
    opts.onProgress?.(written / sectorCount);
    // Yield so the interface stays responsive during a long preparation.
    await new Promise((r) => setTimeout(r, 0));
  }

  const manifest: PreparedManifest = {
    fingerprint,
    title: song.metadata.title,
    frames: song.frames,
    durationSeconds: song.durationSeconds,
    sectorCount,
    chunkCount: chunkIndex,
    audioBytes: song.audioBytes,
    totalBytes: sectorCount * 8192,
    sectorCrc,
    bpm: song.metadata.bpm,
    downbeatSeconds: song.metadata.downbeatSeconds,
    stems: song.stems.map((s) => ({
      name: s.name,
      filename: s.filename,
      padFrames: s.padFrames,
      peak: s.peak,
      clipped: s.clipped,
    })),
    preparedAt: Date.now(),
  };
  await storage.putManifest(manifest);
  await storage.prune(fingerprint);
  return manifest;
}

/** Reuse prepared data only when the fingerprint matches exactly. */
export async function loadPrepared(
  storage: PrepStorage,
  fingerprint: string,
): Promise<PreparedManifest | null> {
  const m = await storage.getManifest(fingerprint);
  if (!m || m.fingerprint !== fingerprint) return null;
  // A manifest without its first chunk is useless; treat it as a miss.
  if (m.chunkCount > 0 && !(await storage.getChunk(fingerprint, 0))) return null;
  return m;
}
