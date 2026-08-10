import { createFileRoute } from "@tanstack/react-router";
import { useCallback, useEffect, useMemo, useState } from "react";
import { DeviceSurface } from "@/device/DeviceSurface";
import { KeyboardPanel } from "@/device/KeyboardPanel";
import { SystemPage } from "@/device/SystemPage";
import { KEY_HINTS, useDeviceSurface } from "@/device/useDeviceSurface";
import { CONTROL_LABELS } from "@/device/geometry";
import { ProjectDrawer } from "@/audio/ProjectDrawer";
import { Waveform } from "@/components/tape/Waveform";
import { useAudioEngine } from "@/audio/useAudioEngine";
import { formatBytes } from "@/audio/format";
import { loadDemoProject } from "@/audio/loadDemo";
import { session, type SessionState } from "@/audio/session";

export const Route = createFileRoute("/")({
  component: LabPage,
  head: () => ({
    meta: [
      { title: "Stem Tape — Four-Track Tape Looper for the Browser" },
      {
        name: "description",
        content:
          "Load four stems, press the rendered SP-1 controls and hear them: loops, chops, varispeed, reverse and filters running on a real Web Audio engine, entirely on your device.",
      },
      { property: "og:title", content: "Stem Tape — Four-Track Tape Looper" },
      {
        property: "og:description",
        content:
          "An unofficial browser digital twin of the SP-1: four stems, four tape loops, gesture-driven performance. No audio ever leaves your device.",
      },
      { property: "og:type", content: "website" },
      { name: "twitter:card", content: "summary_large_image" },
    ],
  }),
});

type Tab = "tape" | "projects" | "guide" | "system";
const TABS: { id: Tab; label: string }[] = [
  { id: "tape", label: "tape" },
  { id: "projects", label: "projects" },
  { id: "guide", label: "guide" },
  { id: "system", label: "system" },
];

const TRACK_ROLES = ["vocals", "drums", "bass", "instruments"] as const;

function clock(s: number) {
  if (!Number.isFinite(s) || s < 0) s = 0;
  const m = Math.floor(s / 60);
  const sec = Math.floor(s % 60);
  return `${m}:${sec.toString().padStart(2, "0")}`;
}

function LabPage() {
  const [tab, setTab] = useState<Tab>("tape");
  const [showHitZones, setShowHitZones] = useState(false);
  const [mounted, setMounted] = useState(false);
  const [sess, setSess] = useState<SessionState>(() => session.get());
  const [demoBusy, setDemoBusy] = useState(false);
  useEffect(() => setMounted(true), []);
  useEffect(() => {
    const off = session.subscribe(setSess);
    return () => {
      off();
    };
  }, []);

  const {
    state,
    leds,
    observed,
    ready,
    powerHoldMs,
    setPowerHoldMs,
    svgRef,
    capRefs,
    faderValuesRef,
    rawLog,
    gestureLog,
    handlers,
    arbiter,
    heldKeys,
    globalScrub,
  } = useDeviceSurface();

  const { engine, status, acks, unlock, unlockNote } = useAudioEngine(state.commands);
  const control = {
    faders: state.tracks.map((t) => t.volume),
    mutes: state.tracks.map((t) => t.content === "muted"),
    masterVolume: state.masterVolume,
    speed: state.speed,
    chopDiv: state.chopDiv,
    window: state.window,
    filter: { mode: state.filter.mode, amount: state.filter.amount },
    grid: { bpm: state.grid.bpm, source: state.grid.source },
    song: state.song,
  };

  const active = state.activeTrack;
  const activeRole = TRACK_ROLES[active] ?? "vocals";
  const activeStem = sess.stems[activeRole];
  const activeTrackStatus = status.tracks[active];
  const buffer = mounted ? engine.getBuffer(active as 0 | 1 | 2 | 3) : null;
  const progress = status.duration > 0 ? status.position / status.duration : 0;
  const loopWindow = useMemo(
    () => (state.window.end > state.window.start ? { start: state.window.start, end: state.window.end } : null),
    [state.window.start, state.window.end],
  );

  const tryDemo = useCallback(async () => {
    setDemoBusy(true);
    await unlock();
    await loadDemoProject(engine);
    setDemoBusy(false);
  }, [engine, unlock]);

  const lastGesture = gestureLog[0]?.text ?? "Nothing yet — press a control on the SP-1.";
  const loaded = TRACK_ROLES.filter((r) => sess.stems[r] && !sess.stems[r]!.trashed);

  return (
    <div className="min-h-screen pb-16 lg:pb-0">
      {/* ---------- top bar ---------- */}
      <header className="border-b border-[var(--bench-line)]">
        <p className="px-4 pt-3 font-mono text-[9px] uppercase tracking-[0.24em] text-[var(--ink-faint)] md:px-8 md:text-[10px]">
          unofficial · independent r&amp;d · not affiliated with teenage engineering
        </p>
        <div className="flex flex-wrap items-center gap-x-8 gap-y-3 px-4 pb-4 pt-2 md:px-8">
          <div className="flex items-center gap-3">
            <svg width="34" height="34" viewBox="0 0 34 34" aria-hidden className="text-[var(--ink)]">
              <path d="M17 5 L30 28 H4 Z" fill="none" stroke="currentColor" strokeWidth="1.2" />
              <path d="M17 13 L23 24 H11 Z" fill="none" stroke="currentColor" strokeWidth="1" opacity="0.6" />
            </svg>
            <div>
              <h1 className="font-mono text-xl tracking-tight text-[var(--ink)]">Stem Tape</h1>
              <p className="font-mono text-[11px] text-[var(--ink-dim)]">
                A four-track tape looper for the browser
              </p>
            </div>
          </div>

          <nav className="order-3 hidden gap-6 lg:order-none lg:flex" aria-label="Sections">
            {TABS.map((t) => (
              <button key={t.id} type="button" className="st-tab" data-on={tab === t.id} onClick={() => setTab(t.id)}>
                {t.label}
              </button>
            ))}
          </nav>

          <div className="ml-auto flex flex-1 gap-3 lg:flex-none">
            <button type="button" className="st-cta flex-1" onClick={() => setTab("projects")}>
              ↑ load stems
            </button>
            <button
              type="button"
              className="st-cta flex-1"
              data-testid="try-demo"
              disabled={demoBusy}
              onClick={() => void tryDemo()}
            >
              ▷ {demoBusy ? "loading…" : "try demo"}
            </button>
          </div>
        </div>
      </header>

      {tab === "tape" && (
        <div className="grid gap-0 lg:grid-cols-[minmax(0,1fr)_360px_300px]">
          {/* ---------- instrument ---------- */}
          <section className="border-b border-[var(--bench-line)] px-4 py-5 md:px-8 lg:border-b-0 lg:border-r">
            <div className="mb-4 flex items-start justify-between gap-4">
              <p className="max-w-sm font-mono text-[12px] leading-relaxed text-[var(--ink-dim)]">
                The SP-1 is the interface. Drag its faders, press its buttons, and combine gestures — audio responds
                in real time.
              </p>
              <div className="flex shrink-0 flex-col gap-2">
                <button
                  type="button"
                  className="st-toggle"
                  data-on={mounted && status.contextState === "running"}
                  data-testid="unlock-audio"
                  onClick={() => void unlock()}
                >
                  {mounted && status.contextState === "running" ? `audio live · ${status.sampleRate ?? "?"} Hz` : "enable audio"}
                </button>
                <button type="button" className="st-toggle" data-on={showHitZones} onClick={() => setShowHitZones((v) => !v)}>
                  {showHitZones ? "hide controls" : "show controls"}
                </button>
              </div>
            </div>
            <DeviceSurface
              svgRef={svgRef}
              capRefs={capRefs}
              faderValues={state.tracks.map((t) => t.volume)}
              pressed={state.pressed}
              leds={leds}
              showHitZones={showHitZones}
              lights={state.lights}
              {...handlers}
            />
            <div className="hidden lg:block">
              <KeyboardPanel
                heldKeys={heldKeys}
                fxOverlay={state.perf.fxOverlay}
                headsMode={state.headsMode}
                globalScrub={globalScrub}
              />
            </div>
          </section>

          {/* ---------- live readout ---------- */}
          <section className="border-b border-[var(--bench-line)] px-4 py-5 md:px-8 lg:border-b-0 lg:border-r lg:px-5">
            <p className="font-mono text-[10px] uppercase tracking-[0.24em] text-[var(--signal)]">live</p>
            <h2 className="mt-1 font-mono text-xl tracking-tight text-[var(--ink)]" data-testid="live-track">
              track <span className="text-[var(--signal)]">{active + 1}</span> ·{" "}
              <span className="uppercase">{activeRole}</span>
            </h2>
            <p className="mt-1 font-mono text-[10px] uppercase tracking-[0.14em] text-[var(--ink-dim)]">
              loop 1/{state.chopDiv} · filter {state.filter.mode === "off" ? "bypass" : state.filter.mode} ·{" "}
              {state.window.reverse ? "reverse" : "forward"}
            </p>

            <ul className="mt-4 grid gap-2" data-testid="track-list">
              {TRACK_ROLES.map((role, i) => {
                const t = status.tracks[i];
                const muted = state.tracks[i]?.content === "muted";
                return (
                  <li key={role}>
                    <div className="st-track" data-on={i === active} data-muted={muted}>
                      <span className="st-track__n">{i + 1}</span>
                      <span className="st-track__name">{sess.stems[role]?.filename ?? role}</span>
                      <span className="st-track__v">
                        {muted ? "muted" : `${Math.round((state.tracks[i]?.volume ?? 0) * 100)}%`}
                      </span>
                      <span className="st-track__tag">{t?.decoded ? (i === active ? "loop" : "") : "empty"}</span>
                    </div>
                  </li>
                );
              })}
            </ul>

            <div className="mt-4">
              <TapeDeck
                buffers={laneBuffers}
                activeIndex={active}
                position={status.position}
                duration={status.duration}
                bpm={sess.bpm}
                grid={sess.songGrid}
                rate={status.rate}
                playing={status.actuallyPlaying}
                slices={state.chopDiv}
                loop={loopWindow}
              />
            </div>

            <div className="mt-5 border-t border-[var(--bench-line)] pt-3">
              <p className="font-mono text-[10px] uppercase tracking-[0.18em] text-[var(--ink-faint)]">
                what just happened?
              </p>
              {lastFxLabel && (
                <p className="mt-1 font-mono text-[12px] uppercase tracking-[0.14em] text-[var(--signal)]" data-testid="what-happened-fx">
                  fx · {lastFxLabel}
                </p>
              )}
              <p className="mt-1 font-mono text-[12px] text-[var(--ink)]" data-testid="what-happened">
                {lastFxDetail ?? lastGesture}
              </p>
              {activeStem && (
                <p className="mt-1 font-mono text-[10px] text-[var(--ink-faint)]">
                  {activeStem.filename} · {formatBytes(activeTrackStatus?.decodedBytes ?? 0)} decoded
                </p>
              )}
            </div>
          </section>

          {/* ---------- project rail ---------- */}
          <aside className="px-4 py-5 md:px-8 lg:px-5">
            <p className="font-mono text-[10px] uppercase tracking-[0.24em] text-[var(--ink-faint)]">project</p>
            <p className="mt-1 font-mono text-[13px] uppercase text-[var(--ink)]">{sess.name}</p>
            <p className="mt-1 font-mono text-[10px] uppercase tracking-[0.14em] text-[var(--ink-dim)]">
              {sess.saved ? "saved locally" : loaded.length ? "unsaved — local only" : "no stems loaded"}
            </p>
            <ul className="mt-4 grid gap-2 border-t border-[var(--bench-line)] pt-3">
              {TRACK_ROLES.map((role) => (
                <li key={role} className="flex items-baseline justify-between gap-3 font-mono text-[11px]">
                  <span className="truncate text-[var(--ink)]">{sess.stems[role]?.filename ?? `${role} —`}</span>
                  <button type="button" className="st-link" onClick={() => setTab("projects")}>
                    {sess.stems[role] ? "replace" : "load"}
                  </button>
                </li>
              ))}
            </ul>
            <button type="button" className="st-link mt-4" onClick={() => setTab("projects")}>
              manage project
            </button>
            <p className="mt-6 font-mono text-[10px] leading-relaxed text-[var(--ink-faint)]">
              No network request in this app ever contains your audio. Everything decodes and stays on this device.
            </p>
          </aside>
        </div>
      )}

      {tab === "projects" && (
        <div className="px-4 py-5 md:px-8">
          <ProjectDrawer engine={engine} status={status} control={control} />
        </div>
      )}

      {tab === "guide" && (
        <div className="grid gap-5 px-4 py-5 md:px-8 lg:grid-cols-2">
          <section className="st-section">
            <p className="st-section__title">keyboard parity</p>
            <div className="flex flex-wrap gap-x-4 gap-y-1 font-mono text-[11px] text-[var(--ink-dim)]">
              {KEY_HINTS.map(([key, ctrl]) => (
                <span key={ctrl}>
                  <span className="text-[var(--ink)]">{key.toLowerCase()}</span> → {CONTROL_LABELS[ctrl]}
                </span>
              ))}
            </div>
            <p className="mt-3 font-mono text-[11px] leading-relaxed text-[var(--ink-faint)]">
              Hold two controls to emit a chord. Taps fire optimistically and revise upward (×1 → ×2 → ×3) so a single
              tap is never delayed by the multi-tap window.
            </p>
          </section>
          <section className="st-section">
            <p className="st-section__title">first moves</p>
            <ol className="grid gap-2 font-mono text-[11px] text-[var(--ink-dim)]">
              <li>1 · press TRY DEMO, then PLAY on the device.</li>
              <li>2 · drag a fader to ride a stem's level.</li>
              <li>3 · tap a track button to mute or unmute it.</li>
              <li>4 · hold FUNCTION + a track for bank / song navigation.</li>
              <li>5 · use the rocker for varispeed; double-tap to snap back.</li>
            </ol>
          </section>
        </div>
      )}

      {tab === "system" && (
        <div className="px-4 py-5 md:px-8">
          <SystemPage
            engine={engine}
            status={status}
            acks={acks}
            unlockNote={unlockNote}
            state={state}
            leds={leds}
            observed={observed}
            ready={ready}
            powerHoldMs={powerHoldMs}
            setPowerHoldMs={setPowerHoldMs}
            rawLog={rawLog}
            gestureLog={gestureLog}
            faderValuesRef={faderValuesRef}
            svgRef={svgRef}
            arbiter={arbiter}
            audio={status}
          />
        </div>
      )}


      {/* ---------- transport bar ---------- */}
      <div className="hidden border-t border-[var(--bench-line)] px-8 py-3 lg:flex lg:items-center lg:gap-6">
        <span className="font-mono text-[12px] tabular-nums text-[var(--ink)]">
          {clock(status.position)} / {clock(status.duration)}
        </span>
        <div className="min-w-0 flex-1">
          <Waveform buffer={buffer} progress={progress} slices={state.chopDiv} loop={loopWindow} height={40} />
        </div>
        <span className="font-mono text-[12px] text-[var(--ink-dim)]">{sess.bpm} BPM</span>
        <span className="font-mono text-[12px] tabular-nums text-[var(--ink)]">{status.rate.toFixed(2)}×</span>
      </div>

      {/* ---------- mobile tab bar ---------- */}
      <nav className="fixed inset-x-0 bottom-0 z-20 grid grid-cols-4 border-t border-[var(--bench-line)] bg-[var(--bench)] lg:hidden" aria-label="Sections">
        {TABS.map((t) => (
          <button key={t.id} type="button" className="st-tab st-tab--bar" data-on={tab === t.id} onClick={() => setTab(t.id)}>
            {t.label}
          </button>
        ))}
      </nav>
    </div>
  );
}
