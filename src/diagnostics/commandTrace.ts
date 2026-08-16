/**
 * Surface-command trace boundary.
 *
 * The reducer is pure: it never records. React StrictMode invokes reducers
 * (and effects) twice, so recording inside `emit()` duplicated every logical
 * command in the flight recorder. Instead the emitted command stream is
 * captured exactly ONCE here, at the dispatcher/consumer boundary, using a
 * monotonic command-id watermark. Re-running this with the same commands —
 * StrictMode double-effect, re-render, replay — records nothing new.
 *
 * Correlation and arbitration IDs are preserved: the ring stamps the currently
 * open `corr` / `gesture` IDs onto each record as it is written.
 */

import type { AudioCommand } from "@/audio/commands";
import { trace as globalTrace, type TraceRing } from "./trace";

export class SurfaceCommandTracer {
  private watermark = -1;
  constructor(private readonly ring: TraceRing = globalTrace) {}

  /** Records only commands newer than the watermark. Returns how many. */
  capture(commands: readonly AudioCommand[]): number {
    let n = 0;
    for (const c of commands) {
      if (c.id <= this.watermark) continue;
      this.watermark = c.id;
      n++;
      this.ring.record("command.surface", `emit ${c.type}`, {
        id: c.id,
        payload: c.payload as unknown,
      }, c.t);
    }
    return n;
  }

  reset(): void {
    this.watermark = -1;
  }
}

/** Process-wide tracer used by the live surface hook. */
export const surfaceCommandTracer = new SurfaceCommandTracer();
