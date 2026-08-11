/**
 * Shared demo-project loader so the header "TRY DEMO" button and the project
 * drawer run the exact same ingest path (sequential, lowest peak RAM).
 */
import { buildDemoProject, DEMO_TITLE } from "./demo";
import { ingestSequential } from "./ingest";
import { session } from "./session";
import { ROLE_LABEL } from "./format";
import type { AudioEngine } from "./engine";

export async function loadDemoProject(
  engine: AudioEngine,
  opts: { signal?: AbortSignal; onResult?: (ok: boolean, text: string) => void } = {},
): Promise<void> {
  session.reset();
  engine.resetDecodeCounters();
  const stems = await buildDemoProject(opts.signal);
  await ingestSequential(
    engine,
    stems.map((stem) => ({
      role: stem.role,
      file: new File([stem.blob], stem.filename, { type: "audio/wav" }),
      provenance: "bundled-demo" as const,
    })),
    {
      ...(opts.signal ? { signal: opts.signal } : {}),
      onResult: (r) => opts.onResult?.(r.ok, `${ROLE_LABEL[r.role]} — ${r.detail}`),
    },
  );
  session.set({ name: DEMO_TITLE, source: "demo" });
}
