/**
 * Phase 6 recording state machine (plan §E, binding corrections M5–M9).
 *
 * Pure, serializable, framework-free — the whole bare-Track state table is
 * decided here and unit-tested. Audio-thread events (onset, stop, interrupt)
 * are fed in as inputs; LEDs and acks are derived from the result, never
 * written by handlers.
 */

export type RecPhase =
  | "idle"
  | "arming"
  | "waiting-for-sound"
  | "waiting-for-grid"
  | "recording"
  | "overdubbing"
  | "stopping"
  | "finalizing"
  | "ready"
  | "interrupted"
  | "failed";

export type TrackContent = "empty" | "loaded" | "trashed";

export interface TrackRecState {
  phase: RecPhase;
  content: TrackContent;
  takeId: string | null;
  /** Frames written so far, mirrored from the audio thread. */
  frames: number;
  failureReason: string | null;
}

export interface RecordingModel {
  /** The single armed / recording external-input target, or null. */
  target: number | null;
  inputEnabled: boolean;
  /** Track whose arm is waiting on the user enabling input (M6). */
  pendingInputTarget: number | null;
  fxOverlay: boolean;
  tracks: TrackRecState[];
  lastAck: string;
}

export type RecIntent =
  | { type: "rec.arm"; track: number }
  | { type: "rec.cancelArm"; track: number }
  | { type: "rec.stop"; track: number }
  | { type: "rec.tap"; track: number }
  | { type: "rec.doubleTap"; track: number }
  | { type: "rec.requestInput"; track: number }
  | { type: "rec.inputEnabled" }
  | { type: "rec.inputDenied" }
  | { type: "rec.onset"; track: number; contextFrame: number; takeId: string }
  | { type: "rec.progress"; track: number; frames: number }
  | { type: "rec.finalizing"; track: number }
  | { type: "rec.ready"; track: number }
  | { type: "rec.interrupted"; track: number; reason: string }
  | { type: "rec.delete"; track: number }
  | { type: "rec.mute"; track: number };

export const ACTIVE_PHASES: RecPhase[] = ["recording", "overdubbing", "stopping", "finalizing"];

export function initialRecordingModel(contents: TrackContent[] = ["empty", "empty", "empty", "empty"]): RecordingModel {
  return {
    target: null,
    inputEnabled: false,
    pendingInputTarget: null,
    fxOverlay: false,
    tracks: contents.map((content) => ({ phase: "idle", content, takeId: null, frames: 0, failureReason: null })),
    lastAck: "recording idle",
  };
}

function setTrack(m: RecordingModel, i: number, patch: Partial<TrackRecState>): TrackRecState[] {
  return m.tracks.map((t, k) => (k === i ? { ...t, ...patch } : t));
}

function ack(m: RecordingModel, detail: string): RecordingModel {
  return { ...m, lastAck: detail };
}

export function isBusy(t: TrackRecState): boolean {
  return ACTIVE_PHASES.includes(t.phase);
}

/**
 * Single reducer for every recording intent. Returns the new model; the caller
 * turns `lastAck` into an ordered semantic ack.
 */
export function recordingReduce(model: RecordingModel, intent: RecIntent): RecordingModel {
  // M9 — the FX overlay owns Tracks 1–4 completely.
  if (model.fxOverlay && "track" in intent && intent.type !== "rec.progress") {
    return ack(model, `rejected: FX overlay open — track ${(intent as { track: number }).track + 1} is an FX control, not a recorder`);
  }

  const i = "track" in intent ? intent.track : -1;
  const t = i >= 0 ? model.tracks[i] : undefined;

  switch (intent.type) {
    case "rec.requestInput":
      return ack({ ...model, pendingInputTarget: i }, `input required — Input Drawer opened, track ${i + 1} pending Enable Input`);

    case "rec.inputEnabled": {
      const pending = model.pendingInputTarget;
      const next: RecordingModel = { ...model, inputEnabled: true, pendingInputTarget: null };
      if (pending == null) return ack(next, "input enabled");
      return recordingReduce(ack(next, "input enabled"), { type: "rec.arm", track: pending });
    }

    case "rec.inputDenied":
      return ack({ ...model, inputEnabled: false, pendingInputTarget: null }, "input denied — pending arm cleared, LEDs restored");

    case "rec.arm": {
      if (!t) return ack(model, "rejected: no such track");
      if (!model.inputEnabled) return recordingReduce(model, { type: "rec.requestInput", track: i });
      if (isBusy(t)) return ack(model, `rejected: track ${i + 1} is ${t.phase}`);
      const other = model.target;
      if (other != null && other !== i) {
        const ot = model.tracks[other]!;
        if (isBusy(ot)) return ack(model, `rejected: track ${other + 1} is ${ot.phase} — arm cannot move`);
        const cleared = model.tracks.map((x, k) => (k === other ? { ...x, phase: "idle" as RecPhase } : x));
        const moved = cleared.map((x, k) => (k === i ? { ...x, phase: "waiting-for-sound" as RecPhase } : x));
        return ack({ ...model, tracks: moved, target: i }, `switched: arm moved from track ${other + 1} to track ${i + 1}`);
      }
      return ack(
        { ...model, target: i, tracks: setTrack(model, i, { phase: "waiting-for-sound", failureReason: null }) },
        t.content === "empty"
          ? `armed track ${i + 1} — records on your first sound`
          : `armed track ${i + 1} for overdub — records on your first sound`,
      );
    }

    case "rec.cancelArm": {
      if (!t) return model;
      if (t.phase !== "waiting-for-sound" && t.phase !== "waiting-for-grid" && t.phase !== "arming")
        return ack(model, `no-op: track ${i + 1} is ${t.phase}`);
      return ack({ ...model, target: null, tracks: setTrack(model, i, { phase: "idle" }) }, `arm cancelled on track ${i + 1}`);
    }

    case "rec.onset": {
      if (!t || (t.phase !== "waiting-for-sound" && t.phase !== "waiting-for-grid")) return ack(model, "rejected: onset with no armed target");
      const phase: RecPhase = t.content === "empty" ? "recording" : "overdubbing";
      return ack(
        { ...model, tracks: setTrack(model, i, { phase, takeId: intent.takeId, frames: 0 }) },
        `${phase} track ${i + 1} from context frame ${intent.contextFrame}`,
      );
    }

    case "rec.progress":
      if (!t) return model;
      return { ...model, tracks: setTrack(model, i, { frames: intent.frames }) };

    case "rec.stop":
      if (!t || (t.phase !== "recording" && t.phase !== "overdubbing")) return ack(model, `no-op: track ${i + 1} is ${t?.phase}`);
      return ack({ ...model, tracks: setTrack(model, i, { phase: "stopping" }) }, `stopping take on track ${i + 1}`);

    case "rec.finalizing":
      if (!t) return model;
      return ack({ ...model, tracks: setTrack(model, i, { phase: "finalizing" }) }, `finalising take on track ${i + 1}`);

    case "rec.ready":
      if (!t) return model;
      return ack(
        { ...model, target: model.target === i ? null : model.target, tracks: setTrack(model, i, { phase: "idle", content: "loaded" }) },
        `take ready on track ${i + 1} — track returns to normal mute behaviour`,
      );

    case "rec.interrupted":
      if (!t) return model;
      return ack(
        { ...model, target: model.target === i ? null : model.target, tracks: setTrack(model, i, { phase: "failed", failureReason: intent.reason }) },
        `failed mid-take on track ${i + 1}: ${intent.reason} — committed chunks preserved, take is NOT ready`,
      );

    // ---- bare-Track state table (M5) ------------------------------------
    case "rec.tap": {
      if (!t) return model;
      switch (t.phase) {
        case "recording":
        case "overdubbing":
          return recordingReduce(model, { type: "rec.stop", track: i });
        case "waiting-for-sound":
        case "waiting-for-grid":
        case "arming":
          return recordingReduce(model, { type: "rec.cancelArm", track: i });
        case "stopping":
        case "finalizing":
          return ack(model, `status: track ${i + 1} is ${t.phase} — no new take, no delete`);
        case "failed":
          return ack(model, `recovery: track ${i + 1} failed (${t.failureReason ?? "unknown"}) — open the Input Drawer to recover or discard`);
        default:
          if (t.content === "empty") return ack(model, `status: track ${i + 1} empty — hold to arm`);
          return recordingReduce(model, { type: "rec.mute", track: i });
      }
    }

    case "rec.doubleTap": {
      if (!t) return model;
      if (isBusy(t) || t.phase === "waiting-for-sound" || t.phase === "waiting-for-grid")
        return recordingReduce(model, t.phase === "waiting-for-sound" || t.phase === "waiting-for-grid" ? { type: "rec.cancelArm", track: i } : { type: "rec.stop", track: i });
      if (t.phase === "failed") return ack(model, `track ${i + 1} failed mid-take — delete is blocked until you recover or discard it`);
      if (t.content === "empty") return ack(model, `no-op: track ${i + 1} is empty`);
      return recordingReduce(model, { type: "rec.delete", track: i });
    }

    case "rec.delete":
      if (!t) return model;
      return ack({ ...model, tracks: setTrack(model, i, { content: "trashed" }) }, `track ${i + 1} moved to recoverable trash`);

    case "rec.mute":
      return ack(model, `track ${i + 1} mute/unmute (normal loaded-track behaviour)`);
  }
}

/**
 * The LED tier this track claims (M11). Higher wins; the caller merges it with
 * the existing FX / solo / base tiers in `surface.ts`.
 */
export const REC_LED_TIERS = {
  failed: 96,
  recording: 94,
  overdubbing: 93,
  finalizing: 92,
  armed: 91,
} as const;

export function recLedFor(t: TrackRecState): { pattern: string; reason: string; priority: number } | null {
  switch (t.phase) {
    case "failed":
      return { pattern: "blink", reason: `failed mid-take: ${t.failureReason ?? "unknown"}`, priority: REC_LED_TIERS.failed };
    case "recording":
      return { pattern: "solid", reason: "recording (solid bright)", priority: REC_LED_TIERS.recording };
    case "overdubbing":
      return { pattern: "pulse", reason: "overdubbing (double pulse)", priority: REC_LED_TIERS.overdubbing };
    case "stopping":
    case "finalizing":
      return { pattern: "chase", reason: "finalising take (rapid chase)", priority: REC_LED_TIERS.finalizing };
    case "arming":
    case "waiting-for-sound":
    case "waiting-for-grid":
      return { pattern: "breathe", reason: "armed — records on your first sound", priority: REC_LED_TIERS.armed };
    default:
      return null;
  }
}
