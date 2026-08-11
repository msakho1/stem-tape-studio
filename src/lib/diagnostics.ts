/**
 * Harness bridge.
 *
 * A browser-driven acceptance pass has to read what the audio graph is actually
 * doing — read pointers, live path counts, control-bus batches, wet-path output
 * — not what the UI claims. This module publishes exactly those observations on
 * `window.__stemTape`. It never drives the instrument: every gesture in a
 * harness run still goes through real pointer and keyboard events.
 */

import { getAudioEngine, type AudioEngine } from "@/audio/engine";
import { controlBus, type ContinuousEvent } from "@/audio/controlBus";
import { buildAlgorithm } from "@/audio/fx/banks";
import type { SurfaceState } from "@/machine/surface";
import type { AudioCommand } from "@/audio/commands";

export interface DiagnosticsBridge {
  engine: AudioEngine;
  /** Rolling tail of every continuous-control event the audio side received. */
  busLog: ContinuousEvent[];
  clearBusLog(): void;
  /** Latest reducer state (read-only snapshot for assertions). */
  surface: SurfaceState | null;
  /** Every semantic command emitted so far this session, in order. */
  commands: AudioCommand[];
  buildAlgorithm: typeof buildAlgorithm;
  /** Measured first-release → committed-tap latency for deferred controls. */
  tapLatencyMs: number[];
}

const BUS_LIMIT = 4000;

let installed = false;

function bridge(): DiagnosticsBridge {
  const w = window as unknown as { __stemTape?: DiagnosticsBridge };
  if (!w.__stemTape) {
    const log: ContinuousEvent[] = [];
    w.__stemTape = {
      engine: getAudioEngine(),
      busLog: log,
      clearBusLog: () => {
        log.length = 0;
      },
      surface: null,
      commands: [],
      buildAlgorithm,
      tapLatencyMs: [],
    };
  }
  return w.__stemTape;
}

/** Idempotent; safe to call from a React effect on every mount. */
export function installDiagnostics(): () => void {
  if (typeof window === "undefined") return () => {};
  const b = bridge();
  if (installed) return () => {};
  installed = true;
  const off = controlBus.subscribe((e) => {
    b.busLog.push(e);
    if (b.busLog.length > BUS_LIMIT) b.busLog.splice(0, b.busLog.length - BUS_LIMIT);
  });
  return () => {
    off();
    installed = false;
  };
}

/** Publish the current reducer state and command stream for the harness. */
export function publishSurface(state: SurfaceState) {
  if (typeof window === "undefined") return;
  const b = bridge();
  b.surface = state;
  b.commands = state.commands;
}

/** Publish the gesture engine's measured multi-tap decision latencies. */
export function publishTapLatency(samples: number[]) {
  if (typeof window === "undefined") return;
  bridge().tapLatencyMs = samples;
}
