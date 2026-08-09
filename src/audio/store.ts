/**
 * Local-only project storage.
 *
 * Audio never leaves the browser in Phase 4. Project JSON lives in IndexedDB;
 * audio blobs prefer OPFS and fall back to IndexedDB — both behind ONE
 * interface so the audio engine never learns which backend won.
 */

import type { StemRole } from "./format";

export const SCHEMA_VERSION = 2;
export const DERIVED_SCHEMA_VERSION = 1;
export const DB_NAME = "stem-tape";
export const DB_VERSION = 1;
const STORE_PROJECTS = "projects";
const STORE_BLOBS = "blobs";

/**
 * A Memory Saver working copy. The ORIGINAL blob is never replaced: this is a
 * separate, separately-keyed derived asset that can be deleted and regenerated.
 */
export interface DerivedAsset {
  derivedKey: string;
  sourceBlobKey: string;
  sourceContentHash: string;
  conversion: { type: "mono-downmix"; sampleRate: number };
  derivedSchemaVersion: number;
}

export interface StoredStem {
  role: StemRole;
  filename: string;
  /** Container as sniffed, NOT as claimed by MIME. */
  container: string;
  extension: string;
  mimeType: string;
  bytes: number;
  duration: number;
  channels: number;
  sourceSampleRate: number;
  decodedSampleRate: number;
  /** Stable content id (SHA-256 of the bytes). */
  contentHash: string;
  /** Key of the ORIGINAL, untouched blob. */
  blobKey: string;
  provenance: "user-private" | "bundled-demo";
  /** In the recoverable trash: unloaded from the surface, bytes retained. */
  trashed?: boolean;
  /** Memory Saver working copy, when one has been generated. */
  derived?: DerivedAsset | null;
}

export interface StoredProject {
  id: string;
  schemaVersion: number;
  name: string;
  createdAt: number;
  updatedAt: number;
  stems: StoredStem[];
  control: {
    faders: number[];
    mutes: boolean[];
    masterVolume: number;
    speed: number;
    chopDiv: number;
    window: { start: number; end: number; shift: number; reverse: boolean };
    filter: { mode: string; amount: number };
    grid: { bpm: number | null; source: string };
    /**
     * Automatic song grid, persisted TIME-FIRST (seconds). Frames and sample
     * rate are cross-checks only: stems can differ in length and encoder
     * padding, so restore converts round(seconds × decodedContextSampleRate).
     */
    songGrid?: {
      bpm: number;
      beatsPerBar: number;
      firstBeatS: number;
      firstDownbeatS: number;
      beatSeconds: number;
      barSeconds: number;
      analysisSampleRate: number;
      analysisFrames: number;
      durationS: number;
      segments: { startS: number; bpm: number }[];
      normalizedDownbeat: number;
      source: string;
      sourceHashes: string[];
    } | null;
    song: number;
  };
  blobBackend: "opfs" | "indexeddb";
  highMemoryMode?: boolean;
}


function openDb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, DB_VERSION);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains(STORE_PROJECTS)) db.createObjectStore(STORE_PROJECTS, { keyPath: "id" });
      if (!db.objectStoreNames.contains(STORE_BLOBS)) db.createObjectStore(STORE_BLOBS);
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error ?? new Error("IndexedDB open failed"));
  });
}

function tx<T>(store: string, mode: IDBTransactionMode, run: (s: IDBObjectStore) => IDBRequest<T>): Promise<T> {
  return openDb().then(
    (db) =>
      new Promise<T>((resolve, reject) => {
        const t = db.transaction(store, mode);
        const req = run(t.objectStore(store));
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error ?? new Error("IndexedDB request failed"));
        t.oncomplete = () => db.close();
      }),
  );
}

async function opfsRoot(): Promise<FileSystemDirectoryHandle | null> {
  try {
    const nav = navigator as Navigator & { storage?: StorageManager & { getDirectory?: () => Promise<FileSystemDirectoryHandle> } };
    if (!nav.storage?.getDirectory) return null;
    const root = await nav.storage.getDirectory();
    return await root.getDirectoryHandle("stem-tape-audio", { create: true });
  } catch {
    return null;
  }
}

export interface StorageReport {
  backend: "opfs" | "indexeddb";
  usage: number | null;
  quota: number | null;
  persisted: boolean;
}

export const projectStore = {
  async blobBackend(): Promise<"opfs" | "indexeddb"> {
    return (await opfsRoot()) ? "opfs" : "indexeddb";
  },

  async putBlob(key: string, blob: Blob): Promise<"opfs" | "indexeddb"> {
    const dir = await opfsRoot();
    if (dir) {
      const handle = await dir.getFileHandle(key, { create: true });
      const w = await (handle as FileSystemFileHandle & { createWritable: () => Promise<FileSystemWritableFileStream> }).createWritable();
      await w.write(blob);
      await w.close();
      return "opfs";
    }
    await tx(STORE_BLOBS, "readwrite", (s) => s.put(blob, key) as IDBRequest<IDBValidKey>);
    return "indexeddb";
  },

  async getBlob(key: string): Promise<Blob | null> {
    const dir = await opfsRoot();
    if (dir) {
      try {
        const handle = await dir.getFileHandle(key);
        return await handle.getFile();
      } catch {
        /* fall through to IndexedDB — a project may predate OPFS support */
      }
    }
    try {
      return (await tx<Blob | undefined>(STORE_BLOBS, "readonly", (s) => s.get(key) as IDBRequest<Blob | undefined>)) ?? null;
    } catch {
      return null;
    }
  },

  async deleteBlob(key: string): Promise<void> {
    const dir = await opfsRoot();
    if (dir) {
      try {
        await dir.removeEntry(key);
      } catch {
        /* not there */
      }
    }
    try {
      await tx(STORE_BLOBS, "readwrite", (s) => s.delete(key) as unknown as IDBRequest<undefined>);
    } catch {
      /* not there */
    }
  },

  async saveProject(project: StoredProject): Promise<void> {
    await tx(STORE_PROJECTS, "readwrite", (s) => s.put(project) as IDBRequest<IDBValidKey>);
  },

  async listProjects(): Promise<StoredProject[]> {
    try {
      const all = await tx<StoredProject[]>(STORE_PROJECTS, "readonly", (s) => s.getAll() as IDBRequest<StoredProject[]>);
      return all.sort((a, b) => b.updatedAt - a.updatedAt);
    } catch {
      return [];
    }
  },

  async getProject(id: string): Promise<StoredProject | null> {
    try {
      return (await tx<StoredProject | undefined>(STORE_PROJECTS, "readonly", (s) => s.get(id) as IDBRequest<StoredProject | undefined>)) ?? null;
    } catch {
      return null;
    }
  },

  /** Deletes the record and its blobs. Never called implicitly. */
  async deleteProject(id: string): Promise<void> {
    const project = await this.getProject(id);
    for (const stem of project?.stems ?? []) await this.deleteBlob(stem.blobKey);
    await tx(STORE_PROJECTS, "readwrite", (s) => s.delete(id) as unknown as IDBRequest<undefined>);
  },

  async report(): Promise<StorageReport> {
    const backend = await this.blobBackend();
    let usage: number | null = null;
    let quota: number | null = null;
    let persisted = false;
    try {
      const est = await navigator.storage?.estimate?.();
      usage = est?.usage ?? null;
      quota = est?.quota ?? null;
      persisted = (await navigator.storage?.persisted?.()) ?? false;
    } catch {
      /* estimate unavailable */
    }
    return { backend, usage, quota, persisted };
  },

  async requestPersistence(): Promise<boolean> {
    try {
      return (await navigator.storage?.persist?.()) ?? false;
    } catch {
      return false;
    }
  },
};

export async function contentHash(blob: Blob): Promise<string> {
  try {
    const bytes = await blob.arrayBuffer();
    const digest = await crypto.subtle.digest("SHA-256", bytes);
    return Array.from(new Uint8Array(digest).slice(0, 16))
      .map((b) => b.toString(16).padStart(2, "0"))
      .join("");
  } catch {
    return `size-${blob.size}`;
  }
}
