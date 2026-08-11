import { useState } from "react";
import { FADER_X, FADER_SLOT_Y, FADER_SLOT_H, HIT_ZONES, type Control } from "@/device/geometry";

/**
 * Compact SP-1 diagram used inside the control drawers. It renders the same
 * geometry as the instrument, highlights the controls a lesson uses and
 * animates the gesture (press pulse, fader travel, rocker deflection) so the
 * musician can see the move rather than read about it.
 */
export type MiniMotion = "press" | "fader" | "rocker" | "double" | "triple" | "hold";

/**
 * Gesture cadence for the red callout, expressed on a fixed 3 s cycle so the
 * blink COUNT reads as the tap count: one flash = single tap, two = double
 * tap, three = triple tap, sustained = hold.
 */
const CYCLE = "3s";
function pulse(motion: MiniMotion): { values: string; keyTimes: string } {
  // one flash occupies 0.18 s (on 0.09 s, off 0.09 s) inside the 3 s cycle
  const flash = (n: number) => {
    const v: string[] = ["0"];
    const k: number[] = [0];
    for (let i = 0; i < n; i++) {
      const t = 0.02 + i * 0.09;
      v.push("1", "1", "0");
      k.push(t, t + 0.03, t + 0.05);
    }
    v.push("0");
    k.push(1);
    return { values: v.join(";"), keyTimes: k.map((x) => x.toFixed(3)).join(";") };
  };
  if (motion === "double") return flash(2);
  if (motion === "triple") return flash(3);
  if (motion === "hold") return { values: "0;1;1;0;0", keyTimes: "0;0.030;0.560;0.600;1" };
  return flash(1);
}

export function MiniSurface({
  highlight,
  motion = "press",
  held = [],
}: {
  highlight: Control[];
  motion?: MiniMotion;
  /** Controls shown as continuously engaged (e.g. FUNCTION during a chord). */
  held?: Control[];
}) {
  const on = (c: Control) => highlight.includes(c);
  const isHeld = (c: Control) => held.includes(c);
  const zones = HIT_ZONES.filter((z) => on(z.control));
  const { values, keyTimes } = pulse(motion);
  const line = "var(--ink)";

  return (
    <svg viewBox="40 40 640 700" className="h-40 w-full" role="img" aria-label="SP-1 control diagram">
      <rect x="52" y="52" width="616" height="676" rx="26" fill="none" stroke={line} strokeWidth="8" />
      {/* faders */}
      {FADER_X.map((x, i) => (
        <g key={i}>
          <rect x={x - 4} y={FADER_SLOT_Y} width="8" height={FADER_SLOT_H} rx="4" fill="none" stroke={line} strokeWidth="6" />
          <circle
            cx={x}
            cy={FADER_SLOT_Y + FADER_SLOT_H * 0.45}
            r="13"
            fill={on(`fader-${i + 1}` as Control) ? "var(--signal)" : "none"}
            stroke={line}
            strokeWidth="6"
          >
            {motion === "fader" && on(`fader-${i + 1}` as Control) && (
              <animate
                attributeName="cy"
                values={`${FADER_SLOT_Y + 16};${FADER_SLOT_Y + FADER_SLOT_H - 16};${FADER_SLOT_Y + 16}`}
                dur="2.4s"
                repeatCount="indefinite"
              />
            )}
          </circle>
        </g>
      ))}
      {/* track buttons */}
      {FADER_X.map((x, i) => (
        <circle
          key={`t${i}`}
          cx={x}
          cy={634.5}
          r="17"
          fill={isHeld(`track-button-${i + 1}` as Control) ? "var(--signal)" : "none"}
          stroke={line}
          strokeWidth="6"
        />
      ))}
      {/* volume buttons */}
      {[
        { c: "volume-minus" as Control, x: 176.4 },
        { c: "volume-plus" as Control, x: 276 },
      ].map((b) => (
        <circle key={b.c} cx={b.x} cy={78.5} r="15" fill={isHeld(b.c) ? "var(--signal)" : "none"} stroke={line} strokeWidth="6" />
      ))}
      {/* rocker */}
      <g>
        <rect
          x={58}
          y={160}
          width="46"
          height="130"
          rx="22"
          fill={on("rocker-fwd") || on("rocker-rwd") ? "var(--signal)" : "none"}
          stroke={line}
          strokeWidth="6"
        >
          {motion === "rocker" && (
            <animateTransform
              attributeName="transform"
              type="rotate"
              values="-6 81 225; 6 81 225; -6 81 225"
              dur="1.8s"
              repeatCount="indefinite"
            />
          )}
        </rect>
      </g>
      {/* play + function rails */}
      <rect x={612} y={180} width="50" height="114" rx="24" fill={isHeld("play") ? "var(--signal)" : "none"} stroke={line} strokeWidth="6" />
      <rect
        x={612}
        y={646}
        width="50"
        height="114"
        rx="24"
        fill={isHeld("function") ? "var(--signal)" : "none"}
        stroke={line}
        strokeWidth="2"
      />
      {/* gesture callout: filled red flash whose blink count = the tap count */}
      {zones.map((z, i) => {
        const sustained = isHeld(z.control) || motion === "fader" || motion === "rocker";
        return (
          <rect
            key={`z${i}`}
            x={z.x}
            y={z.y}
            width={z.width}
            height={z.height}
            rx="10"
            fill="var(--signal)"
            fillOpacity="0.85"
            stroke="var(--signal)"
            strokeWidth="2"
            opacity={sustained ? 0.85 : 0}
          >
            {!sustained && (
              <animate
                attributeName="opacity"
                values={values}
                keyTimes={keyTimes}
                dur={CYCLE}
                calcMode="linear"
                repeatCount="indefinite"
              />
            )}
          </rect>
        );
      })}
    </svg>
  );
}


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
                  <div className="grid gap-2 border-t border-[var(--bench-line)] px-3 py-3 md:grid-cols-[1fr_180px]">
                    <p className="font-mono text-[11px] leading-relaxed text-[var(--ink-dim)]">{l.body}</p>
                    <MiniSurface highlight={l.highlight} motion={l.motion} held={l.held ?? []} />
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
