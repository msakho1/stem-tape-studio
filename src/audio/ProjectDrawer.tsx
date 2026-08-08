import { useCallback, useEffect, useRef, useState } from "react";
import { DEMO_NOTICE, buildDemoProject } from "@/audio/demo";
import { ROLE_LABEL, STEM_ROLE_LIST, formatBytes, type StemRole } from "@/audio/format";
import { ingestStem } from "@/audio/ingest";
import { judge } from "@/audio/memory";
import { session, toStoredProject, type SessionState } from "@/audio/session";
import { projectStore, type StorageReport, type StoredProject } from "@/audio/store";
import type { AudioEngine, EngineStatus } from "@/audio/engine";

interface Props {
  engine: AudioEngine;
  status: EngineStatus;
  /** Reducer-owned control state, stored alongside the project record. */
  control: StoredProject["control"];
}

/**
 * Project drawer — the only place audio enters or leaves the prototype.
 * Privacy contract: no network request in this app ever contains user audio.
 * Files are read with FileReader/ArrayBuffer, kept in OPFS or IndexedDB on this
 * device, and decoded in this tab. Nothing is uploaded.
 */
export function ProjectDrawer({ engine, status, control }: Props) {
  const [sess, setSess] = useState<SessionState>(() => session.get());
  const [busy, setBusy] = useState<string | null>(null);
  const [log, setLog] = useState<{ ok: boolean; text: string }[]>([]);
  const [report, setReport] = useState<StorageReport | null>(null);
  const [projects, setProjects] = useState<StoredProject[]>([]);
  const inputs = useRef<Partial<Record<StemRole, HTMLInputElement | null>>>({});

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

  const note = (ok: boolean, text: string) => setLog((prev) => [{ ok, text }, ...prev].slice(0, 8));

  const onPick = useCallback(
    async (role: StemRole, file: File | undefined) => {
      if (!file) return;
      setBusy(`ingesting ${role}…`);
      const r = await ingestStem(engine, role, file, "user-private");
      note(r.ok, `${ROLE_LABEL[role]} — ${r.detail}`);
      session.set({ source: "upload", saved: false });
      setBusy(null);
      void refresh();
    },
    [engine, refresh],
  );

  const loadDemo = useCallback(async () => {
    setBusy("generating demo stems…");
    session.reset();
    for (const stem of buildDemoProject()) {
      const file = new File([stem.blob], stem.filename, { type: "audio/wav" });
      const r = await ingestStem(engine, stem.role, file, "bundled-demo");
      note(r.ok, `${ROLE_LABEL[stem.role]} — ${r.detail}`);
    }
    session.set({ name: "demo session", source: "demo" });
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
      session.set({ projectId: project.id, name: project.name, source: "restored" });
      for (const stem of project.stems) {
        const blob = await projectStore.getBlob(stem.blobKey);
        if (!blob) {
          note(false, `${ROLE_LABEL[stem.role]} — blob missing from local storage`);
          continue;
        }
        const file = new File([blob], stem.filename, { type: stem.mimeType || "audio/wav" });
        const r = await ingestStem(engine, stem.role, file, stem.provenance);
        note(r.ok, `${ROLE_LABEL[stem.role]} — ${r.detail}`);
      }
      // Amendment 2: a song load stops the transport and waits for PLAY.
      engine.execute({ id: -1, t: performance.now(), type: "song.load", payload: { song: 0 } });
      setBusy(null);
    },
    [engine],
  );

  const verdict = judge(status.decodedBytes, status.budget);

  return (
    <section className="st-card" data-testid="project-drawer">
      <header className="flex items-baseline justify-between gap-3">
        <h2 className="font-mono text-[11px] uppercase tracking-[0.2em] text-[var(--ink)]">project · stems</h2>
        <span className="font-mono text-[10px] text-[var(--ink-faint)]">
          {sess.saved ? "saved locally" : "unsaved"}
        </span>
      </header>

      <p className="mt-2 font-mono text-[10px] leading-relaxed text-[var(--ink-faint)]">
        no network request in this app contains your audio — files are decoded in this tab and stored on this
        device only ({report?.backend ?? "…"}
        {report?.persisted ? " · persisted" : ""}).
      </p>

      <div className="mt-3 grid gap-2">
        {STEM_ROLE_LIST.map((role) => {
          const stem = sess.stems[role];
          const track = status.tracks[STEM_ROLE_LIST.indexOf(role)];
          return (
            <div key={role} className="flex items-center justify-between gap-3 border-b border-[var(--bench-line)] pb-2">
              <div className="min-w-0">
                <p className="font-mono text-[11px] text-[var(--ink)]">
                  {ROLE_LABEL[role]}
                  <span className="ml-2 text-[var(--ink-faint)]">t{STEM_ROLE_LIST.indexOf(role) + 1}</span>
                </p>
                <p className="truncate font-mono text-[10px] text-[var(--ink-dim)]">
                  {stem
                    ? `${stem.filename} · ${stem.probe.duration?.toFixed(2) ?? "?"}s · ${stem.probe.channels ?? "?"}ch · ${
                        stem.probe.decodedSampleRate ?? "?"
                      } Hz · ${stem.probe.verdict}${track?.decoded ? "" : " · not in engine"}`
                    : "empty — choose a wav/mp3/m4a/flac/aiff file"}
                </p>
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
      </div>
      <p className="mt-2 font-mono text-[10px] text-[var(--ink-faint)]">demo: {DEMO_NOTICE}</p>

      <div className="mt-3 font-mono text-[10px] text-[var(--ink-dim)]" data-testid="memory-meter">
        decoded {formatBytes(status.decodedBytes)} / warn {formatBytes(status.budget.warnBytes)} / block{" "}
        {formatBytes(status.budget.blockBytes)} · {status.budget.platform} ·{" "}
        <span data-verdict={verdict} className={verdict === "ok" ? "text-[var(--ink-dim)]" : "text-[var(--ink)]"}>
          {verdict}
        </span>
        {report?.usage != null && report.quota != null
          ? ` · device storage ${formatBytes(report.usage)} of ${formatBytes(report.quota)}`
          : ""}
      </div>

      {projects.length > 0 && (
        <div className="mt-3">
          <p className="font-mono text-[10px] uppercase tracking-[0.16em] text-[var(--ink-faint)]">saved projects</p>
          <div className="mt-1 grid gap-1">
            {projects.map((p) => (
              <div key={p.id} className="flex items-center justify-between gap-2">
                <span className="truncate font-mono text-[10px] text-[var(--ink-dim)]">
                  {p.name} · {p.stems.length} stems · {new Date(p.updatedAt).toLocaleString()}
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
        <ul className="mt-3 grid gap-1">
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
