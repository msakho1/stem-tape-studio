/**
 * Session project state — the bridge between the project drawer and the
 * instrument. Module-level (not React context) because the AudioEngine is a
 * singleton and client-side route changes must not drop a loaded song.
 *
 * Nothing here touches the network. User audio stays in memory / IndexedDB /
 * OPFS; only the procedurally generated demo is ever labelled `bundled-demo`.
 */

import type { PreDecodeEstimate, ProbeResult, StemRole } from "./format";
import type { SongGrid } from "./gridAnalysis";
import { DERIVED_SCHEMA_VERSION, type StoredProject } from "./store";

export interface DerivedCopy {
  kind: "mono-downmix";
  /** Key of the derived working blob (never replaces the original). */
  derivedKey?: string;
  sourceBlobKey?: string;
  sourceContentHash?: string;
  conversion?: { type: "mono-downmix"; sampleRate: number };
  derivedSchemaVersion?: number;
}

export interface SessionStem {
  role: StemRole;
  filename: string;
  blob: Blob;
  probe: ProbeResult;
  provenance: "user-private" | "bundled-demo";
  /** Exact retained decoded bytes (from the adopted AudioBuffer). */
  decodedBytes: number;
  /** Encoded bytes of the original file. */
  fileBytes: number;
  estimate: PreDecodeEstimate | null;
  decodeCount: number;
  decodeMs: number | null;
  loaded: boolean;
  /** Deleted from the surface but recoverable until the project is compacted. */
  trashed: boolean;
  blobKey: string;
  contentHash: string;
  /** Present when a Memory Saver working copy is in use. */
  derived: DerivedCopy | null;
}

export interface SessionState {
  projectId: string;
  name: string;
  stems: Partial<Record<StemRole, SessionStem>>;
  saved: boolean;
  savedAt: number | null;
  source: "none" | "demo" | "upload" | "restored";
  /** Hybrid BPM model: tempo-grid ?? manual ?? provisional 120. */
  bpm: number;
  bpmSource: BpmSource;
  highMemoryMode: boolean;
  lastError: string | null;
  /** Automatic analysed grid for this song, persisted in seconds. */
  songGrid: SongGrid | null;
}

export type BpmSource = "manual" | "tempo-grid" | "provisional";

export const PROVISIONAL_BPM = 120;

export function resolveBpm(tempoGridBpm: number | null, manualBpm: number | null): { bpm: number; source: BpmSource } {
  if (tempoGridBpm != null && tempoGridBpm > 0) return { bpm: tempoGridBpm, source: "tempo-grid" };
  if (manualBpm != null && manualBpm > 0) return { bpm: manualBpm, source: "manual" };
  return { bpm: PROVISIONAL_BPM, source: "provisional" };
}


function emptySession(): SessionState {
  return {
    projectId: `proj-${Math.random().toString(36).slice(2, 10)}`,
    name: "untitled session",
    stems: {},
    saved: false,
    savedAt: null,
    source: "none",
    bpm: PROVISIONAL_BPM,
    bpmSource: "provisional",
    highMemoryMode: false,
    lastError: null,
    songGrid: null,
  };
}


type Listener = (s: SessionState) => void;

let state: SessionState = emptySession();
const listeners = new Set<Listener>();

export const session = {
  get(): SessionState {
    return state;
  },
  subscribe(fn: Listener) {
    listeners.add(fn);
    return () => listeners.delete(fn);
  },
  set(patch: Partial<SessionState>) {
    state = { ...state, ...patch };
    for (const l of listeners) l(state);
  },
  setStem(role: StemRole, stem: SessionStem | null) {
    const stems = { ...state.stems };
    if (stem) stems[role] = stem;
    else delete stems[role];
    this.set({ stems, saved: false });
  },
  reset() {
    state = emptySession();
    for (const l of listeners) l(state);
  },
};

export function toStoredProject(s: SessionState, control: StoredProject["control"], backend: "opfs" | "indexeddb"): StoredProject {
  return {
    id: s.projectId,
    schemaVersion: 2,
    name: s.name,
    createdAt: Date.now(),
    updatedAt: Date.now(),
    blobBackend: backend,
    // BPM value AND source persist per song.
    control: {
      ...control,
      grid: { bpm: s.bpm, source: s.bpmSource },
      songGrid: s.songGrid ?? control.songGrid ?? null,
    },
    highMemoryMode: s.highMemoryMode,
    stems: Object.values(s.stems).map((stem) => ({
      role: stem.role,
      filename: stem.filename,
      container: stem.probe.sniff.container,
      extension: stem.filename.slice(stem.filename.lastIndexOf(".")),
      mimeType: stem.blob.type,
      bytes: stem.blob.size,
      duration: stem.probe.duration ?? 0,
      channels: stem.probe.channels ?? 0,
      sourceSampleRate: stem.probe.sniff.sampleRate ?? 0,
      decodedSampleRate: stem.probe.decodedSampleRate ?? 0,
      contentHash: stem.contentHash,
      blobKey: stem.blobKey,
      provenance: stem.provenance,
      trashed: stem.trashed,
      derived: stem.derived?.derivedKey
        ? {
            derivedKey: stem.derived.derivedKey,
            sourceBlobKey: stem.derived.sourceBlobKey ?? stem.blobKey,
            sourceContentHash: stem.derived.sourceContentHash ?? stem.contentHash,
            conversion: stem.derived.conversion ?? { type: "mono-downmix" as const, sampleRate: 0 },
            derivedSchemaVersion: stem.derived.derivedSchemaVersion ?? DERIVED_SCHEMA_VERSION,
          }
        : null,
    })),
  };

}
