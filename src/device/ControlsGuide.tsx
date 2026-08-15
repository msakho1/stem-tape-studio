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
    motion: "sequence",
    held: ["function", "track-button-1"],
  },
  {
    id: "varispeed",
    title: "How to change tape speed",
    gesture: "Rocker",
    body: "Push the rocker without FUNCTION for varispeed: the tape drags or runs fast, pitch and all. Double-tap it to snap back to 1.00×.",
    highlight: ["rocker-fwd", "rocker-rwd"],
    motion: "rocker-click",
  },
  {
    id: "fx",
    title: "How to use FX",
    gesture: "VOL − + VOL + together",
    body: "Press both volume buttons together to open FX mode. Each Track button is a bank — TONE, MOD, MOTION, SPACE — held for momentary, FUNCTION + Track to latch. VOL −/+ cycles the algorithm inside the selected bank.",
    highlight: [
      "volume-minus",
      "volume-plus",
      "track-button-1",
      "track-button-2",
      "track-button-3",
      "track-button-4",
    ],
    motion: "fx",
  },
  {
    id: "global-loop",
    title: "How to loop the whole song",
    gesture: "Hold PLAY (while playing)",
    body: "Hold PLAY while the song runs and all four stems drop into one shared bar-locked loop. Release PLAY and the song continues from where it would have been. Nothing drifts: it is one window, not four.",
    highlight: ["play"],
    motion: "hold",
  },
  {
    id: "global-loop-latch",
    title: "How to keep the global loop running",
    gesture: "Hold PLAY, then tap FUNCTION",
    body: "While the global loop is held, tap FUNCTION to latch it so you can let PLAY go. Tap FUNCTION again, or press PLAY, to drop back into the song.",
    highlight: ["play", "function"],
    motion: "sequence",
    held: ["play"],
  },
  {
    id: "global-loop-move",
    title: "How to move the global loop",
    gesture: "Hold PLAY + rocker",
    body: "With the global loop held, push the rocker to step the loop window forward or back one division at a time. The transport never toggles — PLAY is owned by the loop.",
    highlight: ["play", "rocker-fwd", "rocker-rwd"],
    motion: "rocker",
    held: ["play"],
  },
  {
    id: "global-loop-division",
    title: "How to change the loop length",
    gesture: "FUNCTION + VOL −/+",
    body: "Hold FUNCTION and press VOL − or VOL + to set the global loop division: one bar, half, quarter, eighth. Change it while the loop runs and the window resizes from the same start.",
    highlight: ["function", "volume-minus", "volume-plus"],
    motion: "sequence",
    held: ["function"],
  },
  {
    id: "solo",
    title: "How to latch a solo",
    gesture: "PLAY + Track (release under 0.7 s)",
    body: "Press PLAY, then a Track button within 0.45 s, and let go: that stem latches solo. The chord takes ownership of PLAY, so no transport and no global loop can fire from the same press.",
    highlight: ["play", "track-button-1"],
    motion: "sequence",
    held: ["play"],
  },
  {
    id: "link",
    title: "How to link stems",
    gesture: "PLAY + Track (hold past 0.7 s)",
    body: "Same chord, held longer: keep PLAY and the Track button down past 0.7 s to link or unlink that stem. Linked stems take tape moves as a group, phase-continuous — nothing restarts.",
    highlight: ["play", "track-button-2"],
    motion: "hold",
    held: ["play"],
  },
  {
    id: "select",
    title: "How to choose the active stem",
    gesture: "FUNCTION tap, then a Track button",
    body: "With no global loop running, a FUNCTION tap arms selection for a moment. The next Track button becomes the active stem — it only selects, it will not mute, loop or audition.",
    highlight: ["function", "track-button-3"],
    motion: "sequence",
  },
  {
    id: "halfspeed",
    title: "How to drop to half speed",
    gesture: "FUNCTION + PLAY · single tap",
    body: "Hold FUNCTION and tap PLAY once to fall to 0.5×, again to return to 1.00×. Double-tap snaps the speed exactly to 1.000× from anywhere.",
    highlight: ["function", "play"],
    motion: "press",
    held: ["function"],
  },
  {
    id: "cue",
    title: "How to cue and skip while stopped",
    gesture: "Hold PLAY stopped · rocker stopped",
    body: "Hold PLAY while the tape is parked to cue every stem to the top. With the tape stopped the bare rocker skips songs instead of changing speed.",
    highlight: ["play", "rocker-fwd", "rocker-rwd"],
    motion: "sequence",
  },
  {
    id: "heads",
    title: "How to use Heads mode",
    gesture: "FUNCTION + PLAY · triple tap",
    body: "Hold FUNCTION and triple-tap PLAY. Four independent heads read the SAME stem — the one you last selected — at four positions in the song. Track 1–4 mute their head, hold to audition, triple-tap to latch it playing, FUNCTION + fader scrubs that head, and the fader alone is its level. Leaving Heads restores the mix exactly as it was.",
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
