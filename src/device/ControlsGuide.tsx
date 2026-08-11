import { useState } from "react";
import type { Control } from "@/device/geometry";
import { Sp1GuideIllustration, type MiniMotion } from "@/device/Sp1GuideIllustration";

/**
 * The control reference reuses the authoritative SP-1 asset illustration for
 * every lesson; there is no hand-drawn diagram in this file.
 */
export type { MiniMotion };



interface Lesson {
  id: string;
  title: string;
  gesture: string;
  body: string;
  highlight: Control[];
  motion: MiniMotion;
  /** Controls held down for the whole gesture (drawn solid, never blinking). */
  held?: Control[];
}

/** The essential moves, in the order a performer learns them. */
export const LESSONS: Lesson[] = [
  {
    id: "play",
    title: "How to start and stop",
    gesture: "PLAY",
    body: "Press PLAY once. The tape spins up over its inertia ramp instead of snapping to speed. Press again to wind down and park.",
    highlight: ["play"],
    motion: "press",
  },
  {
    id: "mute",
    title: "How to mute a stem",
    gesture: "Track 1–4 · single tap",
    body: "One tap on a Track button mutes that stem, another unmutes it. Single, double and triple taps are mutually exclusive — a double tap never flashes the mute first.",
    highlight: ["track-button-1"],
    motion: "press",
  },
  {
    id: "audition",
    title: "How to solo momentarily",
    gesture: "Track 1–4 · hold",
    body: "Hold a Track button to hear only that stem for as long as you hold it. Hold two and you hear both. Release and the previous balance returns.",
    highlight: ["track-button-2"],
    motion: "hold",
  },
  {
    id: "loop",
    title: "How to grab a one-bar loop",
    gesture: "Track 1–4 · double tap",
    body: "Double-tap a Track button to capture a one-bar loop on that stem, starting at the detected grid. Double-tap again to release it back to the song.",
    highlight: ["track-button-3"],
    motion: "double",
  },
  {
    id: "scrub-lane",
    title: "How to scrub one stem",
    gesture: "FUNCTION + Fader 1–4",
    body: "Hold FUNCTION and move a fader: that stem scrubs audibly under your finger. Release at the sound you want — the stem parks there. Then double-tap its Track button to capture a bar from that point.",
    highlight: ["function", "fader-1"],
    motion: "fader",
    held: ["function"],
  },
  {
    id: "scrub-all",
    title: "How to shuttle the whole song",
    gesture: "FUNCTION + rocker",
    body: "Hold FUNCTION and push the rocker to shuttle all four stems together, forwards or backwards. Release and playback lands cleanly at the new position.",
    highlight: ["function", "rocker-fwd", "rocker-rwd"],
    motion: "rocker",
    held: ["function"],
  },
  {
    id: "reverse",
    title: "How to reverse a stem",
    gesture: "FUNCTION + Track · double tap",
    body: "Hold FUNCTION and double-tap a Track button to play that stem backwards — with or without a loop. Its hidden song position keeps moving, so turning reverse off rejoins the song where it now is.",
    highlight: ["function", "track-button-4"],
    motion: "double",
    held: ["function"],
  },
  {
    id: "resize",
    title: "How to resize a loop",
    gesture: "FUNCTION + Track + VOL −/+",
    body: "Hold FUNCTION and the Track button, then press VOL − or VOL + to halve or double that lane's loop length.",
    highlight: ["function", "track-button-1", "volume-minus", "volume-plus"],
    motion: "press",
    held: ["function", "track-button-1"],
  },
  {
    id: "varispeed",
    title: "How to change tape speed",
    gesture: "Rocker",
    body: "Push the rocker without FUNCTION for varispeed: the tape drags or runs fast, pitch and all. Double-tap it to snap back to 1.00×.",
    highlight: ["rocker-fwd", "rocker-rwd"],
    motion: "rocker",
  },
  {
    id: "fx",
    title: "How to use FX",
    gesture: "VOL − + VOL + together",
    body: "Press both volume buttons together to open FX mode. Each Track button is a bank — TONE, MOD, MOTION, SPACE — held for momentary, FUNCTION + Track to latch. VOL −/+ cycles the algorithm inside the selected bank.",
    highlight: ["volume-minus", "volume-plus"],
    motion: "press",
  },
  {
    id: "heads",
    title: "How to use Heads mode",
    gesture: "FUNCTION + PLAY · triple tap",
    body: "Hold FUNCTION and triple-tap PLAY. Each stem gets its own tape head that plays on its own clock — even while the transport is paused. Tap to mute, hold to audition, triple-tap to latch a head playing, FUNCTION + fader to scrub it. Leaving Heads discards head-only loops and reverses and rejoins the song where it now is.",
    highlight: ["function", "play"],
    motion: "triple",
    held: ["function"],
  },
];

/**
 * The control reference on the tape page: a stack of drawers, each one a
 * gesture with an animated example. Details can be collapsed entirely so the
 * surface shows only its hit zones.
 */
export function ControlsGuide({
  showHitZones,
  onToggleHitZones,
}: {
  showHitZones: boolean;
  onToggleHitZones: () => void;
}) {
  const [open, setOpen] = useState<string | null>("play");
  const [details, setDetails] = useState(true);

  return (
    <div className="mt-4 border-t border-[var(--bench-line)] pt-3" data-testid="controls-guide">
      <div className="flex flex-wrap items-center gap-2">
        <p className="font-mono text-[10px] uppercase tracking-[0.18em] text-[var(--ink-faint)]">controls</p>
        <button type="button" className="st-toggle" data-on={details} onClick={() => setDetails((v) => !v)}>
          {details ? "hide details" : "show details"}
        </button>
        <button type="button" className="st-toggle" data-on={showHitZones} onClick={onToggleHitZones}>
          {showHitZones ? "hide hit zones" : "show hit zones"}
        </button>
      </div>

      {details && (
        <div className="mt-3 grid gap-1">
          {LESSONS.map((l) => {
            const isOpen = open === l.id;
            return (
              <div key={l.id} className="border border-[var(--bench-line)]">
                <button
                  type="button"
                  className="flex w-full items-center justify-between gap-3 px-3 py-2 text-left"
                  aria-expanded={isOpen}
                  data-testid={`lesson-${l.id}`}
                  onClick={() => setOpen(isOpen ? null : l.id)}
                >
                  <span className="font-mono text-[12px] text-[var(--ink)]">{l.title}</span>
                  <span className="shrink-0 font-mono text-[10px] uppercase tracking-[0.12em] text-[var(--ink-faint)]">
                    {l.gesture} <span aria-hidden>{isOpen ? "⌃" : "⌄"}</span>
                  </span>
                </button>
                {isOpen && (
                  <div className="grid gap-2 border-t border-[var(--bench-line)] px-3 py-3 md:grid-cols-[1fr_240px]">
                    <p className="font-mono text-[11px] leading-relaxed text-[var(--ink-dim)]">{l.body}</p>
                    <Sp1GuideIllustration highlight={l.highlight} motion={l.motion} held={l.held ?? []} />
                  </div>
                )}
              </div>
            );
          })}
        </div>
      )}
    </div>
  );
}
