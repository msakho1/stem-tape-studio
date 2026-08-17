/**
 * Diagnostic report builder + redaction.
 *
 * REDACTION RULE: the report may never contain audio, uploaded stems, file
 * names, filesystem paths, URLs or unrelated browser data. Only control-surface
 * traffic, machine state, contract results and LED comparisons.
 */

import { EXPECTED_ARTIFACT, type FirmwareConsoleState } from "./firmwareConsole";
import type { ContractResult } from "./contract";
import { BEHAVIOR_CONTRACT_VERSION } from "./contract";
import type { LedInspectionRow } from "./leds";
import { M0_LED_COVERAGE, PHYSICAL_LED_COUNT, type DivergenceCategory, type ObservationKind } from "./physical";
import { SP1_MIDI_CONTRACT, SP1_NOTATION_WARNING } from "./midiContract";
import { formatTraceRow, type TraceRecord, type TraceStats } from "./trace";
import type { FirmwareProfile } from "./firmwareProfiles";
import type { ReproductionResult } from "./segments";

export interface EventRates {
  /** Last-one-second rates. A zero here never erases the cumulative counts. */
  rawMidiPerSec: number;
  surfaceEventsPerSec: number;
  reducerCommandsPerSec: number;
  engineCommandsPerSec: number;
  /** Cumulative counts for the whole capture. */
  cumulativeRawMidi: number;
  cumulativeSurfaceEvents: number;
  cumulativeReducerCommands: number;
  cumulativeEngineCommands: number;
  coalesced: number;
  dropped: number;
  generated: number;
}

export interface StateSnapshot {
  /** Web device power, kept strictly separate from transport playing/stopped. */
  webPowered: boolean;
  playing: boolean;
  globalLoop: string;
  loopDivision: number;
  scrubDirection: "forward" | "reverse" | "idle";
  scrubSpeedLevel: number;
  scrubMultiplier: number;
  scrubLatched: boolean;
  inertia: string;
  activeStem: number;
  fxOverlay: boolean;
  fxScope: string;
  fxBank: number;
  fxMomentary: string[];
  fxLatched: string[];
  /** Three independent arrays. A label such as `1L` is never a solo state. */
  muted: boolean[];
  soloed: boolean[];
  linked: boolean[];
  heldControls: string[];
  /** Null / idle when no gesture currently owns arbitration. */
  arbitrationOwner: string | null;
  /** Hardware fader position, separate from mixer gain. */
  faderHardware: (number | null)[];
  mixerGain: number[];
  faderPickup: string;
  buttons: Record<string, boolean>;
}

export interface FailureRecord {
  id: string;
  /** Reproductions only: a static audit finding never produces a failure. */
  segmentId: string | null;
  lastGoodStage: string;
  firstDivergence: string;
  category: DivergenceCategory | null;
  expected: string;
  actual: string;
  requiresHardware: boolean;
  observation: ObservationKind;
}

export interface LedTransportReport {
  status: string;
  protocolVersion: number | null;
  capabilityQuerySent: boolean;
  leaseActive: boolean;
  commitSequence: number;
  commits: number;
  heartbeats: number;
  stagedMessages: number;
  lastFrame: number[] | null;
  candidateOutputs: { id: string; name: string }[];
  error: string | null;
}

export interface DiagnosticReport {
  contractVersion: string;
  generatedAt: string;
  expectedArtifact: typeof EXPECTED_ARTIFACT;
  firmwareProfiles: FirmwareProfile[];
  midiContract: { rows: typeof SP1_MIDI_CONTRACT; warning: string };
  ledModel: typeof M0_LED_COVERAGE;
  device: {
    midiInputName: string | null;
    midiInputId: string | null;
    midiOutputName: string | null;
    midiOutputId: string | null;
    midiState: string;
    consoleState: string;
    reportedFirmwareVersion: string | null;
    capabilities: Record<string, string>;
  };
  firmware: {
    watchdog: FirmwareConsoleState["watchdog"];
    ain0: number | null;
    ain1: number | null;
    decodedMask: string | null;
    stableMask: string | null;
    unmeasured: number | null;
  };
  ledTransport: LedTransportReport;
  state: StateSnapshot | null;
  rates: EventRates;
  captureStats: TraceStats;
  trace: TraceRecord[];
  contract: ContractResult[];
  reproductions: ReproductionResult[];
  /** EXACTLY the eight physical SP-1 LEDs. */
  physicalLeds: LedInspectionRow[];
  /** Web-only indicators, never part of the physical frame. */
  webOnlyIndicators: LedInspectionRow[];
  failures: FailureRecord[];
  unverified: string[];
  observation: Record<string, ObservationKind>;
}

const FORBIDDEN = /(\.wav|\.mp3|\.flac|\.ogg|\.m4a|\.aiff?|blob:|file:|https?:\/\/|[A-Za-z]:\\|\/Users\/|\/home\/)/i;

/** Deep scrub: drops any string field that looks like a file, path or URL. */
export function redact<T>(value: T): T {
  if (typeof value === "string") {
    return (FORBIDDEN.test(value) ? "[redacted]" : value) as unknown as T;
  }
  if (Array.isArray(value)) return value.map((v) => redact(v)) as unknown as T;
  if (value && typeof value === "object") {
    const out: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(value as Record<string, unknown>)) {
      if (/filename|file|path|url|buffer|stem(s)?Data|audio/i.test(k)) continue;
      out[k] = redact(v);
    }
    return out as unknown as T;
  }
  return value;
}

export function buildReport(input: DiagnosticReport): DiagnosticReport {
  return redact(input);
}

export function reportToText(r: DiagnosticReport): string {
  const t0 = r.trace[0]?.t ?? 0;
  const lines: string[] = [];
  lines.push(`SP-1 DIAGNOSTIC REPORT · ${r.contractVersion} · ${r.generatedAt}`);
  lines.push(`(contract module version ${BEHAVIOR_CONTRACT_VERSION})`);
  lines.push("");
  lines.push("EXPECTED ARTIFACT METADATA (not verified device identity)");
  for (const [k, v] of Object.entries(r.expectedArtifact)) lines.push(`  ${k}: ${v}`);
  lines.push("");
  lines.push("PHYSICAL LED MODEL");
  lines.push(`  physical SP-1 LEDs: ${PHYSICAL_LED_COUNT} (${r.ledModel.layout})`);
  lines.push(`  side row: ${r.ledModel.sideRow}`);
  lines.push(`  represented on the website: ${r.ledModel.webIndicators}`);
  lines.push(`  electrical GPIO coverage (audited M0 driver): ${r.ledModel.electricalCoverage}`);
  lines.push(`  Stem Tape behaviour mapping coverage: ${r.ledModel.behaviorCoverage}`);
  lines.push(`  host→device LED feedback: ${r.ledModel.hostToDeviceLedFeedback}`);
  lines.push(`  note: ${r.ledModel.note}`);
  lines.push("");
  lines.push("MIDI CONTRACT (decimal / hex)");
  lines.push(`  ${SP1_NOTATION_WARNING}`);
  for (const row of r.midiContract.rows) {
    lines.push(`  ${row.kind === "note" ? "note" : "CC  "} ${String(row.dec).padStart(3)} (${row.hex}) ${row.name}`);
  }
  lines.push("");
  lines.push("DEVICE");
  lines.push(`  midi input: ${r.device.midiInputName ?? "none"} (${r.device.midiInputId ?? "-"})`);
  lines.push(`  midi output: ${r.device.midiOutputName ?? "none"} (${r.device.midiOutputId ?? "-"})`);
  lines.push(`  midi state: ${r.device.midiState}`);
  lines.push(`  firmware console: ${r.device.consoleState}`);
  lines.push(`  reported firmware banner: ${r.device.reportedFirmwareVersion ?? "not reported"}`);
  for (const [k, v] of Object.entries(r.device.capabilities)) lines.push(`  capability ${k}: ${v}`);
  lines.push("");
  lines.push("FIRMWARE CONSOLE");
  lines.push(`  watchdog: ${r.firmware.watchdog ? JSON.stringify(r.firmware.watchdog) : "not reported"}`);
  lines.push(`  AIN0=${r.firmware.ain0 ?? "-"} AIN1=${r.firmware.ain1 ?? "-"}`);
  lines.push(`  decoded=${r.firmware.decodedMask ?? "-"} stable=${r.firmware.stableMask ?? "-"} unmeasured=${r.firmware.unmeasured ?? "-"}`);
  lines.push("");
  lines.push("FIRMWARE PROFILE REGISTRY (expected metadata, never device identity)");
  for (const p of r.firmwareProfiles) {
    lines.push(`  ${p.status === "current" ? "*" : " "} ${p.id}: ${p.firmwareBanner} · led protocol v${p.ledProtocolVersion} · sha256 ${p.binarySha256}`);
  }
  lines.push("");
  lines.push("HOST→DEVICE LED LINK (channel 16, protocol v1)");
  lines.push(`  ${JSON.stringify(r.ledTransport)}`);
  lines.push("");
  lines.push("CAPTURE");
  lines.push(`  ${JSON.stringify(r.captureStats)}`);
  lines.push("");
  lines.push("REPRODUCTIONS");
  if (r.reproductions.length === 0) lines.push("  none run — every contract entry is reference data only");
  for (const rep of r.reproductions) {
    lines.push(`  ${rep.segmentId} ${rep.name}: ${rep.status} · ${rep.observationSource} · ${rep.detail ?? ""}`);
  }
  lines.push("");
  lines.push("STATE");
  lines.push(`  ${r.state ? JSON.stringify(r.state) : "no state"}`);
  lines.push("");
  lines.push("RATES");
  lines.push(`  ${JSON.stringify(r.rates)}`);
  lines.push("");
  lines.push(`PHYSICAL LED COMPARISON (${r.physicalLeds.length} of ${PHYSICAL_LED_COUNT})`);
  lines.push("  columns: 1 expected contract | 2 web logical | 3 rendered DOM/CSS | 4 transmitted/firmware");
  for (const l of r.physicalLeds) {
    lines.push(`  ${l.id}`);
    lines.push(`      1 expected:    ${l.columnExpected}`);
    lines.push(`      2 web logical: ${l.columnWebLogical}`);
    lines.push(`      3 rendered:    ${l.columnDom}`);
    lines.push(`      4 transmitted: ${l.columnTransmitted}`);
    lines.push(`      physical observation: ${l.physicalObservation}`);
    if (l.mismatch) lines.push(`      MISMATCH: ${l.mismatch}`);
  }
  lines.push("");
  lines.push("WEB-ONLY INDICATORS — NOT PART OF THE 8-LED PHYSICAL FRAME");
  for (const l of r.webOnlyIndicators) {
    lines.push(`  ${l.id.padEnd(16)} actual ${l.actualMode} · ${l.owner}`);
  }
  lines.push("");
  lines.push("BEHAVIOUR CONTRACT");
  for (const c of r.contract) {
    lines.push(
      `  [audit ${c.implementationStatus} · reproduction ${c.reproductionStatus} · source ${c.observationSource}] ${c.group} · ${c.name} · ${c.provenance} (${c.confidence})`,
    );
    lines.push(`      source: ${c.citation.title} ${c.citation.version} — ${c.citation.locator} (${c.citation.evidence})`);
    lines.push(`      from: ${c.initiatingState} | seq: ${c.sequence} | timing: ${c.timing}`);
    lines.push(`      owner: ${c.expectedOwner} | command: ${c.expectedCommand} | engine: ${c.expectedEngineResult}`);
    lines.push(`      leds: ${c.expectedLedSummary}`);
    lines.push(`      precedence ${c.precedence}${c.competing.length ? ` · competing: ${c.competing.join(", ")}` : ""}`);
    if (c.observed) lines.push(`      observed: ${c.observed}`);
    if (c.reproductionFirstDivergence) {
      lines.push(`      first divergence (observed): ${c.reproductionFirstDivergence}`);
    } else if (c.firstDivergence) {
      lines.push(`      audit divergence category (NOT an observed failure): ${c.firstDivergence}`);
    }
    if (c.notes) lines.push(`      note: ${c.notes}`);
  }
  lines.push("");
  lines.push("FAILURES / FIRST DIVERGENCE");
  if (r.failures.length === 0) lines.push("  none recorded");
  for (const f of r.failures) {
    lines.push(
      `  ${f.id}: last good = ${f.lastGoodStage}; diverged = ${f.firstDivergence} [${f.category ?? "uncategorised"}]`,
    );
    lines.push(`      expected ${f.expected} · actual ${f.actual} · ${f.observation}${f.requiresHardware ? " · requires real SP-1" : ""}`);
  }
  lines.push("");
  lines.push("UNVERIFIED");
  for (const u of r.unverified) lines.push(`  ${u}`);
  lines.push("");
  lines.push("OBSERVATION BASIS");
  for (const [k, v] of Object.entries(r.observation)) lines.push(`  ${k}: ${v}`);
  lines.push("");
  lines.push(`TRACE (${r.trace.length} records, ring capacity 500)`);
  for (const rec of r.trace) lines.push(`  ${formatTraceRow(rec, t0)}`);
  return lines.join("\n");
}
