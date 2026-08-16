/**
 * LED inspector model.
 *
 * The web surface derives an 11-LED logical frame (`deriveLeds`), but only the
 * pattern name reaches the DOM — every brightness/period value lives in CSS.
 * This module maps logical pattern → diagnostic MODE, records which state owns
 * an LED and which competing state lost precedence, and compares the logical
 * frame against what the DOM/CSS actually renders.
 *
 * It never claims parity with physical LEDs: M0 has no host→device LED path.
 */

import type { LedFrame, LedId, LedPattern, LedState, SurfaceState } from "@/machine/surface";
import { deriveLeds } from "@/machine/surface";

export type LedMode = "off" | "dim" | "solid" | "blink" | "breathe" | "one-shot flash" | "chase";

export const LED_IDS: LedId[] = [
  "track-led-1",
  "track-led-2",
  "track-led-3",
  "track-led-4",
  "side-led-1",
  "side-led-2",
  "side-led-3",
  "side-led-4",
  "play-indicator",
  "function-led-1",
  "function-led-2",
];

/** The eight surface LEDs the SP-1 panel exposes (tracks + side row). */
export const PANEL_LED_IDS: LedId[] = LED_IDS.slice(0, 8);

export function modeOf(pattern: LedPattern): LedMode {
  switch (pattern) {
    case "dark":
      return "off";
    case "faint":
      return "dim";
    case "solid":
      return "solid";
    case "blink":
      return "blink";
    case "breathe":
      return "breathe";
    case "pulse":
      return "breathe";
    case "chase":
      return "chase";
  }
}

/** CSS brightness of the core element, from src/styles.css. */
export const CORE_OPACITY: Record<LedPattern, string> = {
  dark: "0.08",
  faint: "0.32",
  solid: "1.00",
  pulse: "0.34→1.00 @1.2s",
  blink: "1.00/0.12 @0.4s",
  breathe: "0.22↔0.95 @2.4s",
  chase: "0.20→1.00 @0.55s",
};

/** Which patterns are animated by CSS keyframes with no application clock. */
export const CSS_ONLY_ANIMATION: Record<LedPattern, boolean> = {
  dark: false,
  faint: false,
  solid: false,
  pulse: true,
  blink: true,
  breathe: true,
  chase: true,
};

export interface LedInspectionRow {
  id: LedId;
  index: number;
  /** Expected mode from the behaviour contract / reducer intent. */
  expectedMode: LedMode;
  expectedPattern: LedPattern;
  actualMode: LedMode;
  actualPattern: LedPattern;
  brightness: string;
  owner: string;
  priority: number;
  /** Highest-priority state that did NOT win this LED. */
  lostTo: string | null;
  source: string;
  animation: "application-driven" | "css-only";
  domClass: string | null;
  mismatch: string | null;
}

const SOURCE: Record<string, string> = {
  track: "src/machine/surface.ts · deriveLeds() track branch",
  side: "src/machine/surface.ts · deriveLeds() side branch",
  play: "src/machine/surface.ts · deriveLeds() play-indicator",
  function: "src/machine/surface.ts · deriveLeds() function-led",
};

function sourceOf(id: LedId): string {
  if (id.startsWith("track-led")) return SOURCE["track"]!;
  if (id.startsWith("side-led")) return SOURCE["side"]!;
  if (id === "play-indicator") return SOURCE["play"]!;
  return SOURCE["function"]!;
}

/**
 * Expected pattern derived from the same reducer state but with the
 * time-dependent flash windows treated as EXPIRED. Any difference exposes the
 * known "flash never expires without a re-render" divergence.
 */
export function expectedFrame(state: SurfaceState, now: number): LedFrame {
  return deriveLeds(state, now);
}

export interface DomLedProbe {
  id: LedId;
  className: string | null;
  animationName: string | null;
  opacity: string | null;
}

/** Reads the rendered LED classes/computed styles. Browser only. */
export function probeDom(): DomLedProbe[] {
  if (typeof document === "undefined") return [];
  return LED_IDS.map((id) => {
    const el = document.querySelector<SVGGElement>(`[data-led="${id}"]`);
    const core = el?.querySelector<SVGCircleElement | SVGPathElement>(".st-led__core") ?? null;
    const style = el && core && typeof window !== "undefined" ? window.getComputedStyle(core) : null;
    return {
      id,
      className: el?.getAttribute("class") ?? null,
      animationName: style?.animationName ?? null,
      opacity: style?.opacity ?? null,
    };
  });
}

function patternFromClass(cls: string | null): LedPattern | null {
  if (!cls) return null;
  const m = /st-led--(dark|faint|solid|pulse|blink|breathe|chase)/.exec(cls);
  return (m?.[1] as LedPattern) ?? null;
}

export function inspectLeds(
  actual: LedFrame,
  expected: LedFrame,
  dom: DomLedProbe[],
  lostTo: Partial<Record<LedId, string>> = {},
): LedInspectionRow[] {
  const byId = new Map(dom.map((d) => [d.id, d]));
  return LED_IDS.map((id, index) => {
    const a: LedState = actual[id];
    const e: LedState = expected[id];
    const probe = byId.get(id) ?? null;
    const rendered = patternFromClass(probe?.className ?? null);
    const cssOnly = CSS_ONLY_ANIMATION[a.pattern];
    const problems: string[] = [];
    if (a.pattern !== e.pattern) {
      problems.push(`logical ${a.pattern} ≠ expected ${e.pattern} (time-window not re-derived)`);
    }
    if (rendered && rendered !== a.pattern) {
      problems.push(`DOM renders ${rendered}, logical state is ${a.pattern}`);
    }
    if (cssOnly && (a.pattern === "blink" || a.pattern === "chase") && /flash/i.test(a.reason)) {
      problems.push("one-shot flash rendered as an infinite CSS animation");
    }
    return {
      id,
      index,
      expectedMode: modeOf(e.pattern),
      expectedPattern: e.pattern,
      actualMode: modeOf(a.pattern),
      actualPattern: a.pattern,
      brightness: CORE_OPACITY[a.pattern],
      owner: a.reason,
      priority: a.priority,
      lostTo: lostTo[id] ?? null,
      source: sourceOf(id),
      animation: cssOnly ? "css-only" : "application-driven",
      domClass: probe?.className ?? null,
      mismatch: problems.length ? problems.join("; ") : null,
    };
  });
}
