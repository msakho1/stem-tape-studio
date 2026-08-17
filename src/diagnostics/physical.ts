/**
 * PHYSICAL SP-1 MODEL.
 *
 * The real SP-1 panel carries exactly EIGHT MCU-controlled LEDs:
 * four Track LEDs and a four-LED side/status row shared by battery, playback
 * and status indication.
 *
 * The `••` marks on the FUNCTION button and the red PLAY triangle are STATIC
 * PRINTED ARTWORK. They are not LEDs. There is no physical `function-led-1`,
 * no `function-led-2`, and no separate physical play indicator. The web build
 * renders additional logical indicators (`play-indicator`, `function-led-1/2`)
 * purely as web-interface status; they must never enter a physical frame.
 *
 * The audited Stem Tape M0 diagnostic firmware drives all EIGHT physical
 * outputs — electrical coverage is 8/8. That is separate from Stem Tape
 * BEHAVIOUR coverage (partial) and from host→device LED feedback, which the
 * currently audited M0 build does not implement. Host feedback is reported as
 * a runtime capability, not a permanent hardcoded "never": the firmware is
 * being updated separately and a later versioned handshake may advertise it.
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
  | "side-led-4";

/** Rendered by the website only; never part of the physical SP-1 frame. */
export type UiOnlyIndicatorId = "play-indicator" | "function-led-1" | "function-led-2";

export const PHYSICAL_LED_IDS: readonly PhysicalLedId[] = [
  "track-led-1",
  "track-led-2",
  "track-led-3",
  "track-led-4",
  "side-led-1",
  "side-led-2",
  "side-led-3",
  "side-led-4",
] as const;

export const UI_ONLY_INDICATOR_IDS: readonly UiOnlyIndicatorId[] = [
  "play-indicator",
  "function-led-1",
  "function-led-2",
] as const;

export const PHYSICAL_LED_COUNT = 8;

/** The side row is multifunctional: battery, playback and status share it. */
export const SIDE_ROW_NOTE =
  "side-led-1..4 are one multifunctional battery / playback / status row; stock playback reuses one of these four and its physical index is UNVERIFIED";

export function isPhysicalLedId(id: string): id is PhysicalLedId {
  return (PHYSICAL_LED_IDS as readonly string[]).includes(id);
}

/** Outputs the audited M0 LED driver actually writes: all eight. */
export const M0_IMPLEMENTED_LED_OUTPUTS: readonly PhysicalLedId[] = [...PHYSICAL_LED_IDS] as const;

/**
 * Runtime host→device LED capability.
 *
 * The CURRENT audited profile (M0 v1.1.2) implements LED protocol v1, but the
 * capability of the CONNECTED device is only ever established by the CC91
 * handshake — never by this constant.
 */
export type HostLedFeedback =
  | "unsupported-by-audited-build"
  | "expected-protocol-v1-pending-handshake"
  | "advertised"
  | "unknown";

export const M0_HOST_LED_FEEDBACK: HostLedFeedback = "expected-protocol-v1-pending-handshake";

export const M0_LED_COVERAGE = {
  physicalLeds: PHYSICAL_LED_COUNT,
  layout: "4 Track LEDs + 4 side/status LEDs",
  sideRow: SIDE_ROW_NOTE,
  webIndicators: `${PHYSICAL_LED_COUNT} physical + ${UI_ONLY_INDICATOR_IDS.length} web-only (non-physical)`,
  /** ELECTRICAL coverage of physical outputs by the audited M0 driver. */
  electricalCoverage: `${M0_IMPLEMENTED_LED_OUTPUTS.length}/${PHYSICAL_LED_COUNT}`,
  m0Implemented: M0_IMPLEMENTED_LED_OUTPUTS.length,
  /** Stem Tape BEHAVIOUR mapped onto those outputs. */
  behaviorCoverage: "partial",
  hostToDeviceLedFeedback: M0_HOST_LED_FEEDBACK,
  note: "the `••` FUNCTION marks and the red PLAY triangle are static printed artwork, not LEDs",
} as const;

export type Capability = "supported" | "unsupported" | "present" | "unresolved";

/** Honest capability table for the audited M0 build. */
export const M0_CAPABILITIES: Record<string, Capability | string> = {
  "physical control input": "supported",
  "four physical faders": "supported",
  "battery CC telemetry": "supported",
  "CDC diagnostics": "supported",
  "direct binary-hash verification": "unsupported",
  "physical LED count": String(PHYSICAL_LED_COUNT),
  "physical LED GPIO coverage (electrical)": `${M0_IMPLEMENTED_LED_OUTPUTS.length}/${PHYSICAL_LED_COUNT}`,
  "Stem Tape behaviour mapping onto physical LEDs": "partial",
  "host→device physical LED feedback": M0_HOST_LED_FEEDBACK,
  "side-row playback index": "unverified",
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
  /** True when the physical LED index for this behaviour is not established. */
  indexUnverified?: boolean;
}

export type ExpectedPhysicalLedFrame = Record<PhysicalLedId, ExpectedLed>;

export const LED_OFF: ExpectedLed = {
  mode: "off",
  brightness: "0.00",
  periodMs: null,
  phaseAnchor: "none",
  durationMs: null,
};

/** Builds a COMPLETE 8-LED expected frame; unspecified LEDs are explicitly off. */
export function expectedPhysicalFrame(
  partial: Partial<Record<PhysicalLedId, ExpectedLed>>,
): ExpectedPhysicalLedFrame {
  const out = {} as ExpectedPhysicalLedFrame;
  for (const id of PHYSICAL_LED_IDS) out[id] = partial[id] ?? LED_OFF;
  return out;
}

/** Rejects missing keys, extra keys, function-led IDs and a play indicator. */
export function validatePhysicalFrame(frame: Record<string, unknown>): string[] {
  const problems: string[] = [];
  const keys = Object.keys(frame);
  for (const id of PHYSICAL_LED_IDS) if (!(id in frame)) problems.push(`missing ${id}`);
  for (const k of keys) {
    if (!isPhysicalLedId(k)) problems.push(`not a physical LED: ${k}`);
    if (k.startsWith("function-led")) problems.push(`fictional function LED: ${k}`);
    if (k === "play-indicator") problems.push("play-indicator is web-only, not a physical LED");
  }
  if (keys.length !== PHYSICAL_LED_COUNT)
    problems.push(`frame has ${keys.length} keys, expected ${PHYSICAL_LED_COUNT}`);
  return problems;
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
  ...(opts.indexUnverified ? { indexUnverified: true } : {}),
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
  | "physical LED index unverified"
  | "expected contract missing"
  | "state not available"
  | "initiating conditions not established"
  | "priority arbitration wrong"
  | "timing/clock missing";

/** How a result was obtained. Mocked input NEVER means hardware verification. */
export type ObservationKind =
  | "mocked"
  | "injected/simulated"
  | "browser-observed"
  | "physically-observed"
  | "not-observed";
