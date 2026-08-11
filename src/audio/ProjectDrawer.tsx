import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { DEMO_NOTICE, DEMO_TITLE, buildDemoProject } from "@/audio/demo";
import { ROLE_LABEL, STEM_ROLE_LIST, formatBytes, type StemRole } from "@/audio/format";
import { analyzeGridForSession, ingestSequential, ingestStem, ROLE_TRACK } from "@/audio/ingest";
import { describeGrid } from "@/audio/gridAnalysis";
import { formatMiB } from "@/audio/memory";
import { downmixToMonoWav, persistDerived, predictMonoDownmix } from "@/audio/saver";
import { session, toStoredProject, type SessionState } from "@/audio/session";
import { projectStore, type StoredProject } from "@/audio/store";
import type { AudioEngine, EngineStatus, TrackId } from "@/audio/engine";

interface Props {
  engine: AudioEngine;
  status: EngineStatus;
  /** Reducer-owned control state, stored alongside the project record. */
  control: StoredProject["control"];
}

/** A file waiting for the musician to confirm which stem cell it lands in. */
interface PendingFile {
  file: File;
  role: StemRole | "skip";
}

/**
 * Project page — the only place audio enters or leaves the prototype.
 *
 * Unit contract: every memory figure below is MiB (1024²), labelled MiB.
 */
export function ProjectDrawer({ engine, status, control }: Props) {
  const [sess, setSess] = useState<SessionState>(() => session.get());
  const [busy, setBusy] = useState<string | null>(null);
  const [log, setLog] = useState<{ ok: boolean; text: string }[]>([]);
  const [projects, setProjects] = useState<StoredProject[]>([]);
  const [pending, setPending] = useState<PendingFile[] | null>(null);
  const [renaming, setRenaming] = useState(false);
  const [nameDraft, setNameDraft] = useState("");
  const inputs = useRef<Partial<Record<StemRole, HTMLInputElement | null>>>({});
  const allInput = useRef<HTMLInputElement | null>(null);
  const abort = useRef<AbortController | null>(null);

  useEffect(() => {
    const off = session.subscribe(setSess);
    return () => {
      off();
    };
  }, []);
  const refresh = useCallback(async () => {
    setProjects(await projectStore.listProjects());
  }, []);
  useEffect(() => {
    void refresh();
  }, [refresh]);

  const note = (ok: boolean, text: string) => setLog((prev) => [{ ok, text }, ...prev].slice(0, 10));

  const detectGrid = useCallback(async () => {
    setBusy("detecting tempo grid…");
    const grid = await analyzeGridForSession(engine);
    note(!!grid, grid ? `grid — ${describeGrid(grid)}` : "grid — no periodic structure detected");
  }, [engine]);

  const onPick = useCallback(
    async (role: StemRole, file: File | undefined) => {
      if (!file) return;
      setBusy(`ingesting ${role}…`);
      const controller = new AbortController();
      abort.current = controller;
      const r = await ingestStem(engine, role, file, "user-private", { signal: controller.signal });
      note(r.ok, `${ROLE_LABEL[role]} — ${r.detail}`);
      session.set({ source: "upload", saved: false });
      // Tempo is detected from the audio itself on every stem change.
      await detectGrid();
      abort.current = null;
      setBusy(null);
      void refresh();
    },
    [engine, refresh, detectGrid],
  );

  /** Stage 1 of "load all": propose a mapping, do not ingest yet. */
  const stageAll = useCallback((files: FileList | null) => {
    if (!files || files.length === 0) return;
    const picked = Array.from(files).slice(0, STEM_ROLE_LIST.length);
    setPending(
      picked.map((file, i) => ({ file, role: (STEM_ROLE_LIST[i] as StemRole | undefined) ?? "skip" })),
    );
  }, []);

  /** Stage 2: the mapping was visibly confirmed — now decode. */
  const confirmAll = useCallback(async () => {
    if (!pending) return;
    const jobs = pending.filter((p): p is { file: File; role: StemRole } => p.role !== "skip");
    setPending(null);
    if (jobs.length === 0) return;
    setBusy("ingesting stems…");
    const controller = new AbortController();
    abort.current = controller;
    await ingestSequential(
      engine,
      jobs.map(({ role, file }) => ({ role, file, provenance: "user-private" as const })),
      {
        signal: controller.signal,
        onResult: (r) => note(r.ok, `${ROLE_LABEL[r.role]} — ${r.detail}`),
      },
    );
    session.set({ source: "upload", saved: false });
    await detectGrid();
    abort.current = null;
    setBusy(null);
    void refresh();
  }, [engine, pending, refresh, detectGrid]);

  const loadDemo = useCallback(async () => {
    setBusy("downloading demo stems…");
    session.reset();
    engine.resetDecodeCounters();
    const controller = new AbortController();
    abort.current = controller;
    const stems = await buildDemoProject(controller.signal);
    setBusy("decoding demo stems…");
    await ingestSequential(
      engine,
      stems.map((stem) => ({
        role: stem.role,
        file: new File([stem.blob], stem.filename, { type: "audio/wav" }),
        provenance: "bundled-demo" as const,
      })),
      {
        signal: controller.signal,
        onResult: (r) => note(r.ok, `${ROLE_LABEL[r.role]} — ${r.detail}`),
      },
    );
    session.set({ name: DEMO_TITLE, source: "demo" });
    await detectGrid();
    abort.current = null;
    setBusy(null);
    void refresh();
  }, [engine, refresh, detectGrid]);

  const save = useCallback(async () => {
    setBusy("saving project…");
    const backend = await projectStore.blobBackend();
    await projectStore.saveProject(toStoredProject(session.get(), control, backend));
    session.set({ saved: true, savedAt: Date.now() });
    note(true, `saved locally to ${backend}`);
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
        bpm: project.control.grid.bpm ?? 120,
        bpmSource: (project.control.grid.source as SessionState["bpmSource"]) ?? "provisional",
      });
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
      // Restored stems are re-analysed: the grid is derived from audio, not trusted from disk.
      await detectGrid();
      // Amendment 2: a song load stops the transport and waits for PLAY.
      engine.execute({ id: -1, t: performance.now(), type: "song.load", payload: { song: 0 } });
      setBusy(null);
    },
    [engine, detectGrid],
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

  const commitName = useCallback(() => {
    const next = nameDraft.trim();
    setRenaming(false);
    if (!next || next === sess.name) return;
    session.set({ name: next, saved: false });
    note(true, `session renamed to “${next}”`);
  }, [nameDraft, sess.name]);

  const cancel = useCallback(() => {
    abort.current?.abort();
    note(false, "load cancelled — remaining stems not decoded, intermediates dereferenced (GC timing not controlled)");
  }, []);

  const loadedCount = useMemo(
    () => STEM_ROLE_LIST.filter((role) => sess.stems[role]).length,
    [sess.stems],
  );

  return (
    <section className="st-pj" data-testid="project-drawer">
      {/* ---------- project banner ---------- */}
      <header className="st-pj-banner">
        <div className="min-w-0">
          <p className="st-pj-banner__eyebrow">project 01</p>
          {renaming ? (
            <input
              autoFocus
              value={nameDraft}
              data-testid="session-name-input"
              onChange={(e) => setNameDraft(e.target.value)}
              onBlur={commitName}
              onKeyDown={(e) => {
                if (e.key === "Enter") commitName();
                if (e.key === "Escape") setRenaming(false);
              }}
              className="w-full border border-[var(--bench-line)] bg-transparent px-2 py-1 font-mono text-sm text-[var(--ink)]"
            />
          ) : (
            <button
              type="button"
              className="st-pj-banner__name text-left"
              data-testid="session-name"
              title="rename session"
              onClick={() => {
                setNameDraft(sess.name);
                setRenaming(true);
              }}
            >
              {sess.name} <span className="text-[var(--ink-faint)]">✎</span>
            </button>
          )}
        </div>
        <span className="st-pj-banner__state" data-saved={sess.saved}>
          <i aria-hidden />
          {sess.saved ? "saved" : "unsaved"}
        </span>
      </header>

      {/* ---------- stems ---------- */}
      <div className="st-pj-card">
        <p className="st-pj-card__title">stems</p>
        <div className="st-pj-stems">
          {STEM_ROLE_LIST.map((role, i) => {
            const stem = sess.stems[role];
            const track = status.tracks[i];
            const prediction = stem ? predictMonoDownmix(stem.probe.channels ?? 2, stem.decodedBytes) : null;
            return (
              <div key={role} className="st-pj-stem">
                <span className="st-pj-stem__n">{String(i + 1).padStart(2, "0")}</span>
                <div className="st-pj-stem__body">
                  <p className="st-pj-stem__role">{ROLE_LABEL[role]}</p>
                  <p className="st-pj-stem__meta">
                    {stem
                      ? `${stem.filename} · ${formatBytes(stem.fileBytes)} · ${stem.probe.duration?.toFixed(2) ?? "?"}s · ${
                          stem.probe.channels ?? "?"
                        }ch · ${stem.probe.decodedSampleRate ?? "?"} Hz${track?.decoded ? "" : " · not in engine"}`
                      : "empty"}
                  </p>
                  {stem && (
                    <p
                      className="st-pj-stem__meta"
                      data-testid={`stem-memory-${role}`}
                      data-decoded-bytes={stem.decodedBytes}
                      data-decode-count={track?.decodeCount ?? 0}
                      data-buffer-reused={String(track?.bufferReused ?? false)}
                    >
                      decoded {formatMiB(stem.decodedBytes)} · decodes {track?.decodeCount ?? 0}
                      {stem.derived ? " · mono working copy" : ""}
                    </p>
                  )}
                  {stem && prediction?.applicable && !stem.derived && (
                    <button
                      type="button"
                      className="st-link mt-1"
                      data-testid={`memory-saver-${role}`}
                      onClick={() => void applyMemorySaver(role)}
                    >
                      memory saver: mono copy saves {formatMiB(prediction.savedBytes)}
                    </button>
                  )}
                </div>
                <input
                  ref={(el) => {
                    inputs.current[role] = el;
                  }}
                  type="file"
                  accept=".wav,.mp3,.m4a,.aac,.flac,.aif,.aiff,audio/*"
                  className="hidden"
                  onChange={(e) => {
                    const file = e.target.files?.[0];
                    // Reset so re-picking the SAME file still fires `change`.
                    e.target.value = "";
                    void onPick(role, file);
                  }}
                />
                <button type="button" className="st-pj-select" onClick={() => inputs.current[role]?.click()}>
                  select
                </button>
              </div>
            );
          })}
        </div>

        <input
          ref={allInput}
          type="file"
          multiple
          accept=".wav,.mp3,.m4a,.aac,.flac,.aif,.aiff,audio/*"
          className="hidden"
          onChange={(e) => {
            stageAll(e.target.files);
            e.target.value = "";
          }}
        />
        <div className="st-pj-actions">
          <button type="button" className="st-pj-btn" data-testid="load-all" onClick={() => allInput.current?.click()}>
            <span aria-hidden>↑</span> load all
          </button>
          <button type="button" className="st-pj-btn st-pj-btn--accent" onClick={() => void loadDemo()} data-testid="load-demo">
            <span aria-hidden>▷</span> try demo
          </button>
        </div>
      </div>

      {/* ---------- load-all mapping confirmation ---------- */}
      {pending && (
        <div className="st-pj-card" data-testid="load-all-mapping">
          <p className="st-pj-card__title">confirm stem mapping</p>
          <p className="font-mono text-[10px] text-[var(--ink-faint)]">
            check each file lands in the right cell before decoding. nothing is loaded until you confirm.
          </p>
          <div className="mt-2 grid gap-1">
            {pending.map((p, i) => (
              <div key={i} className="flex items-center gap-2">
                <span className="min-w-0 flex-1 truncate font-mono text-[10px] text-[var(--ink)]">{p.file.name}</span>
                <span aria-hidden className="font-mono text-[10px] text-[var(--ink-faint)]">
                  →
                </span>
                <select
                  value={p.role}
                  data-testid={`map-${i}`}
                  className="border border-[var(--bench-line)] bg-transparent px-2 py-1 font-mono text-[10px] text-[var(--ink)]"
                  onChange={(e) => {
                    const role = e.target.value as StemRole | "skip";
                    setPending((prev) =>
                      (prev ?? []).map((row, j) =>
                        j === i
                          ? { ...row, role }
                          : // a role can only be used once
                            row.role === role && role !== "skip"
                            ? { ...row, role: "skip" }
                            : row,
                      ),
                    );
                  }}
                >
                  {STEM_ROLE_LIST.map((role) => (
                    <option key={role} value={role}>
                      {ROLE_LABEL[role]}
                    </option>
                  ))}
                  <option value="skip">do not load</option>
                </select>
              </div>
            ))}
          </div>
          <div className="mt-3 flex gap-2">
            <button type="button" className="st-toggle" data-testid="confirm-mapping" onClick={() => void confirmAll()}>
              confirm &amp; load
            </button>
            <button type="button" className="st-toggle" onClick={() => setPending(null)}>
              cancel
            </button>
          </div>
        </div>
      )}

      {/* ---------- status tiles ---------- */}
      <div className="st-pj-tiles">
        <div className="st-pj-tile" data-testid="bpm-field">
          <p className="st-pj-tile__k">tempo</p>
          <p className="st-pj-tile__v">{sess.songGrid ? `${sess.songGrid.bpm.toFixed(2)} bpm` : "—"}</p>
          <p className="st-pj-tile__sub">
            {sess.songGrid ? `detected · ${describeGrid(sess.songGrid)}` : "awaiting analysis"}
          </p>
        </div>
        <div className="st-pj-tile">
          <p className="st-pj-tile__k">stems loaded</p>
          <p className="st-pj-tile__v">{loadedCount} / 4</p>
          <p className="st-pj-tile__sub" data-retained-bytes={status.decodedBytes}>
            {formatMiB(status.decodedBytes)} decoded
          </p>
        </div>
      </div>

      {/* ---------- save ---------- */}
      <button type="button" className="st-pj-save" onClick={() => void save()}>
        save project
      </button>

      {/* ---------- saved projects ---------- */}
      <div className="st-pj-card">
        <p className="st-pj-card__title">saved projects</p>
        {projects.length === 0 ? (
          <p className="font-mono text-[10px] text-[var(--ink-faint)]">nothing saved on this device yet.</p>
        ) : (
          <div className="st-pj-saved">
            {projects.map((p) => (
              <div key={p.id} className="st-pj-saved__row">
                <span className="st-pj-saved__icon" aria-hidden>
                  <svg width="20" height="20" viewBox="0 0 20 20" fill="none" stroke="currentColor" strokeWidth="1">
                    <circle cx="10" cy="10" r="8" />
                    <circle cx="10" cy="10" r="2.2" />
                    <circle cx="10" cy="5" r="1.2" />
                    <circle cx="14.3" cy="12.5" r="1.2" />
                    <circle cx="5.7" cy="12.5" r="1.2" />
                  </svg>
                </span>
                <div className="min-w-0">
                  <p className="st-pj-saved__name">{p.name}</p>
                  <p className="st-pj-saved__meta">
                    {p.stems.length} stems · {p.control.grid.bpm ?? 120} bpm · v{p.schemaVersion}
                  </p>
                </div>
                <button type="button" className="st-link" onClick={() => void restore(p)}>
                  open
                </button>
                <button
                  type="button"
                  className="st-link"
                  onClick={() => void projectStore.deleteProject(p.id).then(refresh)}
                >
                  delete
                </button>
              </div>
            ))}
          </div>
        )}
      </div>

      <p className="font-mono text-[10px] leading-relaxed text-[var(--ink-faint)]">demo: {DEMO_NOTICE}</p>

      {busy && (
        <div className="flex items-center gap-3">
          <p className="font-mono text-[10px] text-[var(--ink)]">{busy}</p>
          <button type="button" className="st-toggle" data-testid="cancel-load" onClick={cancel}>
            cancel load
          </button>
        </div>
      )}

      {log.length > 0 && (
        <ul className="grid gap-1" data-testid="ingest-log">
          {log.map((l, i) => (
            <li key={i} className="font-mono text-[10px] text-[var(--ink-dim)]">
              <span className={l.ok ? "text-[var(--ink)]" : "text-[var(--signal)]"}>{l.ok ? "ok" : "fail"}</span>{" "}
              {l.text}
            </li>
          ))}
        </ul>
      )}
    </section>
  );
}
