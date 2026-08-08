/**
 * Memory Saver — explicit, opt-in working copies.
 *
 * Rules enforced here:
 *  - the ORIGINAL blob is never touched, replaced or re-encoded in place;
 *  - the derived copy is a separate, separately-keyed local asset that can be
 *    deleted and regenerated;
 *  - once persisted, reopening uses the derived blob DIRECTLY — the full
 *    stereo source is not decoded again;
 *  - the predicted saving is shown before anything is generated;
 *  - one track at a time. Nothing is downmixed silently or by default.
 */

import { DERIVED_SCHEMA_VERSION, projectStore, type DerivedAsset } from "./store";
import { encodeWav } from "./wav";
import type { StemRole } from "./format";

export interface SaverPrediction {
  currentBytes: number;
  derivedBytes: number;
  savedBytes: number;
  applicable: boolean;
  reason: string;
}

/** Mono downmix halves a stereo buffer; a mono source has nothing to save. */
export function predictMonoDownmix(channels: number, decodedBytes: number): SaverPrediction {
  if (channels < 2) {
    return {
      currentBytes: decodedBytes,
      derivedBytes: decodedBytes,
      savedBytes: 0,
      applicable: false,
      reason: "already mono — no saving available",
    };
  }
  const derived = Math.round(decodedBytes / channels);
  return {
    currentBytes: decodedBytes,
    derivedBytes: derived,
    savedBytes: decodedBytes - derived,
    applicable: true,
    reason: `${channels}ch → 1ch working copy`,
  };
}

/** Decodes nothing: the caller passes the buffer it already holds. */
export function downmixToMonoWav(buffer: AudioBuffer): Blob {
  const frames = buffer.length;
  const out = new Float32Array(frames);
  const ch = buffer.numberOfChannels;
  for (let c = 0; c < ch; c++) {
    const data = buffer.getChannelData(c);
    for (let i = 0; i < frames; i++) out[i] = (out[i] ?? 0) + (data[i] ?? 0) / ch;
  }
  return encodeWav([out], buffer.sampleRate);
}

export async function persistDerived(
  projectId: string,
  role: StemRole,
  sourceBlobKey: string,
  sourceContentHash: string,
  blob: Blob,
  sampleRate: number,
): Promise<DerivedAsset> {
  const derivedKey = `${projectId}--${role}--mono--${sourceContentHash}`;
  await projectStore.putBlob(derivedKey, blob);
  return {
    derivedKey,
    sourceBlobKey,
    sourceContentHash,
    conversion: { type: "mono-downmix", sampleRate },
    derivedSchemaVersion: DERIVED_SCHEMA_VERSION,
  };
}

export async function dropDerived(asset: DerivedAsset): Promise<void> {
  await projectStore.deleteBlob(asset.derivedKey);
}
