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
import { formatTraceRow, type TraceRecord } from "./trace";

export interface EventRates {
  rawMidiPerSec: number;
  surfaceEventsPerSec: number;
  engineCommandsPerSec: number;
  unmatchedReleases: number;
  staleEvents: number;
  suppressed: number;
}

export interface StateSnapshot {
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
  heldControls: string[];
  arbitrationOwner: string;
  faders: number[];
  faderPickup: string;
  buttons: Record<string, boolean>;
}

export interface DiagnosticReport {
  contractVersion: string;
  generatedAt: string;
  expectedArtifact: typeof EXPECTED_ARTIFACT;
  device: {
    midiInputName: string | null;
    midiInputId: string | null;
    midiOutputName: string | null;
    midiState: string;
    consoleState: string;
    reportedFirmwareVersion: string | null;
    capabilities: Record<string, "supported" | "unsupported" | "present">;
  };
  firmware: {
    watchdog: FirmwareConsoleState["watchdog"];
    ain0: number | null;
    ain1: number | null;
    decodedMask: string | null;
    stableMask: string | null;
    unmeasured: number | null;
  };
  state: StateSnapshot | null;
  rates: EventRates;
  trace: TraceRecord[];
  contract: ContractResult[];
  leds: LedInspectionRow[];
  failures: { id: string; lastGoodStage: string; firstDivergence: string }[];
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
  lines.push("");
  lines.push("EXPECTED ARTIFACT METADATA (not verified device identity)");
  for (const [k, v] of Object.entries(r.expectedArtifact)) lines.push(`  ${k}: ${v}`);
  lines.push("");
  lines.push("DEVICE");
  lines.push(`  midi input: ${r.device.midiInputName ?? "none"} (${r.device.midiInputId ?? "-"})`);
  lines.push(`  midi output: ${r.device.midiOutputName ?? "none"}`);
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
  lines.push("STATE");
  lines.push(`  ${r.state ? JSON.stringify(r.state) : "no state"}`);
  lines.push("");
  lines.push("RATES");
  lines.push(`  ${JSON.stringify(r.rates)}`);
  lines.push("");
  lines.push("LED COMPARISON");
  for (const l of r.leds) {
    lines.push(
      `  ${l.id.padEnd(16)} expected ${l.expectedMode.padEnd(14)} actual ${l.actualMode.padEnd(14)} ${l.animation} · ${l.owner}${l.mismatch ? ` · MISMATCH: ${l.mismatch}` : ""}`,
    );
  }
  lines.push("");
  lines.push("BEHAVIOUR CONTRACT");
  for (const c of r.contract) {
    lines.push(`  [${c.status}] ${c.name} · ${c.provenance} · ${c.reference}`);
    lines.push(`      seq: ${c.sequence}`);
    lines.push(`      expect: ${c.expectedCommand} | leds: ${c.expectedLeds}`);
    if (c.observed) lines.push(`      observed: ${c.observed}`);
    if (c.notes) lines.push(`      note: ${c.notes}`);
  }
  lines.push("");
  lines.push("FAILURES / FIRST DIVERGENCE");
  if (r.failures.length === 0) lines.push("  none recorded");
  for (const f of r.failures) lines.push(`  ${f.id}: last good = ${f.lastGoodStage}; diverged = ${f.firstDivergence}`);
  lines.push("");
  lines.push(`TRACE (${r.trace.length} records, ring capacity 500)`);
  for (const rec of r.trace) lines.push(`  ${formatTraceRow(rec, t0)}`);
  return lines.join("\n");
}
