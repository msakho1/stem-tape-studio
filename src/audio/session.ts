/**
 * Session project state — the bridge between the project drawer and the
 * instrument. Module-level (not React context) because the AudioEngine is a
 * singleton and client-side route changes must not drop a loaded song.
 *
 * Nothing here touches the network. User audio stays in memory / IndexedDB /
 * OPFS; only the procedurally generated demo is ever labelled `bundled-demo`.
 */

import type { ProbeResult, StemRole } from "./format";
import type { StoredProject } from "./store";

export interface SessionStem {
  role: StemRole;
  filename: string;
  blob: Blob;
  probe: ProbeResult;
  provenance: "user-private" | "bundled-demo";
  decodedBytes: number;
  loaded: boolean;
  /** Deleted from the surface but recoverable until the project is compacted. */
  trashed: boolean;
  blobKey: string;
  contentHash: string;
}

export interface SessionState {
  projectId: string;
  name: string;
  stems: Partial<Record<StemRole, SessionStem>>;
  saved: boolean;
  savedAt: number | null;
  source: "none" | "demo" | "upload" | "restored";
  lastError: string | null;
}

function emptySession(): SessionState {
  return {
    projectId: `proj-${Math.random().toString(36).slice(2, 10)}`,
    name: "untitled session",
    stems: {},
    saved: false,
    savedAt: null,
    source: "none",
    lastError: null,
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
    schemaVersion: 1,
    name: s.name,
    createdAt: Date.now(),
    updatedAt: Date.now(),
    blobBackend: backend,
    control,
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
    })),
  };
}
