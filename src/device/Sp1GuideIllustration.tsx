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
  /** milliseconds from the start of the 3 s cycle */
  at: number;
  active: string[];
}

const CYCLE_MS = 3000;

/**
 * Build the gesture timeline: qualifiers (held controls, e.g. FUNCTION) engage
 * first and stay lit, then the target control pulses once / twice / three
 * times, or stays lit for continuous motions (fader travel, rocker deflection)
 * and holds.
 */
function buildFrames(target: string[], held: string[], motion: MiniMotion): Frame[] {
  const base = held;
  const all = [...held, ...target.filter((t) => !held.includes(t))];
  const frames: Frame[] = [{ at: 0, active: [] }];
  const lead = held.length > 0 ? 500 : 0;
  if (lead) frames.push({ at: 220, active: base });

  if (motion === "fader" || motion === "rocker" || motion === "hold") {
    frames.push({ at: lead + 220, active: all });
    frames.push({ at: 2500, active: base });
    return frames;
  }

  const count = motion === "double" ? 2 : motion === "triple" ? 3 : 1;
  let t = lead + 220;
  for (let i = 0; i < count; i++) {
    frames.push({ at: t, active: all });
    frames.push({ at: t + 220, active: base });
    t += 400;
  }
  return frames;
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

  const heldIds = useMemo(() => held.map(assetControlId), [held]);
  const targetIds = useMemo(
    () => highlight.map(assetControlId).filter((id) => !heldIds.includes(id)),
    [highlight, heldIds],
  );
  const frames = useMemo(() => buildFrames(targetIds, heldIds, motion), [targetIds, heldIds, motion]);

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
