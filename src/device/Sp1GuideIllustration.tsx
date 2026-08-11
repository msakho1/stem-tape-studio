import { memo, useEffect, useMemo, useState } from "react";
import type { Control } from "@/device/geometry";
import svgMarkup from "@/assets/stem-tape-sp1-outline.svg?raw";

/**
 * The single authoritative SP-1 illustration used by every control accordion
 * and the Guide tab. The supplied SVG asset is injected verbatim — nothing is
 * redrawn here. Animation works only by toggling `data-active="true"` on the
 * asset's existing `[data-control]` groups.
 */

export type MiniMotion =
  | "press"
  | "fader"
  | "rocker"
  | "rocker-click"
  | "sequence"
  | "double"
  | "triple"
  | "hold"
  | "fx";

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
const ON_MS = 520; // a single tap is visibly held
const MULTI_ON_MS = 260; // a tap inside a double/triple is quick
const MULTI_GAP_MS = 140; // release between taps in a multi-tap
const SEQ_ON_MS = 480; // one button of a two-button sequence
const SEQ_GAP_MS = 420; // deliberate pause between the two buttons
const SUSTAIN_MS = 2600; // fader travel / rocker deflection / hold
const REST_MS = 1500; // dark rest before the gesture repeats

/**
 * Build the gesture timeline: qualifiers (held controls, e.g. FUNCTION) engage
 * first and stay lit, then the target control pulses once / twice / three
 * times, cycles through its targets one at a time (`sequence`), or stays lit
 * for continuous motions (fader travel, rocker deflection) and holds. Returns
 * the frames plus the total cycle length so slower gestures are not clipped by
 * a fixed period.
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

  if (motion === "fader" || motion === "rocker" || motion === "rocker-click" || motion === "hold") {
    frames.push({ at: start, active: all });
    frames.push({ at: start + SUSTAIN_MS, active: base });
    return { frames, cycle: start + SUSTAIN_MS + REST_MS };
  }

  if (motion === "sequence") {
    let t = start;
    for (const id of target) {
      frames.push({ at: t, active: [...base, id] });
      frames.push({ at: t + SEQ_ON_MS, active: base });
      t += SEQ_ON_MS + SEQ_GAP_MS;
    }
    return { frames, cycle: t + REST_MS };
  }

  // FX: both volume buttons together open the mode, then a pause, then each
  // track button is pressed in turn to show the four banks.
  if (motion === "fx") {
    const vol = target.filter((id) => id.startsWith("volume-"));
    const tracks = target.filter((id) => id.startsWith("track-"));
    let t = start;
    frames.push({ at: t, active: [...base, ...vol] });
    frames.push({ at: t + ON_MS, active: base });
    t += ON_MS + 900;
    for (const id of tracks) {
      frames.push({ at: t, active: [...base, id] });
      frames.push({ at: t + SEQ_ON_MS, active: base });
      t += SEQ_ON_MS + SEQ_GAP_MS;
    }
    return { frames, cycle: t + REST_MS };
  }


  const count = motion === "double" ? 2 : motion === "triple" ? 3 : 1;
  const on = count > 1 ? MULTI_ON_MS : ON_MS;
  const gap = count > 1 ? MULTI_GAP_MS : ON_MS;
  let t = start;
  for (let i = 0; i < count; i++) {
    frames.push({ at: t, active: all });
    frames.push({ at: t + on, active: base });
    t += on + gap;
  }
  return { frames, cycle: t + REST_MS };
}


export const Sp1GuideIllustration = memo(function Sp1GuideIllustration({
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
  const [frameIndex, setFrameIndex] = useState(0);

  // Keyed on content, not array identity: callers pass fresh literals every
  // render, and an identity-based memo would restart the timeline forever.
  const heldKey = held.map(assetControlId).sort().join(",");
  const targetKey = highlight
    .map(assetControlId)
    .filter((id) => !heldKey.split(",").includes(id))
    .join(",");
  const { frames, cycle } = useMemo(
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
      timers.push(window.setTimeout(run, cycle));
    };
    run();
    return () => {
      stopped = true;
      timers.forEach(clearTimeout);
    };
  }, [frames, cycle]);

  /**
   * Activation is expressed as a React-owned attribute on the host, never as a
   * post-render write into the injected SVG. The page re-renders this tree
   * several times a second while the engine runs; imperative `data-active`
   * attributes were being wiped by the very next render, which is what turned
   * every gesture into a sub-frame flash.
   */
  const activeList = (frames[frameIndex]?.active ?? []).join(" ");

  return (
    <div
      className={`sp1-illustration w-full ${className ?? ""}`}
      role="img"
      aria-label="SP-1 control illustration"
      data-active-controls={activeList}
      data-gesture={motion}
      dangerouslySetInnerHTML={{ __html: MARKUP }}
    />

  );
});

