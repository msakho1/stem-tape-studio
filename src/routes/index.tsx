import { createFileRoute } from "@tanstack/react-router";
import { useEffect, useState } from "react";
import { DeviceSurface } from "@/device/DeviceSurface";
import { DiagnosticPanel } from "@/device/DiagnosticPanel";
import { KEY_HINTS, useDeviceSurface } from "@/device/useDeviceSurface";
import { CONTROL_LABELS } from "@/device/geometry";
import { ProjectDrawer } from "@/audio/ProjectDrawer";
import { WorkletPanel } from "@/audio/WorkletPanel";
import { useAudioEngine } from "@/audio/useAudioEngine";
import { formatBytes } from "@/audio/format";

export const Route = createFileRoute("/")({
  component: LabPage,
  head: () => ({
    meta: [
      { title: "Stem Tape — SP-1 Interaction Prototype (Unofficial R&D)" },
      {
        name: "description",
        content:
          "Unofficial R&D bench: an interactive digital twin of the SP-1 control surface with gesture recognition, LED arbitration and a live diagnostic log.",
      },
      { property: "og:title", content: "Stem Tape — SP-1 Interaction Prototype" },
      {
        property: "og:description",
        content:
          "Interaction-first prototype of a four-track stem/tape control surface: pointer and keyboard gestures, chords, holds, LED state arbitration.",
      },
      { property: "og:type", content: "website" },
      { name: "twitter:card", content: "summary_large_image" },
    ],
  }),
});

function LabPage() {
  const [showHitZones, setShowHitZones] = useState(false);
  // The AudioContext only exists in the browser: render the locked label until
  // after hydration so SSR and the first client paint agree.
  const [mounted, setMounted] = useState(false);
  useEffect(() => setMounted(true), []);
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

  return (
    <main className="min-h-screen px-5 py-6 md:px-10 md:py-9">
      <header className="mb-7 flex flex-wrap items-end justify-between gap-4 border-b border-[var(--bench-line)] pb-5">
        <div>
          <p className="font-mono text-[10px] uppercase tracking-[0.28em] text-[var(--ink-faint)]">
            unofficial · independent r&amp;d · not affiliated with teenage engineering
          </p>
          <h1 className="mt-2 font-mono text-lg tracking-tight text-[var(--ink)] md:text-2xl">
            Stem Tape — interaction prototype
          </h1>
          <p className="mt-1 max-w-2xl font-mono text-[11px] leading-relaxed text-[var(--ink-dim)]">
            Phase 4: the v2.6 surface now drives a real Web Audio engine. Load four stems (or generate the
            local demo), press PLAY and every mapped gesture — mute, delete, rate, restart, song load — is sent
            to the engine as an ordered command and answered with an ack.

          </p>
        </div>
        <button
          type="button"
          className="st-toggle"
          data-on={mounted && status.contextState === "running"}
          data-testid="unlock-audio"
          onClick={() => void unlock()}
        >
          {mounted && status.contextState === "running" ? `audio live · ${status.sampleRate ?? "?"} Hz` : "enable audio"}
        </button>
        <button
          type="button"
          className="st-toggle"
          data-on={showHitZones}
          onClick={() => setShowHitZones((v) => !v)}
        >
          {showHitZones ? "hide hit zones" : "show hit zones"}
        </button>
      </header>

      <div className="grid gap-8 lg:grid-cols-[minmax(0,1fr)_360px]">
        <section className="flex flex-col items-center">
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
          <div className="mt-5 w-full max-w-xl">
            <p className="font-mono text-[10px] uppercase tracking-[0.16em] text-[var(--ink-faint)]">
              keyboard parity
            </p>
            <div className="mt-2 flex flex-wrap gap-x-4 gap-y-1 font-mono text-[11px] text-[var(--ink-dim)]">
              {KEY_HINTS.map(([key, control]) => (
                <span key={control}>
                  <span className="text-[var(--ink)]">{key.toLowerCase()}</span> → {CONTROL_LABELS[control]}
                </span>
              ))}
            </div>
            <p className="mt-3 font-mono text-[11px] leading-relaxed text-[var(--ink-faint)]">
              Hold two controls to emit a chord. Taps fire optimistically and revise upward
              (×1 → ×2 → ×3) so a single tap is never delayed by the multi-tap window.
            </p>
          </div>
        </section>

        <aside>
          <DiagnosticPanel
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
          />

          <section className="st-card mt-4" data-testid="audio-diagnostics">
            <h2 className="font-mono text-[11px] uppercase tracking-[0.2em] text-[var(--ink)]">audio engine</h2>
            <dl className="mt-2 grid grid-cols-2 gap-x-3 gap-y-1 font-mono text-[10px] text-[var(--ink-dim)]">
              <dt>context</dt>
              <dd className="text-[var(--ink)]" data-testid="ctx-state">{status.contextState}</dd>
              <dt>requested / actual</dt>
              <dd className="text-[var(--ink)]">
                {String(status.requestedPlaying)} / {String(status.actuallyPlaying)}
              </dd>
              <dt>position</dt>
              <dd>{status.position.toFixed(3)}s of {status.duration.toFixed(3)}s</dd>
              <dt>rate</dt>
              <dd>{status.rate.toFixed(4)}×</dd>
              <dt>start spread</dt>
              <dd data-testid="start-spread">{status.startSpreadMs.toFixed(4)} ms</dd>
              <dt>decoded</dt>
              <dd>{formatBytes(status.decodedBytes)}</dd>
              <dt>last decode</dt>
              <dd>{status.lastDecodeMs != null ? `${status.lastDecodeMs.toFixed(0)} ms` : "—"}</dd>
              <dt>last error</dt>
              <dd>{status.lastError ?? "none"}</dd>
            </dl>
            <p className="mt-2 font-mono text-[10px] text-[var(--ink-faint)]">{unlockNote}</p>
            <ul className="mt-2 grid gap-1" data-testid="ack-log">
              {acks.slice(0, 10).map((a) => (
                <li key={a.id} className="font-mono text-[10px] text-[var(--ink-dim)]">
                  <span className="text-[var(--ink)]">#{a.id} {a.type}</span> · {a.status} · {a.detail}
                </li>
              ))}
            </ul>
          </section>

          <div className="mt-4">
            <ProjectDrawer engine={engine} status={status} control={control} />
            <WorkletPanel engine={engine} status={status} />
          </div>

        </aside>
      </div>
    </main>
  );
}
