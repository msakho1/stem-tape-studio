import { useEffect, useMemo, useRef, useState } from "react";
import type { Control } from "@/device/geometry";
import svgMarkup from "@/assets/stem-tape-sp1-outline.svg?raw";

/**
 * The single authoritative SP-1 illustration used by every control accordion
 * and the Guide tab. The supplied SVG asset is injected verbatim — nothing is
 * redrawn here. Animation works only by toggling `data-active="true"` on the
 * asset's existing `[data-control]` groups.
 */

export type MiniMotion = "press" | "fader" | "rocker" | "double" | "triple" | "hold";

/** Map internal control ids onto the asset's data-control ids. */
export function assetControlId(c: Control): string {
  if (c === "rocker-fwd" || c === "rocker-rwd") return "rocker";
  if (c.startsWith("track-button-")) return `track-${c.slice("track-button-".length)}`;
  return c;
}

const MARKUP = svgMarkup.replace(/<\?xml[^>]*\?>/, "");

interface Frame {
  /** milliseconds from the start of the cycle */
  at: number;
  active: string[];
}

/**
 * Gesture cadence, in milliseconds. A tap must read as a deliberate press and
 * release, not a strobe: the control stays lit long enough to be seen (ON),
 * clears fully between taps (GAP), and the whole gesture is followed by a rest
 * so the eye can count the taps before the cycle repeats.
 */
const LEAD_MS = 700; // qualifier (FUNCTION etc.) engages first
const ON_MS = 520; // a tap is visibly held
const GAP_MS = 380; // release between taps in a multi-tap
const SUSTAIN_MS = 2600; // fader travel / rocker deflection / hold
const REST_MS = 1500; // dark rest before the gesture repeats

/**
 * Build the gesture timeline: qualifiers (held controls, e.g. FUNCTION) engage
 * first and stay lit, then the target control pulses once / twice / three
 * times, or stays lit for continuous motions (fader travel, rocker deflection)
 * and holds. Returns the frames plus the total cycle length so slower gestures
 * are not clipped by a fixed period.
 */
function buildFrames(
  target: string[],
  held: string[],
  motion: MiniMotion,
): { frames: Frame[]; cycle: number } {
  const base = held;
  const all = [...held, ...target.filter((t) => !held.includes(t))];
  const frames: Frame[] = [{ at: 0, active: [] }];
  const lead = held.length > 0 ? LEAD_MS : 0;
  if (lead) frames.push({ at: 200, active: base });

  const start = lead + 200;

  if (motion === "fader" || motion === "rocker" || motion === "hold") {
    frames.push({ at: start, active: all });
    frames.push({ at: start + SUSTAIN_MS, active: base });
    return { frames, cycle: start + SUSTAIN_MS + REST_MS };
  }

  const count = motion === "double" ? 2 : motion === "triple" ? 3 : 1;
  let t = start;
  for (let i = 0; i < count; i++) {
    frames.push({ at: t, active: all });
    frames.push({ at: t + ON_MS, active: base });
    t += ON_MS + GAP_MS;
  }
  return { frames, cycle: t + REST_MS };
}


export function Sp1GuideIllustration({
  highlight = [],
  motion = "press",
  held = [],
  className,
}: {
  highlight?: Control[];
  motion?: MiniMotion;
  held?: Control[];
  className?: string;
}) {
  const host = useRef<HTMLDivElement>(null);
  const [frameIndex, setFrameIndex] = useState(0);

  // Keyed on content, not array identity: callers pass fresh literals every
  // render, and an identity-based memo would restart the timeline forever.
  const heldKey = held.map(assetControlId).sort().join(",");
  const targetKey = highlight
    .map(assetControlId)
    .filter((id) => !heldKey.split(",").includes(id))
    .join(",");
  const frames = useMemo(
    () =>
      buildFrames(
        targetKey ? targetKey.split(",") : [],
        heldKey ? heldKey.split(",") : [],
        motion,
      ),
    [targetKey, heldKey, motion],
  );


  useEffect(() => {
    setFrameIndex(0);
    let stopped = false;
    const timers: number[] = [];
    const run = () => {
      if (stopped) return;
      frames.forEach((f, i) => {
        timers.push(window.setTimeout(() => !stopped && setFrameIndex(i), f.at));
      });
      timers.push(window.setTimeout(run, CYCLE_MS));
    };
    run();
    return () => {
      stopped = true;
      timers.forEach(clearTimeout);
    };
  }, [frames]);

  useEffect(() => {
    const root = host.current?.querySelector("svg");
    if (!root) return;
    const active = new Set(frames[frameIndex]?.active ?? []);
    root.querySelectorAll<SVGGElement>("[data-control]").forEach((g) => {
      const id = g.getAttribute("data-control") ?? "";
      if (active.has(id)) g.setAttribute("data-active", "true");
      else g.removeAttribute("data-active");
    });
  }, [frameIndex, frames]);

  return (
    <div
      ref={host}
      className={`sp1-illustration w-full ${className ?? ""}`}
      role="img"
      aria-label="SP-1 control illustration"
      dangerouslySetInnerHTML={{ __html: MARKUP }}
    />
  );
}
