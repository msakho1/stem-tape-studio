/**
 * LED inspector model.
 *
 * Two strictly separate surfaces:
 *   • the TEN physical SP-1 LEDs (`PHYSICAL_LED_IDS`);
 *   • the web-only `play-indicator`, which is never part of a physical frame.
 *
 * The web surface derives a logical frame (`deriveLeds`), but only the pattern
 * name reaches the DOM — every brightness/period value lives in CSS. This
 * module maps logical pattern → diagnostic MODE, records which state owns an
 * LED, and compares logical against what the DOM/CSS actually renders.
 *
 * It never claims parity with physical LEDs: the audited M0 build has no
 * host→device LED path, so nothing here is proof of what the panel shows.
 * The panel has exactly EIGHT LEDs (4 Track + 4 side/status); the `••` marks
 * and the red PLAY triangle are printed artwork, not outputs.
 */

import type { LedFrame, LedId, LedPattern, LedState, SurfaceState } from "@/machine/surface";
import { deriveLeds } from "@/machine/surface";
import {
  M0_IMPLEMENTED_LED_OUTPUTS,
  PHYSICAL_LED_IDS,
  UI_ONLY_INDICATOR_IDS,
  type DivergenceCategory,
  type LedDiagnosticMode,
  type PhysicalLedId,
  type UiOnlyIndicatorId,
} from "./physical";

export type LedMode = LedDiagnosticMode;

/** Everything the web renders: the 8 physical LEDs plus the web-only indicators. */
export const LED_IDS: LedId[] = [...PHYSICAL_LED_IDS, ...UI_ONLY_INDICATOR_IDS] as LedId[];

/** The eight SP-1 panel LEDs (4 Track + 4 side/status) the M0 driver writes. */
export const PANEL_LED_IDS: LedId[] = [...M0_IMPLEMENTED_LED_OUTPUTS] as LedId[];

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
      return "pulse";
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

export const CSS_PERIOD_MS: Record<LedPattern, number | null> = {
  dark: null,
  faint: null,
  solid: null,
  pulse: 1200,
  blink: 400,
  breathe: 2400,
  chase: 550,
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
  /** True for the 10 physical LEDs, false for `play-indicator`. */
  physical: boolean;
  /** Does the audited M0 firmware drive this output at all? */
  m0Driven: boolean;
  /** Expected mode from the behaviour contract / reducer intent. */
  expectedMode: LedMode;
  expectedPattern: LedPattern;
  actualMode: LedMode;
  actualPattern: LedPattern;
  brightness: string;
  periodMs: number | null;
  phaseAnchor: "none" | "css-arbitrary";
  owner: string;
  priority: number;
  /** Highest-priority state that did NOT win this LED. */
  lostTo: string | null;
  source: string;
  animation: "application-driven" | "css-only";
  domClass: string | null;
  mismatch: string | null;
  divergence: DivergenceCategory | null;

  // ---- the four strictly separate columns --------------------------------
  /** 1. Expected contract state. */
  columnExpected: string;
  /** 2. Web LOGICAL LED state (reducer-derived). */
  columnWebLogical: string;
  /** 3. Rendered DOM/CSS state. NEVER labelled physical. */
  columnDom: string;
  /** 4. Last MIDI frame transmitted / firmware-reported value. */
  columnTransmitted: string;
  /**
   * Stays `unknown` unless a human records a physical observation or firmware
   * reports its committed frame. Even that proves PWM command state, not
   * human-visible illumination.
   */
  physicalObservation: "unknown" | "firmware-reported (PWM command, not illumination)" | "human-observed";
}

export interface TransmittedLedView {
  /** Eight MIDI values last transmitted by the host, or null when none. */
  transmitted: number[] | null;
  /** Eight values the firmware reported as committed over CDC, if any. */
  firmwareReported: number[] | null;
  /** Human-recorded physical observations, per physical index. */
  humanObserved?: Partial<Record<number, string>>;
}

const SOURCE: Record<string, string> = {
  track: "src/machine/surface.ts · deriveLeds() track branch",
  side: "src/machine/surface.ts · deriveLeds() side branch",
  play: "src/machine/surface.ts · deriveLeds() play-indicator (web-only)",
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

function inspectOne(
  id: LedId,
  index: number,
  actual: LedFrame,
  expected: LedFrame,
  byId: Map<LedId, DomLedProbe>,
  lostTo: Partial<Record<LedId, string>>,
  tx: TransmittedLedView = { transmitted: null, firmwareReported: null },
): LedInspectionRow {
  const a: LedState = actual[id];
  const e: LedState = expected[id];
  const probe = byId.get(id) ?? null;
  const rendered = patternFromClass(probe?.className ?? null);
  const cssOnly = CSS_ONLY_ANIMATION[a.pattern];
  const problems: string[] = [];
  let divergence: DivergenceCategory | null = null;
  if (a.pattern !== e.pattern) {
    problems.push(`logical ${a.pattern} ≠ expected ${e.pattern} (time-window not re-derived)`);
    divergence = "timing/clock missing";
  }
  if (rendered && rendered !== a.pattern) {
    problems.push(`DOM renders ${rendered}, logical state is ${a.pattern}`);
    divergence = "logical LED state correct but DOM rendering wrong";
  }
  if (cssOnly && (a.pattern === "blink" || a.pattern === "chase") && /flash|reject|confirm/i.test(a.reason)) {
    problems.push("one-shot flash rendered as an infinite CSS animation");
    divergence = "DOM state correct but CSS timing wrong";
  }
  const physical = (PHYSICAL_LED_IDS as readonly string[]).includes(id);
  const m0Driven = (M0_IMPLEMENTED_LED_OUTPUTS as readonly string[]).includes(id);
  const physIndex = PHYSICAL_LED_IDS.indexOf(id as PhysicalLedId);
  const sent = physical && physIndex >= 0 ? (tx.transmitted?.[physIndex] ?? null) : null;
  const reported = physical && physIndex >= 0 ? (tx.firmwareReported?.[physIndex] ?? null) : null;
  const human = physical && physIndex >= 0 ? (tx.humanObserved?.[physIndex] ?? null) : null;
  return {
    columnExpected: `${modeOf(e.pattern)} (${e.pattern})`,
    columnWebLogical: `${modeOf(a.pattern)} (${a.pattern}) · owner ${a.reason}`,
    columnDom: rendered ? `class ${rendered}${probe?.animationName ? ` · anim ${probe.animationName}` : ""}` : "not rendered / not probed",
    columnTransmitted: physical
      ? `${sent === null ? "not transmitted" : `midi ${sent}`}${reported === null ? "" : ` · firmware committed ${reported}`}`
      : "web-only indicator — never transmitted",
    physicalObservation: human
      ? "human-observed"
      : reported !== null
        ? "firmware-reported (PWM command, not illumination)"
        : "unknown",
    id,
    index,
    physical,
    m0Driven,
    expectedMode: modeOf(e.pattern),
    expectedPattern: e.pattern,
    actualMode: modeOf(a.pattern),
    actualPattern: a.pattern,
    brightness: CORE_OPACITY[a.pattern],
    periodMs: CSS_PERIOD_MS[a.pattern],
    phaseAnchor: cssOnly ? "css-arbitrary" : "none",
    owner: a.reason,
    priority: a.priority,
    lostTo: lostTo[id] ?? null,
    source: sourceOf(id),
    animation: cssOnly ? "css-only" : "application-driven",
    domClass: probe?.className ?? null,
    mismatch: problems.length ? problems.join("; ") : null,
    divergence,
  };
}

/** All rendered indicators (physical + web-only). */
export function inspectLeds(
  actual: LedFrame,
  expected: LedFrame,
  dom: DomLedProbe[],
  lostTo: Partial<Record<LedId, string>> = {},
): LedInspectionRow[] {
  const byId = new Map(dom.map((d) => [d.id, d]));
  return LED_IDS.map((id, index) => inspectOne(id, index, actual, expected, byId, lostTo));
}

/** EXACTLY the eight physical SP-1 LEDs, in physical order. */
export function inspectPhysicalLeds(
  actual: LedFrame,
  expected: LedFrame,
  dom: DomLedProbe[],
  lostTo: Partial<Record<LedId, string>> = {},
  tx: TransmittedLedView = { transmitted: null, firmwareReported: null },
): LedInspectionRow[] {
  const byId = new Map(dom.map((d) => [d.id, d]));
  return PHYSICAL_LED_IDS.map((id, index) =>
    inspectOne(id as LedId, index, actual, expected, byId, lostTo, tx),
  );
}

/** The web-only indicators — never presented as physical SP-1 LEDs. */
export function inspectWebOnlyIndicators(
  actual: LedFrame,
  expected: LedFrame,
  dom: DomLedProbe[],
): LedInspectionRow[] {
  const byId = new Map(dom.map((d) => [d.id, d]));
  return UI_ONLY_INDICATOR_IDS.map((id, index) =>
    inspectOne(id as LedId, index, actual, expected, byId, {}),
  );
}

export type { PhysicalLedId, UiOnlyIndicatorId };
