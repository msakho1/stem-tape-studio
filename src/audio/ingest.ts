import { contentHash, projectStore } from "./store";
import { probeFile, type StemRole } from "./format";
import { session, type SessionStem } from "./session";
import type { AudioEngine, TrackId } from "./engine";
import { STEM_ROLE_LIST } from "./format";

export const ROLE_TRACK: Record<StemRole, TrackId> = {
  vocals: 0,
  drums: 1,
  bass: 2,
  instruments: 3,
};

export interface IngestResult {
  ok: boolean;
  role: StemRole;
  detail: string;
}

/**
 * One ingest path for every stem, user file or generated demo:
 * sniff -> real decode probe -> persist blob locally -> decode into the engine.
 * No step performs a network request; the bytes never leave the tab.
 */
export async function ingestStem(
  engine: AudioEngine,
  role: StemRole,
  file: File,
  provenance: SessionStem["provenance"],
): Promise<IngestResult> {
  await engine.unlock();
  if (!engine.ctx) return { ok: false, role, detail: "audio could not be unlocked in this browser" };

  const probe = await probeFile(file, engine.ctx);
  if (probe.verdict === "malformed" || probe.verdict === "unsupported") {
    return { ok: false, role, detail: `${probe.verdict}: ${probe.error ?? "cannot decode"}` };
  }

  const hash = await contentHash(file);
  const blobKey = `${session.get().projectId}--${role}--${hash}`;
  let stored = false;
  try {
    await projectStore.putBlob(blobKey, file);
    stored = true;
  } catch {
    /* Storage can be full or blocked; playback must still work from memory. */
  }

  const bytes = await file.arrayBuffer();
  const loaded = await engine.loadTrack(ROLE_TRACK[role], bytes, { name: file.name, provenance });
  if (!loaded.ok) return { ok: false, role, detail: loaded.detail };

  session.setStem(role, {
    role,
    filename: file.name,
    blob: file,
    probe,
    provenance,
    decodedBytes: Math.round((probe.duration ?? 0) * (probe.decodedSampleRate ?? 48000) * (probe.channels ?? 2) * 4),
    loaded: true,
    trashed: false,
    blobKey,
    contentHash: hash,
  });

  return {
    ok: true,
    role,
    detail: `${loaded.detail}${stored ? "" : " · local copy NOT persisted (storage unavailable)"}${
      probe.paddingWarning ? " · compressed source: encoder padding may offset alignment" : ""
    }`,
  };
}

export function missingRoles(): StemRole[] {
  const stems = session.get().stems;
  return STEM_ROLE_LIST.filter((r) => !stems[r] || stems[r]!.trashed);
}
