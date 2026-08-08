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
  | "fx.latch"
  | "fx.clearLatches"
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
