import { useCallback, useEffect, useState } from "react";
import { formatMiB } from "@/audio/memory";
import { installRedecode } from "@/audio/ingest";
import type { AudioEngine, EngineStatus, TrackId } from "@/audio/engine";

interface Props {
  engine: AudioEngine;
  status: EngineStatus;
}

/**
 * Phase 5B engine selector and migration diagnostics.
 *
 * The default is ALWAYS the Phase 5A node engine. Selecting the worklet engine
 * runs the full preflight — availability, secure context, module load,
 * operation-level memory gate, processor readiness — and a refusal leaves 5A
 * playing, untouched.
 */
export function WorkletPanel({ engine, status }: Props) {
  const [busy, setBusy] = useState<string | null>(null);
  const [report, setReport] = useState<string>("");
  const [drift, setDrift] = useState<string>("");

  useEffect(() => {
    installRedecode(engine);
  }, [engine]);

  const run = useCallback(
    async (label: string, fn: () => Promise<{ ok: boolean; detail: string }>) => {
      setBusy(label);
      const r = await fn();
      setReport(`${label}: ${r.ok ? "OK" : "REFUSED"}\n${r.detail}`);
      setBusy(null);
    },
    [],
  );

  const selectWorklet = () => {
    engine.enginePreference = "worklet";
    void run("preflight", async () => {
      const pre = await engine.preflight();
      return { ok: pre.ok, detail: pre.checks.map((c) => `${c.ok ? "✓" : "✗"} ${c.name} — ${c.detail}`).join("\n") };
    });
  };

  const selectNode = () => {
    void run("revert to node engine", () => engine.revertToNode());
  };

  const gateLine = status.migrationAllowed
    ? `allowed · worst case ${formatMiB(status.migrationWorstCaseBytes)}`
    : `refused · worst case ${formatMiB(status.migrationWorstCaseBytes)}`;

  return (
    <section className="st-card mt-4" data-testid="worklet-panel">
      <h2 className="font-mono text-[11px] uppercase tracking-[0.2em] text-[var(--ink)]">
        engine · phase 5b (experimental)
      </h2>

      <div className="mt-2 flex flex-wrap gap-2">
        <button
          type="button"
          className="st-toggle"
          data-testid="engine-node"
          data-active={status.enginePreference === "node"}
          onClick={selectNode}
        >
          node engine — stable
        </button>
        <button
          type="button"
          className="st-toggle"
          data-testid="engine-worklet"
          data-active={status.enginePreference === "worklet"}
          onClick={selectWorklet}
        >
          worklet engine — experimental
        </button>
        <button
          type="button"
          className="st-toggle"
          data-testid="migrate-one"
          disabled={busy != null}
          onClick={() => void run("migrate track 1", () => engine.migrateTrack(0 as TrackId))}
        >
          migrate track 1
        </button>
        <button
          type="button"
          className="st-toggle"
          data-testid="migrate-all"
          disabled={busy != null}
          onClick={() => void run("migrate all four", () => engine.migrateAll())}
        >
          migrate all 4
        </button>
        <button
          type="button"
          className="st-toggle"
          data-testid="measure-drift"
          onClick={() =>
            void engine.measureDrift().then((d) =>
              setDrift(
                d.pairs.length
                  ? `max ${d.maxDrift} frames · ${d.pairs.join(" · ")}`
                  : "no worklet tracks to compare",
              ),
            )
          }
        >
          measure drift
        </button>
        <button
          type="button"
          className="st-toggle"
          data-testid="force-failure"
          onClick={() => {
            const first = status.tracks.find((t) => t.engineMode === "worklet");
            if (first == null) {
              setReport("forced failure: no worklet track");
              return;
            }
            void engine.forceProcessorFailure(first.id as TrackId).then((detail) => setReport(`forced failure: ${detail}`));
          }}
        >
          force processor failure
        </button>
      </div>

      <dl className="mt-3 grid grid-cols-2 gap-x-3 gap-y-1 font-mono text-[10px] text-[var(--ink-dim)]">
        <dt>preference</dt>
        <dd className="text-[var(--ink)]" data-testid="engine-preference">{status.enginePreference}</dd>
        <dt>worklet supported</dt>
        <dd>{String(status.workletSupported)}</dd>
        <dt>tracks on worklet</dt>
        <dd data-testid="worklet-track-count">{status.workletTrackCount} / 4</dd>
        <dt>migration gate</dt>
        <dd data-testid="migration-gate">{gateLine}</dd>
        <dt>peak migration memory</dt>
        <dd data-testid="migration-peak">{formatMiB(status.lastMigrationPeakBytes)}</dd>
        <dt>reverse bytes</dt>
        <dd data-testid="reverse-bytes">{formatMiB(status.reverseBytes)}</dd>
        <dt>underruns</dt>
        <dd data-testid="underrun-label">{status.underrunLabel}</dd>
        <dt>pairwise drift</dt>
        <dd data-testid="pairwise-drift">{drift || "—"}</dd>
      </dl>

      <p className="mt-2 font-mono text-[10px] leading-relaxed text-[var(--ink-faint)]" data-testid="migration-statement">
        {status.migrationStatement}
      </p>

      <ul className="mt-2 grid gap-1" data-testid="worklet-tracks">
        {status.tracks.map((t) => (
          <li key={t.id} className="font-mono text-[10px] text-[var(--ink-dim)]">
            <span className="text-[var(--ink)]">T{t.id + 1}</span> · {t.engineMode} · {t.migrationStatus} ·
            drift {t.driftFrames == null ? "—" : `${t.driftFrames} frames`} ·
            pcm {formatMiB(t.engineMode === "worklet" ? t.workletPcmBytes : t.decodedBytes)} ·
            wraps {t.workletWraps}
            {t.fallbackReason ? ` · fallback: ${t.fallbackReason}` : ""}
            {t.lastWorkletAck ? ` · ack ${t.lastWorkletAck}` : ""}
          </li>
        ))}
      </ul>

      {report ? (
        <pre
          className="mt-2 whitespace-pre-wrap font-mono text-[10px] text-[var(--ink-dim)]"
          data-testid="worklet-report"
        >
          {report}
        </pre>
      ) : null}

      <ul className="mt-2 grid gap-1" data-testid="migration-log">
        {status.migrationLog.slice(0, 8).map((line, i) => (
          <li key={i} className="font-mono text-[10px] text-[var(--ink-faint)]">
            {line}
          </li>
        ))}
      </ul>
    </section>
  );
}
