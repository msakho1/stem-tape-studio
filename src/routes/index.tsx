import { createFileRoute } from "@tanstack/react-router";
import { useState } from "react";
import { DeviceSurface } from "@/device/DeviceSurface";
import { DiagnosticPanel } from "@/device/DiagnosticPanel";
import { KEY_HINTS, useDeviceSurface } from "@/device/useDeviceSurface";
import { CONTROL_LABELS } from "@/device/geometry";

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
  const { state, leds, svgRef, capRefs, faderValuesRef, rawLog, gestureLog, handlers } = useDeviceSurface();

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
            Phase 2: control surface and input model only. No audio engine is running. Every gesture is
            recognised, logged and arbitrated so the mapping can be validated before a single sample is
            loaded.
          </p>
        </div>
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
            rawLog={rawLog}
            gestureLog={gestureLog}
            faderValuesRef={faderValuesRef}
          />
        </aside>
      </div>
    </main>
  );
}
