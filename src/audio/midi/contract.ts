/**
 * Normalized MIDI event contract — Checkpoint 1 of Stem Instrument Mode.
 *
 * ONE shape is shared by every transport:
 *   - desktop/Android Web MIDI (`webMidi.ts`)
 *   - iPhone/iPad native CoreMIDI bridge (`nativeBridge.ts`)
 *   - tests (`source: "test"`)
 *
 * This module is pure: no audio, no engine, no commands. Nothing here can emit
 * a single audio command, which is the checkpoint's central guarantee.
 */

export type MidiTransport = "webmidi" | "coremidi-bridge" | "test";

export type StemMidiEvent = {
  kind: "noteOn" | "noteOff" | "allNotesOff";
  note: number;
  velocity: number;
  /** 0..15 */
  channel: number;
  /** performance.now() domain, monotonic */
  timestampMs: number;
  source: MidiTransport;
  deviceId: string;
  deviceName: string;
};

export type MidiOrigin = {
  timestampMs: number;
  source: MidiTransport;
  deviceId: string;
  deviceName: string;
};

/** An event older than this is reported as stale (late JS callback / backgrounded tab). */
export const STALE_EVENT_MS = 250;

/** Learning/playback identity: channel + note. Overlapping pairs cannot corrupt each other. */
export function eventKey(ev: Pick<StemMidiEvent, "channel" | "note">): string {
  return `${ev.channel}:${ev.note}`;
}

export function isStale(ev: StemMidiEvent, nowMs: number, windowMs = STALE_EVENT_MS): boolean {
  return nowMs - ev.timestampMs > windowMs;
}

const clamp7 = (n: number) => Math.max(0, Math.min(127, Math.round(n)));

/**
 * Raw status/data bytes → StemMidiEvent.
 *
 * Normalizations required by the plan:
 *   - Note On with velocity 0 → noteOff
 *   - CC 123 (all notes off) → allNotesOff
 * Everything else (clock, aftertouch, program change, sysex, CC != 123) is
 * dropped by returning null.
 */
export function normalizeMidiBytes(
  bytes: ArrayLike<number>,
  origin: MidiOrigin,
): StemMidiEvent | null {
  if (bytes.length < 1) return null;
  const status = bytes[0] & 0xff;
  if (status < 0x80 || status >= 0xf0) return null; // system / running status: ignored
  const type = status & 0xf0;
  const channel = status & 0x0f;
  const d1 = bytes.length > 1 ? clamp7(bytes[1]) : 0;
  const d2 = bytes.length > 2 ? clamp7(bytes[2]) : 0;

  const base = {
    channel,
    timestampMs: origin.timestampMs,
    source: origin.source,
    deviceId: origin.deviceId,
    deviceName: origin.deviceName,
  };

  if (type === 0x90) {
    return d2 === 0
      ? { kind: "noteOff", note: d1, velocity: 0, ...base }
      : { kind: "noteOn", note: d1, velocity: d2, ...base };
  }
  if (type === 0x80) {
    return { kind: "noteOff", note: d1, velocity: d2, ...base };
  }
  if (type === 0xb0 && d1 === 123) {
    return { kind: "allNotesOff", note: 0, velocity: 0, ...base };
  }
  return null;
}

/** Synthetic all-notes-off, used for device disconnect and backgrounding. */
export function allNotesOffEvent(origin: MidiOrigin, channel = 0): StemMidiEvent {
  return {
    kind: "allNotesOff",
    note: 0,
    velocity: 0,
    channel,
    timestampMs: origin.timestampMs,
    source: origin.source,
    deviceId: origin.deviceId,
    deviceName: origin.deviceName,
  };
}

const KINDS = new Set(["noteOn", "noteOff", "allNotesOff"]);

/**
 * Native bridge payload → StemMidiEvent.
 *
 * The native side may deliver either already-decoded fields or raw bytes; both
 * are accepted, and velocity-0 / CC123 are normalized identically.
 */
export function normalizeNativeEvent(raw: unknown): StemMidiEvent | null {
  if (!raw || typeof raw !== "object") return null;
  const r = raw as Record<string, unknown>;
  const timestampMs = typeof r.timestampMs === "number" && Number.isFinite(r.timestampMs)
    ? r.timestampMs
    : NaN;
  if (!Number.isFinite(timestampMs)) return null;
  const origin: MidiOrigin = {
    timestampMs,
    source: "coremidi-bridge",
    deviceId: typeof r.deviceId === "string" ? r.deviceId : "coremidi",
    deviceName: typeof r.deviceName === "string" ? r.deviceName : "CoreMIDI device",
  };

  if (Array.isArray(r.bytes)) {
    return normalizeMidiBytes(r.bytes.map((b) => Number(b) | 0), origin);
  }

  const kind = typeof r.kind === "string" ? r.kind : "";
  if (!KINDS.has(kind)) return null;
  const note = clamp7(Number(r.note ?? 0));
  const velocity = clamp7(Number(r.velocity ?? 0));
  const channel = Math.max(0, Math.min(15, Number(r.channel ?? 0) | 0));
  if (kind === "allNotesOff") return allNotesOffEvent(origin, channel);
  if (kind === "noteOn" && velocity === 0) {
    return { kind: "noteOff", note, velocity: 0, channel, ...origin };
  }
  return { kind: kind as StemMidiEvent["kind"], note, velocity, channel, ...origin };
}

export function describeEvent(ev: StemMidiEvent): string {
  if (ev.kind === "allNotesOff") return `all notes off · ch${ev.channel + 1}`;
  return `${ev.kind === "noteOn" ? "on" : "off"} ${ev.note} v${ev.velocity} · ch${ev.channel + 1}`;
}
