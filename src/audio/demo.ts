/**
 * Bundled demo project — "All Your Love", four real stems (vocal, drums, bass,
 * guitar) supplied by the project owner and served from the Lovable CDN as
 * 16-bit 44.1 kHz stereo WAVs, i.e. exactly the P4 contract format.
 *
 * The files are fetched over the network ONCE on demand and then decoded
 * locally. Nothing about user audio ever leaves the device; this is the only
 * audio the app ever downloads, and it is the demo itself.
 */

import type { StemRole } from "./format";
import vocalAsset from "@/assets/demo/vocal.wav.asset.json";
import drumsAsset from "@/assets/demo/drums.wav.asset.json";
import bassAsset from "@/assets/demo/bass.wav.asset.json";
import guitarAsset from "@/assets/demo/guitar.wav.asset.json";

export const DEMO_SAMPLE_RATE = 44100;
export const DEMO_TITLE = "All Your Love";
const DEMO_SECONDS = 162.8;

interface DemoSource {
  role: StemRole;
  url: string;
  filename: string;
  bytes: number;
}

/** instruments/other lane carries the guitar stem. */
const DEMO_SOURCES: DemoSource[] = [
  { role: "vocals", url: vocalAsset.url, filename: "all-your-love-vocal.wav", bytes: vocalAsset.size },
  { role: "drums", url: drumsAsset.url, filename: "all-your-love-drums.wav", bytes: drumsAsset.size },
  { role: "bass", url: bassAsset.url, filename: "all-your-love-bass.wav", bytes: bassAsset.size },
  { role: "instruments", url: guitarAsset.url, filename: "all-your-love-guitar.wav", bytes: guitarAsset.size },
];

export const DEMO_TOTAL_BYTES = DEMO_SOURCES.reduce((n, s) => n + s.bytes, 0);

export interface DemoStem {
  role: StemRole;
  filename: string;
  blob: Blob;
}

/**
 * Fetch the four demo stems. Sequential on purpose: one encoded file resident
 * at a time keeps peak RAM to a single stem, matching the ingest path.
 */
export async function buildDemoProject(signal?: AbortSignal): Promise<DemoStem[]> {
  const out: DemoStem[] = [];
  for (const s of DEMO_SOURCES) {
    const res = await fetch(s.url, signal ? { signal } : {});
    if (!res.ok) throw new Error(`demo stem ${s.role} failed to download (${res.status})`);
    out.push({ role: s.role, filename: s.filename, blob: await res.blob() });
  }
  return out;
}

export const DEMO_NOTICE =
  `${DEMO_TITLE} · four real stems (vocal · drums · bass · guitar) · 16-bit 44.1 kHz stereo WAV · ` +
  `${DEMO_SECONDS.toFixed(0)} s · ${(DEMO_TOTAL_BYTES / 1048576).toFixed(0)} MiB downloaded once, decoded on this device`;
