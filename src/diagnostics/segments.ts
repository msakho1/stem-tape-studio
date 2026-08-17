/**
 * Named reproduction segments.
 *
 * The behaviour contract is REFERENCE DATA. A contract entry is never a failed
 * test until a reproduction has actually been run against it. This module owns
 * that distinction: until `begin()` → `end()` executes for an entry, its
 * reproduction status stays `not-run` and it can produce no failure record.
 */

import { trace, traceNow } from "./trace";

export type ReproductionStatus = "not-run" | "running" | "passed" | "failed" | "blocked";

export type ObservationSource = "live-hardware" | "browser-injection" | "mocked" | "source-audit";

export interface ReproductionResult {
  segmentId: string;
  contractId: string;
  name: string;
  status: ReproductionStatus;
  observationSource: ObservationSource;
  startedAt: number;
  endedAt: number | null;
  initialState: string;
  finalState: string | null;
  /** Stages that were actually observed, in order. */
  stagesSeen: string[];
  lastSuccessfulStage: string | null;
  /** Only ever set when a reproduction really ran and really diverged. */
  firstMissingStage: string | null;
  detail: string | null;
}

export interface SegmentDefinition {
  id: string;
  contractId: string;
  name: string;
  /** Stages that must be observed, in order, for the reproduction to pass. */
  requiredStages: string[];
}

/** The reproductions the diagnostic drawer offers as BEGIN REPRODUCTION. */
export const SEGMENT_DEFINITIONS: readonly SegmentDefinition[] = [
  { id: "fn-rocker-fwd", contractId: "fn.rocker.fwd", name: "FUNCTION + rocker forward", requiredStages: ["surface.decoded", "gesture.arbitration", "command.surface", "engine.ack"] },
  { id: "fn-rocker-rwd", contractId: "fn.rocker.rwd", name: "FUNCTION + rocker reverse", requiredStages: ["surface.decoded", "gesture.arbitration", "command.surface", "engine.ack"] },
  { id: "scrub-momentary-release", contractId: "scrub.momentary", name: "momentary scrub release", requiredStages: ["surface.decoded", "command.surface", "engine.ack"] },
  { id: "scrub-latch-unlatch", contractId: "scrub.latch", name: "scrub latch and bare-FUNCTION unlatch", requiredStages: ["surface.decoded", "command.surface", "engine.ack"] },
  { id: "loop-momentary", contractId: "loop.momentary", name: "momentary global loop", requiredStages: ["surface.decoded", "command.surface", "engine.ack"] },
  { id: "loop-latch", contractId: "loop.latch", name: "FUNCTION loop latch", requiredStages: ["surface.decoded", "command.surface", "engine.ack"] },
  { id: "loop-play-exit", contractId: "loop.play.exit", name: "PLAY latched-loop exit", requiredStages: ["surface.decoded", "command.surface", "engine.ack"] },
  { id: "fx-track-momentary-loop", contractId: "fx.track.momentaryLoop", name: "FX Track during momentary loop", requiredStages: ["surface.decoded", "gesture.arbitration", "command.surface"] },
  { id: "fx-track-latched-loop", contractId: "fx.track.latchedLoop", name: "FX Track during latched loop", requiredStages: ["surface.decoded", "gesture.arbitration", "command.surface"] },
  { id: "fx-track-play-held", contractId: "fx.track.playHeld", name: "FX Track while PLAY is physically held", requiredStages: ["surface.decoded", "gesture.arbitration", "command.surface"] },
  { id: "resync-t1-t4", contractId: "system.resync", name: "Track 1 + Track 4 resync", requiredStages: ["surface.decoded", "connection.resync"] },
  { id: "fader-initial", contractId: "mixer.faderSnapshot", name: "initial fader snapshot", requiredStages: ["surface.decoded"] },
  { id: "fader-idle-10s", contractId: "mixer.faderIdle", name: "ten-second idle-fader observation", requiredStages: [] },
  { id: "led-sweep", contractId: "led.sweep", name: "physical LED sweep", requiredStages: ["led.derived", "led.transmitted"] },
] as const;

export class SegmentRunner {
  private active: ReproductionResult | null = null;
  private results = new Map<string, ReproductionResult>();
  private counter = 0;
  private listeners = new Set<() => void>();

  subscribe(fn: () => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  private notify(): void {
    for (const fn of this.listeners) fn();
  }

  current(): ReproductionResult | null {
    return this.active ? { ...this.active, stagesSeen: [...this.active.stagesSeen] } : null;
  }

  resultFor(contractId: string): ReproductionResult | null {
    const r = this.results.get(contractId);
    return r ? { ...r, stagesSeen: [...r.stagesSeen] } : null;
  }

  all(): ReproductionResult[] {
    return [...this.results.values()].map((r) => ({ ...r, stagesSeen: [...r.stagesSeen] }));
  }

  begin(def: SegmentDefinition, initialState: string, source: ObservationSource = "live-hardware"): ReproductionResult {
    this.counter += 1;
    const segmentId = `${def.id}#${this.counter}`;
    const rec: ReproductionResult = {
      segmentId,
      contractId: def.contractId,
      name: def.name,
      status: "running",
      observationSource: source,
      startedAt: traceNow(),
      endedAt: null,
      initialState,
      finalState: null,
      stagesSeen: [],
      lastSuccessfulStage: null,
      firstMissingStage: null,
      detail: null,
    };
    this.active = rec;
    this.required = def.requiredStages;
    this.results.set(def.contractId, rec);
    trace.beginSegment(segmentId);
    trace.record("capture.control", `BEGIN REPRODUCTION ${def.name}`, { segmentId, initialState });
    this.notify();
    return rec;
  }

  private required: string[] = [];

  /** Called by the drawer as stages arrive, in order. */
  observeStage(stage: string): void {
    if (!this.active) return;
    this.active.stagesSeen.push(stage);
    this.notify();
  }

  end(finalState: string): ReproductionResult | null {
    const rec = this.active;
    if (!rec) return null;
    rec.endedAt = traceNow();
    rec.finalState = finalState;
    let lastGood: string | null = null;
    let missing: string | null = null;
    for (const stage of this.required) {
      if (rec.stagesSeen.includes(stage)) lastGood = stage;
      else {
        missing = stage;
        break;
      }
    }
    rec.lastSuccessfulStage = lastGood;
    rec.firstMissingStage = missing;
    rec.status = missing ? "failed" : "passed";
    rec.detail = missing
      ? `last successful stage ${lastGood ?? "none"}, first missing stage ${missing}`
      : `all required stages observed (${this.required.join(" → ") || "none required"})`;
    trace.record("capture.control", `END REPRODUCTION ${rec.name} → ${rec.status}`, {
      segmentId: rec.segmentId,
      finalState,
      lastSuccessfulStage: rec.lastSuccessfulStage,
      firstMissingStage: rec.firstMissingStage,
    });
    trace.endSegment();
    this.active = null;
    this.notify();
    return rec;
  }

  reset(): void {
    this.active = null;
    this.results.clear();
    trace.endSegment();
    this.notify();
  }
}

export const segmentRunner = new SegmentRunner();
