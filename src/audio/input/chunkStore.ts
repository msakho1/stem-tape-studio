/**
 * Take chunk storage — the same OPFS-preferred / IndexedDB-fallback façade as
 * `store.ts`, but usable from a Worker (no DOM, no main-thread assumptions).
 *
 * Audio never leaves the device: these are local writes only.
 */

const DB_NAME = "stem-tape";
const DB_VERSION = 1;
const STORE_BLOBS = "blobs";
const DIR = "stem-tape-audio";

async function opfsDir(): Promise<FileSystemDirectoryHandle | null> {
  try {
    const st = navigator.storage as StorageManager & { getDirectory?: () => Promise<FileSystemDirectoryHandle> };
    if (!st?.getDirectory) return null;
    const root = await st.getDirectory();
    return await root.getDirectoryHandle(DIR, { create: true });
  } catch {
    return null;
  }
}

function idb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, DB_VERSION);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains("projects")) db.createObjectStore("projects", { keyPath: "id" });
      if (!db.objectStoreNames.contains(STORE_BLOBS)) db.createObjectStore(STORE_BLOBS);
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error ?? new Error("IndexedDB open failed"));
  });
}

export const chunkStore = {
  async backend(): Promise<"opfs" | "indexeddb"> {
    return (await opfsDir()) ? "opfs" : "indexeddb";
  },

  async put(key: string, bytes: ArrayBuffer): Promise<"opfs" | "indexeddb"> {
    const dir = await opfsDir();
    if (dir) {
      const handle = await dir.getFileHandle(key, { create: true });
      const w = await (handle as FileSystemFileHandle & { createWritable: () => Promise<FileSystemWritableFileStream> }).createWritable();
      await w.write(bytes);
      await w.close();
      return "opfs";
    }
    const db = await idb();
    await new Promise<void>((res, rej) => {
      const tx = db.transaction(STORE_BLOBS, "readwrite");
      tx.objectStore(STORE_BLOBS).put(new Blob([bytes]), key);
      tx.oncomplete = () => res();
      tx.onerror = () => rej(tx.error);
    });
    db.close();
    return "indexeddb";
  },

  async get(key: string): Promise<ArrayBuffer | null> {
    const dir = await opfsDir();
    if (dir) {
      try {
        const h = await dir.getFileHandle(key);
        return await (await h.getFile()).arrayBuffer();
      } catch {
        /* fall through */
      }
    }
    try {
      const db = await idb();
      const blob = await new Promise<Blob | undefined>((res, rej) => {
        const tx = db.transaction(STORE_BLOBS, "readonly");
        const r = tx.objectStore(STORE_BLOBS).get(key) as IDBRequest<Blob | undefined>;
        r.onsuccess = () => res(r.result);
        r.onerror = () => rej(r.error);
      });
      db.close();
      return blob ? await blob.arrayBuffer() : null;
    } catch {
      return null;
    }
  },

  async remove(key: string): Promise<void> {
    const dir = await opfsDir();
    if (dir) {
      try {
        await dir.removeEntry(key);
      } catch {
        /* absent */
      }
    }
    try {
      const db = await idb();
      await new Promise<void>((res) => {
        const tx = db.transaction(STORE_BLOBS, "readwrite");
        tx.objectStore(STORE_BLOBS).delete(key);
        tx.oncomplete = () => res();
        tx.onerror = () => res();
      });
      db.close();
    } catch {
      /* absent */
    }
  },
};

/** Deterministic chunk key: one take never collides with another. */
export function chunkKey(takeId: string, index: number): string {
  return `take-${takeId}-chunk-${index.toString().padStart(5, "0")}.f32`;
}

/** Float32 planar → one interleaved ArrayBuffer for storage. */
export function packChunk(channels: Float32Array[], frames: number): ArrayBuffer {
  const ch = channels.length;
  const out = new Float32Array(frames * ch);
  for (let i = 0; i < frames; i++) for (let c = 0; c < ch; c++) out[i * ch + c] = channels[c]![i] ?? 0;
  return out.buffer;
}

/** Interleaved storage bytes → planar Float32Arrays. */
export function unpackChunk(buf: ArrayBuffer, channels: number): Float32Array[] {
  const src = new Float32Array(buf);
  const frames = Math.floor(src.length / channels);
  const out: Float32Array[] = [];
  for (let c = 0; c < channels; c++) out.push(new Float32Array(frames));
  for (let i = 0; i < frames; i++) for (let c = 0; c < channels; c++) out[c]![i] = src[i * channels + c]!;
  return out;
}
