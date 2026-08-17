/**
 * Physical Stem Tape SP-1 surface adapter.
 *
 * The M0 firmware enumerates as a class-compliant USB MIDI device named
 * `STEM TAPE SP-1 …` and transmits RAW physical state only. Those messages are
 * NOT musical input and must never reach the generic MIDI cue-learning system:
 * they are the same raw press / release / fader events the on-screen surface
 * and the keyboard produce, arriving over a wire.
 *
 * This module is pure decoding + held-state bookkeeping. It emits no audio
 * commands; `useDeviceSurface` feeds the events into the existing
 * GestureEngine / ChordArbiter / reducer path.
 */

import type { Control } from "@/device/geometry";
import { trace } from "@/diagnostics/trace";

/** Case-insensitive device recognition; suffixes such as "BLOCK 1" are allowed. */
export function isSp1DeviceName(name: string | null | undefined): boolean {
  return typeof name === "string" && name.toUpperCase().includes("STEM TAPE SP-1");
}

/** Firmware MIDI contract v1 — buttons, channel 1. */
export const SP1_NOTE_CONTROLS: Record<number, Control> = {
  36: "track-button-1",
  37: "track-button-2",
  38: "track-button-3",
  39: "track-button-4",
  40: "play",
  41: "function",
  42: "volume-plus",
  43: "volume-minus",
  44: "rocker-fwd",
  45: "rocker-rwd",
};

/** CC20..23 = faders 1..4. CC24 = battery telemetry only. */
export const SP1_FADER_CC: Record<number, 0 | 1 | 2 | 3> = { 20: 0, 21: 1, 22: 2, 23: 3 };
export const SP1_BATTERY_CC = 24;

export type Sp1Message =
  | { type: "down"; control: Control }
  | { type: "up"; control: Control }
  | { type: "fader"; index: 0 | 1 | 2 | 3; value: number }
  | { type: "battery"; value: number }
  | { type: "allOff" };

/** Raw bytes → one physical message, or null when the message is not part of the contract. */
export function decodeSp1Message(bytes: ArrayLike<number>): Sp1Message | null {
  if (bytes.length < 2) return null;
  const status = (bytes[0] ?? 0) & 0xff;
  const type = status & 0xf0;
  const d1 = (bytes[1] ?? 0) & 0x7f;
  const d2 = (bytes[2] ?? 0) & 0x7f;

  if (type === 0x90 || type === 0x80) {
    const control = SP1_NOTE_CONTROLS[d1];
    if (!control) return null;
    // Note On velocity 0 is a release, exactly like Note Off.
    const down = type === 0x90 && d2 > 0;
    return down ? { type: "down", control } : { type: "up", control };
  }
  if (type === 0xb0) {
    if (d1 === 123) return { type: "allOff" };
    if (d1 === SP1_BATTERY_CC) return { type: "battery", value: d2 };
    const index = SP1_FADER_CC[d1];
    if (index === undefined) return null;
    return { type: "fader", index, value: d2 / 127 };
  }
  return null;
}

export type Sp1SurfaceEvent =
  | { type: "down"; control: Control; timestampMs: number; deviceId: string; deviceName: string }
  | { type: "up"; control: Control; timestampMs: number; deviceId: string; deviceName: string }
  | { type: "fader"; index: 0 | 1 | 2 | 3; value: number; timestampMs: number; deviceId: string; deviceName: string }
  | { type: "battery"; value: number; timestampMs: number; deviceId: string; deviceName: string };

export type Sp1Listener = (ev: Sp1SurfaceEvent) => void;

const nowMs = () => (typeof performance !== "undefined" ? performance.now() : Date.now());

/**
 * A live physical press must never be rejected as stale, so the incoming
 * timestamp is only trusted when it is a finite, non-negative value that is not
 * in the future relative to the current `performance.now()` domain.
 */
function pageTime(ts: number | undefined): number {
  const now = nowMs();
  if (typeof ts !== "number" || !Number.isFinite(ts) || ts < 0 || ts > now + 1) return now;
  return ts;
}

export class Sp1SurfaceAdapter {
  private listeners = new Set<Sp1Listener>();
  /** deviceId → held controls. Per physical input, so two blocks cannot collide. */
  private held = new Map<string, Set<Control>>();
  private lastBattery: number | null = null;
  private connectedName: string | null = null;

  subscribe(fn: Sp1Listener): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  snapshot(): { deviceName: string | null; held: Control[]; battery: number | null } {
    const held: Control[] = [];
    for (const set of this.held.values()) held.push(...set);
    return { deviceName: this.connectedName, held, battery: this.lastBattery };
  }

  /** Returns true when the message belonged to the SP-1 contract (consumed). */
  handleBytes(
    bytes: ArrayLike<number>,
    device: { id: string; name: string },
    timestampMs?: number,
  ): boolean {
    const msg = decodeSp1Message(bytes);
    this.connectedName = device.name;
    if (!msg) return false;
    const t = pageTime(timestampMs);
    const base = { timestampMs: t, deviceId: device.id, deviceName: device.name };

    if (msg.type === "allOff") {
      this.releaseAll(device.id, t);
      return true;
    }
    if (msg.type === "battery") {
      this.lastBattery = msg.value;
      this.emit({ type: "battery", value: msg.value, ...base });
      return true;
    }
    if (msg.type === "fader") {
      this.emit({ type: "fader", index: msg.index, value: msg.value, ...base });
      return true;
    }

    const set = this.held.get(device.id) ?? new Set<Control>();
    this.held.set(device.id, set);
    if (msg.type === "down") {
      // Idempotent: a repeated Note On for a held control is not a second press.
      if (set.has(msg.control)) return true;
      set.add(msg.control);
      this.emit({ type: "down", control: msg.control, ...base });
      return true;
    }
    // Idempotent release.
    if (!set.delete(msg.control)) return true;
    this.emit({ type: "up", control: msg.control, ...base });
    return true;
  }

  /** CC123 / hot-unplug / blur: release every held control for this device. */
  releaseAll(deviceId?: string, timestampMs?: number): void {
    const t = pageTime(timestampMs);
    const ids = deviceId ? [deviceId] : [...this.held.keys()];
    for (const id of ids) {
      const set = this.held.get(id);
      if (!set || set.size === 0) continue;
      for (const control of [...set]) {
        set.delete(control);
        this.emit({
          type: "up",
          control,
          timestampMs: t,
          deviceId: id,
          deviceName: this.connectedName ?? "STEM TAPE SP-1",
        });
      }
    }
  }

  deviceDisconnected(deviceId: string): void {
    this.releaseAll(deviceId);
    this.held.delete(deviceId);
    if (this.held.size === 0) this.connectedName = null;
  }

  /**
   * Decode point. The record is written HERE, with the event's own timestamp,
   * so a 1200 ms hold shows 1200 ms between its DOWN and UP records. Nothing
   * downstream may reconstruct these from held state.
   */
  private emit(ev: Sp1SurfaceEvent): void {
    if (trace.enabled) {
      const label =
        ev.type === "fader"
          ? `FADER ${ev.index + 1} → ${(ev.value * 100).toFixed(0)}% (CC${20 + ev.index} / 0x${(20 + ev.index).toString(16).toUpperCase()})`
          : ev.type === "battery"
            ? `battery CC24 (0x18) = ${ev.value}`
            : `${ev.control.toUpperCase()} ${ev.type === "down" ? "DOWN" : "UP"}`;
      trace.record(
        "surface.decoded",
        label,
        {
          ...(ev.type === "fader" ? { index: ev.index, value: ev.value } : {}),
          ...(ev.type === "battery" ? { value: ev.value } : {}),
          ...(ev.type === "down" || ev.type === "up" ? { control: ev.control, phase: ev.type } : {}),
          source: "sp1-adapter",
          device: ev.deviceName,
        },
        { t: ev.timestampMs },
      );
      if (ev.type === "down" || ev.type === "up") {
        const all: Control[] = [];
        for (const set of this.held.values()) all.push(...set);
        trace.record("surface.held", `held: ${all.join(" + ") || "none"}`, { held: all }, { t: ev.timestampMs });
      }
    }
    for (const fn of this.listeners) fn(ev);
  }
}

export const sp1Surface = new Sp1SurfaceAdapter();

// Diagnostic seam: the browser smoke harness injects raw SP-1 bytes here.
// Read-only observation path — no behaviour depends on this handle.
if (typeof window !== "undefined") {
  (window as unknown as { __stemTapeSp1Surface?: Sp1SurfaceAdapter }).__stemTapeSp1Surface = sp1Surface;
}
