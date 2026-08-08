/**
 * Main-thread export orchestrator (plan §L 6C, M12).
 *
 * Chooses the export mode from the measured device ceiling, streams the take
 * through `wavWorker`, and hands the user a local download. No upload path
 * exists anywhere in this file.
 */

import type { TakeManifest } from "@/audio/input/takes";
import { exportPlan, wavTotalBytes, type BitDepth } from "./wavStream";

export interface ExportResult {
  ok: boolean;
  detail: string;
  filename?: string;
  bytes?: number;
}

/** Conservative measured ceiling: mobile Safari reliably materialises ~180 MiB blobs. */
export function measuredSafeBytes(): number {
  const ua = typeof navigator !== "undefined" ? navigator.userAgent : "";
  const ios = /iPad|iPhone|iPod/.test(ua) || (/Macintosh/.test(ua) && typeof document !== "undefined" && "ontouchend" in document);
  return ios ? 180 * 1024 * 1024 : 512 * 1024 * 1024;
}

export function planFor(take: TakeManifest, bitDepth: BitDepth) {
  const spec = { sampleRate: take.sampleRate, channels: take.channels, bitDepth, frames: take.frames };
  return { spec, total: wavTotalBytes(spec), plan: exportPlan(spec, measuredSafeBytes()) };
}

export async function exportTakeToWav(
  take: TakeManifest,
  bitDepth: BitDepth,
  onProgress?: (written: number, total: number) => void,
): Promise<ExportResult> {
  if (take.state !== "ready") return { ok: false, detail: `take ${take.id} is ${take.state} — refusing to export an incomplete recording` };
  if (!take.chunks.length || take.frames <= 0) return { ok: false, detail: "take holds no audio" };
  const { plan } = planFor(take, bitDepth);
  const depth: BitDepth = plan.mode === "16-bit" ? 16 : bitDepth;
  const filename = `${(take.label || take.id).replace(/[^\w.-]+/g, "_")}-${depth}bit.wav`;

  const worker = new Worker(new URL("../../workers/wavWorker.ts", import.meta.url), { type: "module" });
  try {
    const result = await new Promise<ExportResult>((resolve) => {
      worker.onmessage = (e) => {
        const m = e.data as Record<string, unknown>;
        if (m["type"] === "progress") {
          onProgress?.(Number(m["written"]), Number(m["total"]));
          return;
        }
        if (m["type"] === "failed") {
          resolve({ ok: false, detail: String(m["reason"]) });
          return;
        }
        if (m["type"] === "done") {
          const blob = m["blob"] as Blob;
          const url = URL.createObjectURL(blob);
          const a = document.createElement("a");
          a.href = url;
          a.download = String(m["filename"]);
          a.click();
          setTimeout(() => URL.revokeObjectURL(url), 4000);
          resolve({
            ok: true,
            filename: String(m["filename"]),
            bytes: Number(m["bytes"]),
            detail: `${String(m["filename"])} · ${(Number(m["bytes"]) / 1048576).toFixed(1)} MiB · ${depth}-bit · ${plan.detail} · assembled and saved on this device only`,
          });
        }
      };
      worker.onerror = (err) => resolve({ ok: false, detail: `export worker failed: ${err.message}` });
      worker.postMessage({
        type: "export",
        chunks: take.chunks.map((c) => ({ blobKey: c.blobKey, frames: c.frames })),
        frames: take.frames,
        channels: take.channels,
        sampleRate: take.sampleRate,
        bitDepth: depth,
        filename,
      });
    });
    return result;
  } finally {
    worker.terminate();
  }
}
