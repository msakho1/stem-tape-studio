/**
 * PHYSICAL SP-1 MODEL.
 *
 * The real SP-1 panel carries exactly TEN physical LEDs. The website renders
 * those ten plus one web-only transport indicator (`play-indicator`), which is
 * NOT part of the physical frame and must never be silently inserted into it.
 *
 * The audited Stem Tape M0 diagnostic firmware directly drives only EIGHT of
 * the ten (four Track + four side/playback). The two Function/status outputs
 * are unresolved in that source: no GPIO is aliased, guessed, or reused here.
 *
 * This module is declarative only.
 */

export type PhysicalLedId =
  | "track-led-1"
  | "track-led-2"
  | "track-led-3"
  | "track-led-4"
  | "side-led-1"
  | "side-led-2"
  | "side-led-3"
  | "side-led-4"
  | "function-led-1"
  | "function-led-2";

export type UiOnlyIndicatorId = "play-indicator";

export const PHYSICAL_LED_IDS: readonly PhysicalLedId[] = [
  "track-led-1",
  "track-led-2",
  "track-led-3",
  "track-led-4",
  "side-led-1",
  "side-led-2",
  "side-led-3",
  "side-led-4",
  "function-led-1",
  "function-led-2",
] as const;

export const UI_ONLY_INDICATOR_IDS: readonly UiOnlyIndicatorId[] = ["play-indicator"] as const;

export const PHYSICAL_LED_COUNT = 10;

export function isPhysicalLedId(id: string): id is PhysicalLedId {
  return (PHYSICAL_LED_IDS as readonly string[]).includes(id);
}

/** Outputs the audited M0 LED driver actually writes. */
export const M0_IMPLEMENTED_LED_OUTPUTS: readonly PhysicalLedId[] = [
  "track-led-1",
  "track-led-2",
  "track-led-3",
  "track-led-4",
  "side-led-1",
  "side-led-2",
  "side-led-3",
  "side-led-4",
] as const;

/** Physical outputs with no resolved electrical control in the audited source. */
export const M0_UNRESOLVED_LED_OUTPUTS: readonly PhysicalLedId[] = [
  "function-led-1",
  "function-led-2",
] as const;

export const M0_LED_COVERAGE = {
  physicalLeds: PHYSICAL_LED_COUNT,
  webIndicators: `${PHYSICAL_LED_COUNT} physical + ${UI_ONLY_INDICATOR_IDS.length} web-only`,
  m0Implemented: M0_IMPLEMENTED_LED_OUTPUTS.length,
  m0Unresolved: M0_UNRESOLVED_LED_OUTPUTS.slice(),
  hostToDeviceLedFeedback: "unsupported",
  note: "function-led-1 / function-led-2 have no resolved GPIO in the audited M0 source; no pin is guessed or reused",
} as const;

export type Capability = "supported" | "unsupported" | "present" | "unresolved";

/** Honest capability table for the audited M0 build. */
export const M0_CAPABILITIES: Record<string, Capability | string> = {
  "physical control input": "supported",
  "four physical faders": "supported",
  "battery CC telemetry": "supported",
  "CDC diagnostics": "supported",
  "direct binary-hash verification": "unsupported",
  "host→device physical LED feedback": "unsupported",
  "physical LED count": String(PHYSICAL_LED_COUNT),
  "M0 LED-driver outputs implemented": `${M0_IMPLEMENTED_LED_OUTPUTS.length} of ${PHYSICAL_LED_COUNT}`,
  "unresolved M0 function/status LED outputs": String(M0_UNRESOLVED_LED_OUTPUTS.length),
  "unmeasured resistor-ladder chords": "present",
};

/** Diagnostic LED modes. One-shots are distinct from infinite animations. */
export type LedDiagnosticMode =
  | "off"
  | "dim"
  | "solid"
  | "pulse"
  | "blink"
  | "breathe"
  | "chase"
  | "one-shot single flash"
  | "one-shot double flash";

export interface ExpectedLed {
  mode: LedDiagnosticMode;
  /** 0..1 nominal brightness, or a range string for animated modes. */
  brightness: string;
  /** Animation period in ms, or null for static modes. */
  periodMs: number | null;
  /** What the animation phase is anchored to. */
  phaseAnchor: "none" | "loop-wrap" | "beat" | "gesture-start" | "css-arbitrary";
  /** One-shot duration in ms; null for continuous modes. */
  durationMs: number | null;
  direction?: "forward" | "reverse" | "none";
}

export type ExpectedPhysicalLedFrame = Record<PhysicalLedId, ExpectedLed>;

export const LED_OFF: ExpectedLed = {
  mode: "off",
  brightness: "0.00",
  periodMs: null,
  phaseAnchor: "none",
  durationMs: null,
};

/** Builds a COMPLETE 10-LED expected frame; unspecified LEDs are explicitly off. */
export function expectedPhysicalFrame(
  partial: Partial<Record<PhysicalLedId, ExpectedLed>>,
): ExpectedPhysicalLedFrame {
  const out = {} as ExpectedPhysicalLedFrame;
  for (const id of PHYSICAL_LED_IDS) out[id] = partial[id] ?? LED_OFF;
  return out;
}

export const led = (
  mode: LedDiagnosticMode,
  opts: Partial<Omit<ExpectedLed, "mode">> = {},
): ExpectedLed => ({
  mode,
  brightness: opts.brightness ?? (mode === "solid" ? "1.00" : mode === "dim" ? "0.32" : "0.00→1.00"),
  periodMs: opts.periodMs ?? null,
  phaseAnchor: opts.phaseAnchor ?? "none",
  durationMs: opts.durationMs ?? null,
  ...(opts.direction ? { direction: opts.direction } : {}),
});

/** Where a reproduction first diverged. */
export type DivergenceCategory =
  | "firmware emitted no control"
  | "firmware reported an unmeasured ladder value"
  | "browser received no MIDI"
  | "SP-1 recognition rejected the port"
  | "decoder rejected the bytes"
  | "timestamp repair marked it stale"
  | "baseline suppression consumed it"
  | "held-state tracking disagreed"
  | "gesture arbitration selected the wrong owner"
  | "correct surface command never emitted"
  | "correct command emitted but engine rejected it"
  | "engine accepted but state did not reconcile"
  | "audio state changed but LED derivation was wrong"
  | "logical LED state correct but DOM rendering wrong"
  | "DOM state correct but CSS timing wrong"
  | "physical mapping unresolved"
  | "expected contract missing"
  | "state not available"
  | "priority arbitration wrong"
  | "timing/clock missing";

/** How a result was obtained. Mocked input NEVER means hardware verification. */
export type ObservationKind = "mocked" | "browser-observed" | "physically-observed" | "not-observed";
