import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { DEMO_NOTICE, buildDemoProject } from "@/audio/demo";
import { ROLE_LABEL, STEM_ROLE_LIST, formatBytes, type StemRole } from "@/audio/format";
import { ingestSequential, ingestStem, ROLE_TRACK } from "@/audio/ingest";
import { describeVerdict, formatMiB, judge, reverseCostBytes } from "@/audio/memory";
import { downmixToMonoWav, persistDerived, predictMonoDownmix } from "@/audio/saver";
import { resolveBpm, session, toStoredProject, type SessionState } from "@/audio/session";
import { projectStore, type StorageReport, type StoredProject } from "@/audio/store";
import type { AudioEngine, EngineStatus, TrackId } from "@/audio/engine";

interface Props {
  engine: AudioEngine;
  status: EngineStatus;
  /** Reducer-owned control state, stored alongside the project record. */
  control: StoredProject["control"];
}

/**
 * Project drawer — the only place audio enters or leaves the prototype.
 * Privacy contract: no network request in this app ever contains user-selected
 * or user-recorded audio. (Bundled demo assets are ordinary app fetches.)
 *
 * Unit contract: every memory figure below is MiB (1024²), labelled MiB.
 */
export function ProjectDrawer({ engine, status, control }: Props) {
  const [sess, setSess] = useState<SessionState>(() => session.get());
  const [busy, setBusy] = useState<string | null>(null);
  const [log, setLog] = useState<{ ok: boolean; text: string }[]>([]);
  const [report, setReport] = useState<StorageReport | null>(null);
  const [projects, setProjects] = useState<StoredProject[]>([]);
  const [manualBpm, setManualBpm] = useState<string>("");
  const inputs = useRef<Partial<Record<StemRole, HTMLInputElement | null>>>({});
  const abort = useRef<AbortController | null>(null);

  useEffect(() => {
    const off = session.subscribe(setSess);
    return () => {
      off();
    };
  }, []);
  const refresh = useCallback(async () => {
    setReport(await projectStore.report());
    setProjects(await projectStore.listProjects());
  }, []);
  useEffect(() => {
    void refresh();
  }, [refresh]);

  const note = (ok: boolean, text: string) => setLog((prev) => [{ ok, text }, ...prev].slice(0, 10));

  const onPick = useCallback(
    async (role: StemRole, file: File | undefined) => {
      if (!file) return;
      setBusy(`ingesting ${role}…`);
      const controller = new AbortController();
      abort.current = controller;
      const r = await ingestStem(engine, role, file, "user-private", { signal: controller.signal });
      note(r.ok, `${ROLE_LABEL[role]} — ${r.detail}`);
      session.set({ source: "upload", saved: false });
      abort.current = null;
      setBusy(null);
      void refresh();
    },
    [engine, refresh],
  );

  const loadDemo = useCallback(async () => {
    setBusy("generating demo stems…");
    session.reset();
    engine.resetDecodeCounters();
    const controller = new AbortController();
    abort.current = controller;
    await ingestSequential(
      engine,
      buildDemoProject().map((stem) => ({
        role: stem.role,
        file: new File([stem.blob], stem.filename, { type: "audio/wav" }),
        provenance: "bundled-demo" as const,
      })),
      {
        signal: controller.signal,
        onResult: (r) => note(r.ok, `${ROLE_LABEL[r.role]} — ${r.detail}`),
      },
    );
    session.set({ name: "demo session", source: "demo" });
    abort.current = null;
    setBusy(null);
    void refresh();
  }, [engine, refresh]);

  const save = useCallback(async () => {
    setBusy("saving project…");
    const backend = await projectStore.blobBackend();
    await projectStore.saveProject(toStoredProject(session.get(), control, backend));
    session.set({ saved: true, savedAt: Date.now() });
    note(true, `saved locally to ${backend} — audio stays on this device`);
    setBusy(null);
    void refresh();
  }, [control, refresh]);

  const restore = useCallback(
    async (project: StoredProject) => {
      setBusy(`restoring “${project.name}”…`);
      session.reset();
      engine.resetDecodeCounters();
      session.set({
        projectId: project.id,
        name: project.name,
        source: "restored",
        highMemoryMode: project.highMemoryMode ?? false,
        bpm: project.control.grid.bpm ?? 120,
        bpmSource: (project.control.grid.source as SessionState["bpmSource"]) ?? "provisional",
      });
      engine.setHighMemoryMode(project.highMemoryMode ?? false);
      const controller = new AbortController();
      abort.current = controller;
      for (const stem of project.stems) {
        // Memory Saver: reopen the DERIVED working copy directly. The full
        // stereo original is not decoded again.
        const useDerived = stem.derived?.derivedKey != null;
        const key = useDerived ? stem.derived!.derivedKey : stem.blobKey;
        const blob = await projectStore.getBlob(key);
        if (!blob) {
          note(false, `${ROLE_LABEL[stem.role]} — blob missing from local storage (${key})`);
          continue;
        }
        const file = new File([blob], stem.filename, { type: useDerived ? "audio/wav" : stem.mimeType || "audio/wav" });
        const r = await ingestStem(engine, stem.role, file, stem.provenance, {
          signal: controller.signal,
          derived: useDerived,
        });
        note(r.ok, `${ROLE_LABEL[stem.role]} — ${useDerived ? "mono working copy · " : ""}${r.detail}`);
      }
      abort.current = null;
      // Amendment 2: a song load stops the transport and waits for PLAY.
      engine.execute({ id: -1, t: performance.now(), type: "song.load", payload: { song: 0 } });
      setBusy(null);
    },
    [engine],
  );

  const applyMemorySaver = useCallback(
    async (role: StemRole) => {
      const stem = sess.stems[role];
      const id = ROLE_TRACK[role] as TrackId;
      const buffer = engine.getBuffer(id);
      if (!stem || !buffer) return;
      setBusy(`generating mono working copy for ${role}…`);
      const blob = downmixToMonoWav(buffer);
      const asset = await persistDerived(sess.projectId, role, stem.blobKey, stem.contentHash, blob, buffer.sampleRate);
      const file = new File([blob], stem.filename.replace(/(\.[^.]+)$/, "-mono.wav"), { type: "audio/wav" });
      const r = await ingestStem(engine, role, file, stem.provenance, { derived: true });
      const current = session.get().stems[role];
      if (current) session.setStem(role, { ...current, derived: { kind: "mono-downmix", ...asset }, blobKey: stem.blobKey });
      note(r.ok, `${ROLE_LABEL[role]} — mono working copy stored (original untouched) · ${r.detail}`);
      setBusy(null);
      void refresh();
    },
    [engine, refresh, sess],
  );

  const setBpm = useCallback(() => {
    const n = Number(manualBpm);
    if (!Number.isFinite(n) || n <= 0) return;
    const r = resolveBpm(null, n);
    session.set({ bpm: r.bpm, bpmSource: r.source, saved: false });
    note(true, `project BPM set manually to ${r.bpm} — future ±1 BPM steps use it; current playback rate unchanged`);
  }, [manualBpm]);

  const toggleHighMemory = useCallback(() => {
    const on = !sess.highMemoryMode;
    engine.setHighMemoryMode(on);
    session.set({ highMemoryMode: on, saved: false });
    note(true, `High Memory Mode ${on ? "enabled" : "disabled"} — ceiling ${formatMiB(status.budget.highMemoryBlockBytes)}`);
  }, [engine, sess.highMemoryMode, status.budget.highMemoryBlockBytes]);

  const cancel = useCallback(() => {
    abort.current?.abort();
    note(false, "load cancelled — remaining stems not decoded, intermediates dereferenced (GC timing not controlled)");
  }, []);

  const memory = useMemo(() => {
    const loaded = STEM_ROLE_LIST.map((role) => ({ role, stem: sess.stems[role] })).filter((s) => s.stem);
    const retained = status.decodedBytes;
    const peak = retained + Math.max(0, ...STEM_ROLE_LIST.map((r) => sess.stems[r]?.fileBytes ?? 0));
    const perStem = loaded.length ? retained / loaded.length : 0;
    const projectedFour = loaded.length > 0 && loaded.length < 4 ? Math.round(perStem * 4) : null;
    return {
      retained,
      peak,
      projectedFour,
      reverse: reverseCostBytes(retained),
      verdict: judge(retained, status.budget, sess.highMemoryMode),
      statement: describeVerdict(retained, status.budget, sess.highMemoryMode),
      loadedCount: loaded.length,
    };
  }, [sess, status.decodedBytes, status.budget, sess.highMemoryMode]);

  return (
    <section className="st-card" data-testid="project-drawer">
      <header className="flex items-baseline justify-between gap-3">
        <h2 className="font-mono text-[11px] uppercase tracking-[0.2em] text-[var(--ink)]">project · stems</h2>
        <span className="font-mono text-[10px] text-[var(--ink-faint)]">{sess.saved ? "saved locally" : "unsaved"}</span>
      </header>

      <p className="mt-2 font-mono text-[10px] leading-relaxed text-[var(--ink-faint)]">
        no network request in this app contains your audio — files are decoded in this tab and stored on this device
        only ({report?.backend ?? "…"}
        {report?.persisted ? " · persisted" : ""}).
      </p>

      <div className="mt-3 grid gap-2">
        {STEM_ROLE_LIST.map((role) => {
          const stem = sess.stems[role];
          const track = status.tracks[STEM_ROLE_LIST.indexOf(role)];
          const prediction = stem ? predictMonoDownmix(stem.probe.channels ?? 2, stem.decodedBytes) : null;
          return (
            <div key={role} className="border-b border-[var(--bench-line)] pb-2">
              <div className="flex items-center justify-between gap-3">
                <div className="min-w-0">
                  <p className="font-mono text-[11px] text-[var(--ink)]">
                    {ROLE_LABEL[role]}
                    <span className="ml-2 text-[var(--ink-faint)]">t{STEM_ROLE_LIST.indexOf(role) + 1}</span>
                  </p>
                  <p className="truncate font-mono text-[10px] text-[var(--ink-dim)]">
                    {stem
                      ? `${stem.filename} · file ${formatBytes(stem.fileBytes)} · ${stem.probe.duration?.toFixed(2) ?? "?"}s · ${
                          stem.probe.channels ?? "?"
                        }ch · ${stem.probe.decodedSampleRate ?? "?"} Hz · ${stem.probe.verdict}${
                          track?.decoded ? "" : " · not in engine"
                        }`
                      : "empty — choose a wav/mp3/m4a/flac/aiff file"}
                  </p>
                  {stem && (
                    <p
                      className="font-mono text-[10px] text-[var(--ink-faint)]"
                      data-testid={`stem-memory-${role}`}
                      data-decoded-bytes={stem.decodedBytes}
                      data-decode-count={track?.decodeCount ?? 0}
                      data-buffer-reused={String(track?.bufferReused ?? false)}
                    >
                      decoded {formatMiB(stem.decodedBytes)} · estimate{" "}
                      {stem.estimate ? formatMiB(stem.estimate.decodedBytes) : "—"}
                      {stem.estimate?.uncertain ? " (uncertain)" : " (exact)"} · decodes {track?.decodeCount ?? 0} ·{" "}
                      {track?.bufferReused ? "probe buffer reused" : "re-decoded"}
                      {stem.derived ? " · mono working copy" : ""}
                    </p>
                  )}
                </div>
                <input
                  ref={(el) => {
                    inputs.current[role] = el;
                  }}
                  type="file"
                  accept=".wav,.mp3,.m4a,.aac,.flac,.aif,.aiff,audio/*"
                  className="hidden"
                  onChange={(e) => void onPick(role, e.target.files?.[0])}
                />
                <button type="button" className="st-toggle shrink-0" onClick={() => inputs.current[role]?.click()}>
                  load
                </button>
              </div>
              {stem && prediction?.applicable && !stem.derived && (
                <button
                  type="button"
                  className="st-toggle mt-1"
                  data-testid={`memory-saver-${role}`}
                  onClick={() => void applyMemorySaver(role)}
                >
                  memory saver: mono copy saves {formatMiB(prediction.savedBytes)}
                </button>
              )}
            </div>
          );
        })}
      </div>

      <div className="mt-3 flex flex-wrap gap-2">
        <button type="button" className="st-toggle" onClick={() => void loadDemo()} data-testid="load-demo">
          load demo stems
        </button>
        <button type="button" className="st-toggle" onClick={() => void save()}>
          save project
        </button>
        <button type="button" className="st-toggle" onClick={() => void projectStore.requestPersistence().then(refresh)}>
          request persistence
        </button>
        {busy && (
          <button type="button" className="st-toggle" data-testid="cancel-load" onClick={cancel}>
            cancel load
          </button>
        )}
      </div>
      <p className="mt-2 font-mono text-[10px] text-[var(--ink-faint)]">demo: {DEMO_NOTICE}</p>

      <div className="mt-3 font-mono text-[10px] text-[var(--ink-dim)]" data-testid="memory-meter">
        <p data-testid="memory-total" data-retained-bytes={memory.retained}>
          retained decoded total: <span className="text-[var(--ink)]">{formatMiB(memory.retained)}</span>
          {memory.projectedFour != null ? ` · projected 4-role total ≈ ${formatMiB(memory.projectedFour)} (estimate)` : ""}
        </p>
        <p>estimated peak during load: {formatMiB(memory.peak)}</p>
        <p>estimated reverse-copy cost (Phase 5): {formatMiB(memory.reverse)}</p>
        <p>
          {status.budget.platform} thresholds — warn {formatMiB(status.budget.warnBytes)} · standard{" "}
          {formatMiB(status.budget.standardBlockBytes)} · high-memory ceiling{" "}
          {formatMiB(status.budget.highMemoryBlockBytes)}
        </p>
        <p data-verdict={memory.verdict} data-testid="memory-statement" className="text-[var(--ink)]">
          {memory.statement}
        </p>
        {report?.usage != null && report.quota != null && (
          <p className="text-[var(--ink-faint)]">
            device storage (not RAM): {formatBytes(report.usage)} of {formatBytes(report.quota)}
          </p>
        )}
      </div>

      <div className="mt-3 flex flex-wrap items-center gap-2">
        <button
          type="button"
          className="st-toggle"
          data-on={sess.highMemoryMode}
          data-testid="high-memory-toggle"
          onClick={toggleHighMemory}
        >
          high memory mode {sess.highMemoryMode ? "on" : "off"}
        </button>
        <span className="font-mono text-[10px] text-[var(--ink-faint)]">
          this project may use substantial browser memory. on some phones, loading it may cause the page to reload or
          audio to stop. your original files remain untouched.
        </span>
      </div>

      <div className="mt-3 flex flex-wrap items-center gap-2" data-testid="bpm-field">
        <span className="font-mono text-[10px] text-[var(--ink-dim)]">
          project bpm: <span className="text-[var(--ink)]">{sess.bpm}</span> —{" "}
          {sess.bpmSource === "provisional" ? "provisional (not detected)" : sess.bpmSource}
        </span>
        <input
          inputMode="decimal"
          value={manualBpm}
          onChange={(e) => setManualBpm(e.target.value)}
          placeholder="set bpm"
          className="w-20 border border-[var(--bench-line)] bg-transparent px-2 py-1 font-mono text-[10px] text-[var(--ink)]"
        />
        <button type="button" className="st-toggle" data-testid="set-bpm" onClick={setBpm}>
          apply
        </button>
      </div>

      {projects.length > 0 && (
        <div className="mt-3">
          <p className="font-mono text-[10px] uppercase tracking-[0.16em] text-[var(--ink-faint)]">saved projects</p>
          <div className="mt-1 grid gap-1">
            {projects.map((p) => (
              <div key={p.id} className="flex items-center justify-between gap-2">
                <span className="truncate font-mono text-[10px] text-[var(--ink-dim)]">
                  {p.name} · {p.stems.length} stems · v{p.schemaVersion}
                </span>
                <span className="flex gap-1">
                  <button type="button" className="st-toggle" onClick={() => void restore(p)}>
                    open
                  </button>
                  <button
                    type="button"
                    className="st-toggle"
                    onClick={() => void projectStore.deleteProject(p.id).then(refresh)}
                  >
                    delete
                  </button>
                </span>
              </div>
            ))}
          </div>
        </div>
      )}

      {busy && <p className="mt-3 font-mono text-[10px] text-[var(--ink)]">{busy}</p>}
      {log.length > 0 && (
        <ul className="mt-3 grid gap-1" data-testid="ingest-log">
          {log.map((l, i) => (
            <li key={i} className="font-mono text-[10px] text-[var(--ink-dim)]">
              <span className={l.ok ? "text-[var(--ink)]" : "text-[var(--led-hot,#c33)]"}>{l.ok ? "ok" : "fail"}</span>{" "}
              {l.text}
            </li>
          ))}
        </ul>
      )}
    </section>
  );
}
