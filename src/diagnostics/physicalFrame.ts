/**
 * Resolved PHYSICAL LED frame.
 *
 * Exactly eight indices, in physical order:
 *   0..3 Track 1..4, 4..7 Side/status 1..4.
 *
 * `play-indicator`, `function-led-1` and `function-led-2` are web-only
 * illustrations and can never enter this frame.
 *
 * This module only CONVERTS the authoritative resolved logical frame produced
 * by `deriveLeds`. It never reinterprets transport, loop, scrub or FX state.
 */

import type { LedFrame, LedId, LedPattern } from "@/machine/surface";
import { PHYSICAL_LED_IDS, PHYSICAL_LED_COUNT, type PhysicalLedId } from "./physical";

/** Nominal MIDI brightness (0..127) per logical pattern. */
export const PATTERN_BRIGHTNESS: Record<LedPattern, number> = {
  dark: 0,
  faint: 40,
  solid: 127,
  pulse: 110,
  blink: 127,
  breathe: 96,
  chase: 118,
};

export const PATTERN_PERIOD_MS: Record<LedPattern, number | null> = {
  dark: null,
  faint: null,
  solid: null,
  pulse: 1200,
  blink: 400,
  breathe: 2400,
  chase: 550,
};

export const ANIMATED_PATTERNS: Record<LedPattern, boolean> = {
  dark: false,
  faint: false,
  solid: false,
  pulse: true,
  blink: true,
  breathe: true,
  chase: true,
};

export interface ResolvedPhysicalLed {
  index: number;
  id: PhysicalLedId;
  pattern: LedPattern;
  /** 0..127 MIDI value for the physical sink. */
  value: number;
  owner: string;
  priority: number;
  periodMs: number | null;
  animated: boolean;
}

export interface ResolvedPhysicalFrame {
  leds: ResolvedPhysicalLed[];
  /** Eight MIDI values, index-ordered. */
  values: number[];
  /** Stable semantic signature — animation sampling never changes it. */
  signature: string;
}

const SHORT: string[] = ["T1", "T2", "T3", "T4", "S1", "S2", "S3", "S4"];

export function resolvePhysicalFrame(frame: LedFrame): ResolvedPhysicalFrame {
  const leds = PHYSICAL_LED_IDS.map((id, index) => {
    const s = frame[id as LedId];
    return {
      index,
      id,
      pattern: s.pattern,
      value: PATTERN_BRIGHTNESS[s.pattern],
      owner: s.reason,
      priority: s.priority,
      periodMs: PATTERN_PERIOD_MS[s.pattern],
      animated: ANIMATED_PATTERNS[s.pattern],
    } satisfies ResolvedPhysicalLed;
  });
  return {
    leds,
    values: leds.map((l) => l.value),
    signature: leds.map((l) => `${l.pattern}:${l.value}:${l.owner}:${l.priority}:${l.periodMs ?? "-"}`).join("|"),
  };
}

/** `[T1 0, T2 0, T3 127, T4 0 | S1 127, S2 0, S3 0, S4 0] owner=fx.momentary` */
export function formatPhysicalFrame(resolved: ResolvedPhysicalFrame): string {
  const cell = (i: number) => `${SHORT[i]} ${resolved.values[i] ?? 0}`;
  const tracks = [0, 1, 2, 3].map(cell).join(", ");
  const side = [4, 5, 6, 7].map(cell).join(", ");
  const top = [...resolved.leds].sort((a, b) => b.priority - a.priority)[0];
  return `[${tracks} | ${side}] owner=${top?.owner ?? "none"}`;
}

export { PHYSICAL_LED_COUNT };
