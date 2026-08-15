/**
 * Cue markers — Checkpoint 2 of Stem Instrument Mode.
 *
 * PURE module. It owns the cue-learning / cue-playback *decision* state only:
 * no AudioEngine, no DOM, no React, no persistence, no command emission, no
 * arbiter, no UI. Callers hand it normalized MIDI events plus precomputed
 * snapshots and receive a description of what should happen. Wiring happens in
 * a later checkpoint.
 */

import type { StemMidiEvent } from "./midi/contract";
import { eventKey } from "./midi/contract";

export type CueLane = 0 | 1 | 2 | 3;
export type CueScope = "global" | "lane";

/** Minimum committed passage length. Shorter captures are discarded. */
export const MIN_CUE_FRAMES = 1024;

/** Held hardware qualifiers at the instant of the Note On. */
export type QualifierSnapshot = {
  functionHeld: boolean;
  /** Track buttons currently held, any order. */
  tracksHeld: readonly CueLane[];
};

/**
 * Engine conditions at the instant of the event. Learning requires aligned,
 * forward, non-looping Tape playback; every field is precomputed by the caller.
 */
export type EligibilitySnapshot = {
  headsActive: boolean;
  /** Any scrub in progress: global shuttle or per-lane fader scrub. */
  scrubActive: boolean;
  /** Any lane reversed. */
  reverseActive: boolean;
  /** Global loop running or any lane loop enabled. */
  loopActive: boolean;
  transportPlaying: boolean;
  /** Integrated playback rate; must be aligned forward 1x to learn. */
  rate: number;
};

export const RATE_TOLERANCE = 1e-3;

export type CueSource = { lane: CueLane; contentHash: string };

export type CueInvalidation = { invalid: true; reason: "source-replaced" } | { invalid: false };

export type CueMarker = {
  key: string;
  channel: number;
  note: number;
  scope: CueScope;
  /** null for global markers. */
  lane: CueLane | null;
  startFrame: number;
  endFrame: number;
  sampleRate: number;
  /** Content identity of every stem the marker depends on. */
  sources: CueSource[];
  createdAt: number;
  /** Retained-but-unplayable flag; markers are never deleted by invalidation. */
  invalidReason: "source-replaced" | null;
};

export type LearnRejectReason =
  | "heads-active"
  | "scrub-active"
  | "reverse-active"
  | "loop-active"
  | "transport-stopped"
  | "rate-not-1x"
  | "multiple-tracks-held";

export type DiscardReason = LearnRejectReason | "too-short" | "cancelled";

export type CueEventContext = {
  /** Frame the event actually landed on, precomputed from the mapped timestamp. */
  frame: number;
  qualifiers: QualifierSnapshot;
  eligibility: EligibilitySnapshot;
  sampleRate: number;
  /** Current content hash of each lane, index = lane. */
  contentHashes: readonly string[];
};

export type CueAction =
  | { type: "learn.start"; key: string; scope: CueScope; lane: CueLane | null; startFrame: number }
  | { type: "learn.commit"; key: string; marker: CueMarker }
  | { type: "learn.discard"; key: string; reason: DiscardReason }
  | { type: "learn.reject"; key: string; reason: LearnRejectReason }
  | { type: "cue.play"; key: string; marker: CueMarker }
  | { type: "cue.reject"; key: string; reason: "source-replaced" }
  | { type: "ignored"; key: string; reason: "unlearned" | "playback-note-off" | "note-off-unmatched" | "all-notes-off" };

type Capture = {
  key: string;
  channel: number;
  note: number;
  scope: CueScope;
  lane: CueLane | null;
  startFrame: number;
  sampleRate: number;
  sources: CueSource[];
};

const REASON_TEXT: Record<DiscardReason, string> = {
  "heads-active": "Heads mode is active",
  "scrub-active": "a scrub is in progress",
  "reverse-active": "a stem is reversed",
  "loop-active": "a loop is running",
  "transport-stopped": "the transport is stopped",
  "rate-not-1x": "playback is not aligned 1x forward",
  "multiple-tracks-held": "hold one Track button to learn an isolated cue",
  "too-short": "the passage is shorter than the minimum length",
  cancelled: "the capture was cancelled",
};

export function describeReason(reason: DiscardReason | "source-replaced" | "unlearned"): string {
  if (reason === "source-replaced") return "source replaced";
  if (reason === "unlearned") return "no cue learned for this key";
  return REASON_TEXT[reason];
}

/** Learning eligibility, in the plan's rejection order. Null means eligible. */
export function learnRejection(e: EligibilitySnapshot): LearnRejectReason | null {
  if (e.headsActive) return "heads-active";
  if (e.scrubActive) return "scrub-active";
  if (e.reverseActive) return "reverse-active";
  if (e.loopActive) return "loop-active";
  if (!e.transportPlaying) return "transport-stopped";
  if (!(e.rate > 0) || Math.abs(e.rate - 1) > RATE_TOLERANCE) return "rate-not-1x";
  return null;
}

/** FUNCTION wins over Tracks; two or more Tracks without FUNCTION is a rejection. */
export function resolveQualifier(
  q: QualifierSnapshot,
): { kind: "global" } | { kind: "lane"; lane: CueLane } | { kind: "none" } | { kind: "reject" } {
  if (q.functionHeld) return { kind: "global" };
  const held = Array.from(new Set(q.tracksHeld));
  if (held.length === 0) return { kind: "none" };
  if (held.length > 1) return { kind: "reject" };
  return { kind: "lane", lane: held[0] as CueLane };
}

function sourcesFor(
  scope: CueScope,
  lane: CueLane | null,
  hashes: readonly string[],
): CueSource[] {
  if (scope === "lane" && lane != null) {
    return [{ lane, contentHash: hashes[lane] ?? "" }];
  }
  return [0, 1, 2, 3].map((l) => ({ lane: l as CueLane, contentHash: hashes[l] ?? "" }));
}

export class CueStore {
  private markers = new Map<string, CueMarker>();
  private captures = new Map<string, Capture>();
  private now: () => number;

  constructor(now: () => number = () => Date.now()) {
    this.now = now;
  }

  list(): CueMarker[] {
    return Array.from(this.markers.values());
  }

  get(key: string): CueMarker | null {
    return this.markers.get(key) ?? null;
  }

  openCaptures(): string[] {
    return Array.from(this.captures.keys());
  }

  /** Restore persisted markers (metadata only). */
  load(markers: readonly CueMarker[]): void {
    this.markers.clear();
    for (const m of markers) this.markers.set(m.key, { ...m });
  }

  handle(ev: StemMidiEvent, ctx: CueEventContext): CueAction {
    const key = eventKey(ev);
    if (ev.kind === "allNotesOff") {
      this.captures.clear();
      return { type: "ignored", key, reason: "all-notes-off" };
    }
    if (ev.kind === "noteOn") return this.noteOn(ev, ctx, key);
    return this.noteOff(ctx, key);
  }

  private noteOn(ev: StemMidiEvent, ctx: CueEventContext, key: string): CueAction {
    const q = resolveQualifier(ctx.qualifiers);

    if (q.kind === "none") {
      const marker = this.markers.get(key);
      if (!marker) return { type: "ignored", key, reason: "unlearned" };
      if (marker.invalidReason) return { type: "cue.reject", key, reason: marker.invalidReason };
      return { type: "cue.play", key, marker };
    }

    if (q.kind === "reject") {
      this.captures.delete(key);
      return { type: "learn.reject", key, reason: "multiple-tracks-held" };
    }

    const bad = learnRejection(ctx.eligibility);
    if (bad) {
      this.captures.delete(key);
      return { type: "learn.reject", key, reason: bad };
    }

    const scope: CueScope = q.kind === "global" ? "global" : "lane";
    const lane: CueLane | null = q.kind === "lane" ? q.lane : null;
    this.captures.set(key, {
      key,
      channel: ev.channel,
      note: ev.note,
      scope,
      lane,
      startFrame: ctx.frame,
      sampleRate: ctx.sampleRate,
      sources: sourcesFor(scope, lane, ctx.contentHashes),
    });
    return { type: "learn.start", key, scope, lane, startFrame: ctx.frame };
  }

  private noteOff(ctx: CueEventContext, key: string): CueAction {
    const cap = this.captures.get(key);
    if (!cap) {
      if (this.markers.has(key)) return { type: "ignored", key, reason: "playback-note-off" };
      return { type: "ignored", key, reason: "note-off-unmatched" };
    }
    this.captures.delete(key);

    const bad = learnRejection(ctx.eligibility);
    if (bad) return { type: "learn.discard", key, reason: bad };

    const endFrame = ctx.frame;
    if (endFrame - cap.startFrame < MIN_CUE_FRAMES) {
      return { type: "learn.discard", key, reason: "too-short" };
    }

    const marker: CueMarker = {
      key,
      channel: cap.channel,
      note: cap.note,
      scope: cap.scope,
      lane: cap.lane,
      startFrame: cap.startFrame,
      endFrame,
      sampleRate: cap.sampleRate,
      sources: cap.sources,
      createdAt: this.now(),
      invalidReason: null,
    };
    this.markers.set(key, marker); // same key relearn overwrites in place
    return { type: "learn.commit", key, marker };
  }

  /**
   * Poll between events: if an eligibility condition began mid-capture, every
   * open capture is discarded with that reason.
   */
  syncEligibility(e: EligibilitySnapshot): CueAction[] {
    const bad = learnRejection(e);
    if (!bad) return [];
    const out: CueAction[] = [];
    for (const key of Array.from(this.captures.keys())) {
      this.captures.delete(key);
      out.push({ type: "learn.discard", key, reason: bad });
    }
    return out;
  }

  /** All Notes Off / disconnect / song change / transport stop. */
  cancelAllCaptures(reason: DiscardReason = "cancelled"): CueAction[] {
    const out: CueAction[] = [];
    for (const key of Array.from(this.captures.keys())) {
      this.captures.delete(key);
      out.push({ type: "learn.discard", key, reason });
    }
    return out;
  }

  /**
   * contentHash-only invalidation. A changed lane invalidates its isolated
   * markers and every global marker. Markers are retained with a reason;
   * a lane restored to its original hash becomes playable again.
   */
  revalidate(hashes: readonly string[]): { invalidated: string[]; restored: string[] } {
    const invalidated: string[] = [];
    const restored: string[] = [];
    for (const m of this.markers.values()) {
      const stale = m.sources.some((s) => (hashes[s.lane] ?? "") !== s.contentHash);
      const was = m.invalidReason !== null;
      if (stale && !was) {
        m.invalidReason = "source-replaced";
        invalidated.push(m.key);
      } else if (!stale && was) {
        m.invalidReason = null;
        restored.push(m.key);
      }
    }
    return { invalidated, restored };
  }
}
