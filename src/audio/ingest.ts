import { contentHash, projectStore } from "./store";
import { probeFile, formatBytes, type StemRole, type PreDecodeEstimate } from "./format";
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
  estimate?: PreDecodeEstimate | undefined;
  decodedBytes?: number | undefined;
  decodeMs?: number | undefined;
  /** Which gate refused it, when it was refused. */
  refusedAt?: "pre-decode" | "post-decode" | "format" | undefined;
}


export interface IngestOptions {
  /** Aborts the remaining work of a queued multi-stem load. */
  signal?: AbortSignal;
  /** Skip the probe decode and adopt these bytes as an already-derived copy. */
  derived?: boolean;
}

/**
 * ONE ingest path for every stem, user file or generated demo:
 *
 *   sniff → pre-decode gate (estimate) → ONE decode → post-decode gate (exact)
 *         → adopt that same AudioBuffer → persist the original Blob
 *
 * The decoded buffer produced by the probe is the buffer the engine plays.
 * Nothing here performs a network request; the bytes never leave the tab.
 */
export async function ingestStem(
  engine: AudioEngine,
  role: StemRole,
  file: File,
  provenance: SessionStem["provenance"],
  opts: IngestOptions = {},
): Promise<IngestResult> {
  if (opts.signal?.aborted) return { ok: false, role, detail: "cancelled before decode" };
  await engine.unlock();
  if (!engine.ctx) return { ok: false, role, detail: "audio could not be unlocked in this browser" };

  const trackId = ROLE_TRACK[role];
  let refusal: string | null = null;

  // Stage 1 + the single decode happen inside probeFile: the gate callback runs
  // BEFORE decodeAudioData allocates anything.
  const probe = await probeFile(file, engine.ctx, {
    gate: (estimate) => {
      const gate = engine.preDecodeGate(trackId, estimate.decodedBytes);
      if (!gate.ok) refusal = gate.detail;
      return gate;
    },
  });

  if (refusal) {
    return { ok: false, role, detail: refusal, estimate: probe.estimate, refusedAt: "pre-decode" };
  }
  if (probe.verdict === "malformed" || probe.verdict === "unsupported" || probe.verdict === "decode-failed") {
    return {
      ok: false,
      role,
      detail: `${probe.verdict}: ${probe.error ?? "cannot decode"}`,
      estimate: probe.estimate,
      refusedAt: "format",
    };
  }
  if (!probe.buffer) return { ok: false, role, detail: "decode produced no buffer", refusedAt: "format" };

  if (opts.signal?.aborted) {
    // Dereference the decoded buffer we just produced (GC timing not controlled).
    return { ok: false, role, detail: "cancelled after decode — buffer dereferenced" };
  }

  // Stage 2: exact verdict against the real buffer. Rejected buffers are never
  // installed and go out of scope here.
  const adopted = engine.adoptBuffer(trackId, probe.buffer, {
    name: file.name,
    provenance,
    decodeMs: probe.decodeMs,
    reused: true,
  });
  if (!adopted.ok) {
    return { ok: false, role, detail: adopted.detail, estimate: probe.estimate, refusedAt: "post-decode" };
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

  session.setStem(role, {
    role,
    filename: file.name,
    blob: file,
    probe: { ...probe, buffer: undefined } as SessionStem["probe"],
    provenance,
    decodedBytes: adopted.bytes,
    fileBytes: file.size,
    estimate: probe.estimate ?? null,
    decodeCount: 1,
    decodeMs: probe.decodeMs ?? null,
    loaded: true,
    trashed: false,
    blobKey,
    contentHash: hash,
    derived: opts.derived ? { kind: "mono-downmix" } : null,
  });

  return {
    ok: true,
    role,
    detail: `${adopted.detail} · retained ${formatBytes(adopted.bytes)}${stored ? "" : " · local copy NOT persisted (storage unavailable)"}${
      probe.paddingWarning ? " · compressed source: encoder padding may offset alignment" : ""
    }`,
    estimate: probe.estimate,
    decodedBytes: adopted.bytes,
    decodeMs: probe.decodeMs,
  };
}

/** Yield to the browser so a large sequential load never blocks the surface. */
export function yieldToBrowser(): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, 0));
}

/**
 * Sequential ingest. Four large stems are never decoded concurrently: decode
 * one, adopt it, release the encoded reference, yield, continue.
 */
export async function ingestSequential(
  engine: AudioEngine,
  items: { role: StemRole; file: File; provenance: SessionStem["provenance"] }[],
  opts: IngestOptions & { onResult?: (r: IngestResult) => void } = {},
): Promise<IngestResult[]> {
  const results: IngestResult[] = [];
  for (const item of items) {
    if (opts.signal?.aborted) {
      const r: IngestResult = { ok: false, role: item.role, detail: "cancelled — no decode attempted" };
      results.push(r);
      opts.onResult?.(r);
      continue;
    }
    const r = await ingestStem(engine, item.role, item.file, item.provenance, opts);
    results.push(r);
    opts.onResult?.(r);
    await yieldToBrowser();
  }
  return results;
}

export function missingRoles(): StemRole[] {
  const stems = session.get().stems;
  return STEM_ROLE_LIST.filter((r) => !stems[r] || stems[r]!.trashed);
}
