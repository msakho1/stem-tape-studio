/**
 * Ordered semantic command stream (Phase 4).
 *
 * The audio layer NEVER infers actions by diffing reducer snapshots: repeated
 * restarts, optimistic multi-tap actions, rollbacks and duplicate identical
 * commands are all lost by diffing. Instead the reducer appends an ordered,
 * monotonically-numbered command list to its own (still serializable) state and
 * the AudioEngine drains it by watermark.
 *
 * The reducer remains authoritative for project state; the engine is
 * authoritative for what is actually audible, and answers with an ack.
 */

export type AudioCommandType =
  | "transport.play"
  | "transport.stop"
  | "transport.restart"
  | "transport.cue"
  | "transport.scrub"
  | "transport.scrub.start"
  | "transport.scrub.end"
  | "transport.scrub.speed"
  | "track.mute"
  | "track.unmute"
  | "track.gain"
  | "track.delete"
  | "track.restore"
  | "master.gain"
  | "rate.set"
  | "loop.set"
  | "loop.chop"
  | "tape.reverse"
  | "filter.set"
  | "song.load"
  // Phase 5C — stem performance layer. Same ordered command + ack path.
  | "stem.select"
  | "stem.solo"
  | "stem.link"
  | "fx.overlay"
  | "fx.momentary.start"
  | "fx.momentary.end"
  | "fx.variation"
  | "fx.bank.select"
  | "fx.algorithm.cycle"
  | "fx.macro"

  | "fx.latch"
  | "fx.clearLatches"
  // Phase 6 — grid and export.
  | "grid.tap"
  | "grid.quantise"
  // Heads mode is a first-class Stem Tape v1 feature, not a reserved gesture.
  // ---- Heads Mode v2: four INDEPENDENT lane heads --------------------------
  // Head N reads lane N and plays on its own clock. `heads.source` and
  // `heads.print` no longer exist: there is nothing to assign and nothing to
  // bake. Every command below addresses ONE head by lane index.
  | "heads.enter"
  | "heads.exit"
  | "heads.level"
  | "heads.mute"
  /** payload.mask = "0110" of Tracks held right now; "" ends the momentary hold. */
  | "heads.play.hold"
  | "heads.latch"
  | "heads.loop.capture"
  | "heads.loop.resize"
  | "heads.reverse"
  | "heads.scrub"
  // ---- Universal lane layer (corrective production task) -------------------
  // These are LANE commands: one implementation serves Tape, Heads and the FX
  // overlay. `heads.reverse` has been deleted; `lane.reverse` replaces it.
  /** Momentary audition (bare Track hold). payload.mask = "0110" or "" to end. */
  | "lane.audition"
  | "loop.capture"
  | "loop.release"
  | "loop.resize"
  | "lane.reverse"
  | "lane.scrub.start"
  | "lane.scrub.end"
  | "lane.scrub.park"
  // ---- Global (all-four-stems) one-bar loop --------------------------------
  // Hold PLAY while the transport runs. It is a SEPARATE object from the per
  // lane loops: a lane loop keeps its own audible pointer, the global loop
  // supplies the hidden song target that every unlooped lane follows.
  /** payload.division = 1 | 2 | 4 | 8 (bar fraction). */
  | "loop.global.start"
  | "loop.global.release"
  /** FUNCTION + Volume ± while the global loop exists. */
  | "loop.global.resize"
  /** PLAY held + rocker: nudge the global loop window by one division. */
  | "loop.global.move"
  /** Selection arm / active-track choice (FUNCTION tap, then a Track tap). */
  | "stem.select"

  // ---- Stem Instrument Mode (MIDI cues) ------------------------------------
  // ONE ingress command per normalized MIDI event. The engine — not the
  // reducer — decides learn vs play, because only the engine knows the frame
  // the event landed on and whether the tape was eligible at that instant.
  | "cue.event"
  /** All Notes Off / device disconnect / backgrounding. */
  | "cue.panic"

  | "rollback";





export interface AudioCommand {
  /** Monotonic, never reused — the drain watermark. */
  id: number;
  /** performance.now() at emit time. */
  t: number;
  type: AudioCommandType;
  payload: Readonly<Record<string, number | string | boolean | null>>;
  /** Set when this command belongs to an optimistic multi-tap transaction. */
  txnId?: string;
  /** The v2.6 row that produced it, for diagnostics. */
  rowId?: string;
}

export type AckStatus = "accepted" | "completed" | "rejected" | "failed";

export interface Ack {
  id: number;
  type: AudioCommandType;
  status: AckStatus;
  detail: string;
  t: number;
}

let seq = 0;

export function nextCommandId(): number {
  return ++seq;
}

export function makeCommand(
  type: AudioCommandType,
  payload: AudioCommand["payload"],
  opts: { txnId?: string; rowId?: string; t?: number } = {},
): AudioCommand {
  const cmd: AudioCommand = {
    id: nextCommandId(),
    t: opts.t ?? (typeof performance !== "undefined" ? performance.now() : Date.now()),
    type,
    payload,
  };
  if (opts.txnId) cmd.txnId = opts.txnId;
  if (opts.rowId) cmd.rowId = opts.rowId;
  return cmd;
}

export const COMMAND_LOG_LIMIT = 200;
