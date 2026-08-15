/**
 * Phase 5C — ordered chord arbitration (binding correction 1).
 *
 * Authoritative flow:
 *
 *   raw pointer/key input → ordered chord arbitration → ONE semantic command
 *                          → reducer + audio
 *
 * Base Play / Volume / Track commands are never emitted-then-undone by this
 * layer: the arbiter consumes the raw transitions first and marks the controls
 * it claimed, so the v2.6 gesture consumer drops them before dispatch.
 * `TxnSnapshot` rollback stays in place ONLY as the safety fallback for
 * genuinely optimistic multi-tap sequences and for lost pointers.
 *
 * Precedence (highest first):
 *   1. cancel / safety
 *   2. long system chords (Vol− + Vol+ ≈ 2 s pairing)
 *   3. Play-first chords    (Play + Vol−/+, Play + Track)
 *   4. Function-first chords (FX track + Function)
 *   5. FX-track-first chords (FX track + Vol−/+)
 *   6. bare FX-overlay track (momentary FX)
 *   7. bare v2.6
 */

import type { Control } from "@/device/geometry";
import type { RawInputEvent } from "@/input/gestures";
import { bankOfButton, type BankIndex } from "./fx12";
import { type StemIndex } from "./stemPerformance";

export interface ArbiterTimings {
  /** The second control of a chord must arrive within this of the first. */
  modifierArrivalMs: number;
  /** Play + Track: overlap shorter than this is solo, longer is link/unlink. */
  soloLinkMs: number;
  /** Vol− + Vol+ both released before this = overlay toggle. */
  overlayShortMs: number;
  /** Vol− + Vol+ held at least this = the existing pairing gesture. */
  pairingMs: number;
  /** Volume held this long inside FX mode starts macro adjustment. */
  macroHoldMs: number;
  /** Macro repeat interval once macro adjustment has started. */
  macroRepeatMs: number;
}

export const DEFAULT_ARBITER_TIMINGS: ArbiterTimings = {
  modifierArrivalMs: 400,
  soloLinkMs: 700,
  overlayShortMs: 600,
  pairingMs: 2000,
  macroHoldMs: 450,
  macroRepeatMs: 120,
};

export type PerfIntent =
  | { type: "stem.select"; dir: 1 | -1 }
  | { type: "stem.solo"; stem: StemIndex; overlapMs: number }
  | { type: "stem.link"; stem: StemIndex; overlapMs: number }
  | { type: "fx.overlay"; on: boolean }
  | { type: "system.pairing" }
  | { type: "system.noop"; detail: string }
  // Twelve-FX intents. Selection + momentary fire on POINTER-DOWN: there is no
  // hold threshold between touching a bank button and hearing the effect.
  | { type: "fx.bank.select"; stem: StemIndex; bank: BankIndex }
  | { type: "fx.momentary.start"; stem: StemIndex; bank: BankIndex }
  | { type: "fx.momentary.end"; stem: StemIndex; bank: BankIndex }
  | { type: "fx.algorithm.cycle"; stem: StemIndex; bank: BankIndex; dir: 1 | -1 }
  | { type: "fx.macro"; stem: StemIndex; bank: BankIndex; dir: 1 | -1 }
  | { type: "fx.latch"; stem: StemIndex; bank: BankIndex }
  | { type: "fx.clearLatches"; stem: StemIndex };


export interface ArbitrationRecord {
  t: number;
  controls: Control[];
  intent: PerfIntent["type"] | "none";
  suppressed: Control[];
  detail: string;
}

const TRACK_INDEX: Record<string, number> = {
  "track-button-1": 0,
  "track-button-2": 1,
  "track-button-3": 2,
  "track-button-4": 3,
};

function isTrack(c: Control): boolean {
  return c in TRACK_INDEX;
}
function isVolume(c: Control): c is "volume-minus" | "volume-plus" {
  return c === "volume-minus" || c === "volume-plus";
}
function isRocker(c: Control): c is "rocker-fwd" | "rocker-rwd" {
  return c === "rocker-fwd" || c === "rocker-rwd";
}


export interface ArbiterView {
  activeStem: StemIndex;
  fxOverlay: boolean;
  /** Which bank owns Volume ± right now. null = base master volume. */
  selectedBank: BankIndex | null;
}


/**
 * Stateful, framework-free. Fed raw transitions BEFORE the gesture engine's
 * semantic output reaches the reducer.
 */
export class ChordArbiter {
  timings: ArbiterTimings = { ...DEFAULT_ARBITER_TIMINGS };
  readonly log: ArbitrationRecord[] = [];

  private down = new Map<Control, number>();
  /** Controls claimed by a chord: their base v2.6 gesture must be dropped. */
  private claimed = new Set<Control>();
  private listeners = new Set<(i: PerfIntent) => void>();
  /** Volume held inside FX mode: macro repeat timers, per control. */
  private macroTimers = new Map<Control, ReturnType<typeof setInterval> | ReturnType<typeof setTimeout>>();
  /** Volume presses that became a macro hold — their release must not cycle. */
  private macroFired = new Set<Control>();

  constructor(private view: () => ArbiterView) {}

  onIntent(fn: (i: PerfIntent) => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  private emit(intent: PerfIntent, controls: Control[], detail: string) {
    this.log.unshift({ t: Date.now(), controls, intent: intent.type, suppressed: [...controls], detail });
    if (this.log.length > 60) this.log.length = 60;
    for (const l of this.listeners) l(intent);
  }

  private claim(...controls: Control[]) {
    for (const c of controls) this.claimed.add(c);
  }

  /** True while this control's press has been consumed by a chord. */
  isClaimed(control: Control): boolean {
    return this.claimed.has(control);
  }

  reset() {
    this.down.clear();
    this.claimed.clear();
    this.clearMacro();
  }

  private clearMacro(control?: Control) {
    const controls = control ? [control] : [...this.macroTimers.keys()];
    for (const c of controls) {
      const timer = this.macroTimers.get(c);
      if (timer != null) clearTimeout(timer as ReturnType<typeof setTimeout>);
      if (timer != null) clearInterval(timer as ReturnType<typeof setInterval>);
      this.macroTimers.delete(c);
      this.macroFired.delete(c);
    }
  }

  /** Feed every raw transition here, in order. */
  handle(e: RawInputEvent): void {
    if (e.phase === "down") return this.onDown(e.control, e.t);
    if (e.phase === "cancel") return this.onCancel(e.control);
    return this.onUp(e.control, e.t);
  }

  private heldSince(c: Control, now: number): number | null {
    const at = this.down.get(c);
    if (at == null) return null;
    return now - at;
  }

  private modifierFresh(c: Control, now: number): boolean {
    const held = this.heldSince(c, now);
    return held != null && held <= this.timings.modifierArrivalMs;
  }

  private activeFxTrackHeld(): Control | null {
    for (const c of Object.keys(TRACK_INDEX) as Control[]) if (this.down.has(c)) return c;
    return null;
  }

  private startMacroHold(control: Control, dir: 1 | -1, stem: StemIndex, bank: BankIndex) {
    this.clearMacro(control);
    const fire = () => {
      this.macroFired.add(control);
      this.emit({ type: "fx.macro", stem, bank, dir }, [control], `macro ${dir > 0 ? "+" : "−"} on bank ${bank + 1}`);
    };
    const start = setTimeout(() => {
      fire();
      const repeat = setInterval(fire, this.timings.macroRepeatMs);
      this.macroTimers.set(control, repeat);
    }, this.timings.macroHoldMs);
    this.macroTimers.set(control, start);
  }

  private onDown(control: Control, t: number) {
    // A claim lives until the control is pressed again: clearing it on release
    // would let the base v2.6 tap (which fires on release) slip through.
    this.claimed.delete(control);
    this.down.set(control, t);
    const { fxOverlay, activeStem, selectedBank } = this.view();

    if (fxOverlay && isTrack(control)) {
      const bank = bankOfButton(TRACK_INDEX[control]!);
      const functionHeld = this.down.has("function");
      const otherModifier = this.down.has("play") || this.down.has("volume-minus") || this.down.has("volume-plus");
      if (functionHeld) {
        // ORDER IS THE DISCRIMINATOR. FUNCTION went down FIRST, so this press
        // belongs to the universal lane layer (FN + Track double-tap =
        // lane.reverse, FN + Track held + Volume = loop.resize). The arbiter
        // must NOT claim it and must NOT emit any FX intent: latching is
        // Track-first-then-FUNCTION only, resolved in onDown("function").
        return;
      }
      if (!otherModifier) {
        // Zero hold latency: select AND sound on pointer-down.
        this.claim(control);
        this.emit({ type: "fx.bank.select", stem: activeStem, bank }, [control], `bank ${bank + 1} selected`);
        this.emit(
          { type: "fx.momentary.start", stem: activeStem, bank },
          [control],
          `momentary bank ${bank + 1} on stem ${activeStem + 1}`,
        );
      }
      return;
    }

    if (fxOverlay && isVolume(control)) {
      const other: Control = control === "volume-minus" ? "volume-plus" : "volume-minus";
      if (this.down.has(other)) {
        // Vol− + Vol+ is the overlay/system chord — never a cycle or a macro.
        this.clearMacro(other);
        this.clearMacro(control);
        this.claim(control, other);
        return;
      }
      if (selectedBank != null) {
        // Claimed BEFORE dispatch: no master-volume command is ever emitted.
        this.claim(control);
        this.startMacroHold(control, control === "volume-plus" ? 1 : -1, activeStem, selectedBank);
        return;
      }
    }

    // Play + Rocker = the chop family (Stem Tape extension, supersedes the v2.6
    // `rocker.chop` row). The deflection claims PLAY the instant it arrives, so
    // the pending Play tap transaction is cancelled BEFORE dispatch and no
    // transport.play / transport.stop / transport.cue can leak out.
    if (isRocker(control) && this.down.has("play")) {
      this.claim("play");
      this.log.unshift({
        t,
        controls: ["play", control],
        intent: "none",
        suppressed: ["play"],
        detail: "PLAY claimed by rocker deflection — chop family, transport suppressed",
      });
      if (this.log.length > 60) this.log.length = 60;
    }

    // PLAY-first chords are RETIRED (PLAY + Volume = stem.select, PLAY + Track
    // = solo/link). Hold PLAY is exclusively the global one-bar loop, which
    // begins at holdMs = 450 — a >=700 ms link overlap could never complete and
    // a solo could be cancelled mid-gesture. Nothing is claimed here any more,
    // so Volume stays master volume and Track keeps its own deferred group.
    void this.modifierFresh;

    if (control === "function" && fxOverlay) {
      const heldTrack = this.activeFxTrackHeld();
      if (heldTrack) {
        const allFour = (Object.keys(TRACK_INDEX) as Control[]).every((c) => this.down.has(c));
        this.claim("function", heldTrack);
        if (allFour) {
          this.emit(
            { type: "fx.clearLatches", stem: activeStem },
            ["function", heldTrack],
            "all four bank buttons + FUNCTION — latches cleared",
          );
        } else {
          const bank = bankOfButton(TRACK_INDEX[heldTrack]!);
          this.emit({ type: "fx.latch", stem: activeStem, bank }, ["function", heldTrack], `latch toggle bank ${bank + 1}`);
        }
      }
    }
  }

  private onCancel(control: Control) {
    // Precedence 1 — safety. A lost pointer releases every claim on it.
    this.down.delete(control);
    this.clearMacro(control);
    const { fxOverlay, activeStem } = this.view();
    if (fxOverlay && isTrack(control) && this.claimed.has(control)) {
      const bank = bankOfButton(TRACK_INDEX[control]!);
      this.emit({ type: "fx.momentary.end", stem: activeStem, bank }, [control], "pointer cancel — momentary released");
    }
  }


  private onUp(control: Control, t: number) {
    const downAt = this.down.get(control);
    this.down.delete(control);
    const { fxOverlay, activeStem } = this.view();
    const heldMs = downAt == null ? 0 : t - downAt;

    // ---- precedence 2: long system chord (Vol− + Vol+)
    if (isVolume(control)) {
      const other: Control = control === "volume-minus" ? "volume-plus" : "volume-minus";
      const otherAt = this.down.get(other);
      if (otherAt != null && downAt != null) {
        const overlap = t - Math.max(otherAt, downAt);
        const arrival = Math.abs(otherAt - downAt);
        this.claim(control, other);
        if (arrival > this.timings.modifierArrivalMs) {
          this.emit({ type: "system.noop", detail: `volume chord arrival ${arrival.toFixed(0)} ms — ignored` }, [control, other], "arrival too wide");
          return;
        }
        if (overlap >= this.timings.pairingMs) {
          this.emit({ type: "system.pairing" }, [control, other], `pairing gesture (${overlap.toFixed(0)} ms)`);
        } else if (overlap < this.timings.overlayShortMs) {
          this.emit({ type: "fx.overlay", on: !fxOverlay }, [control, other], `FX overlay ${fxOverlay ? "closed" : "opened"} (${overlap.toFixed(0)} ms)`);
        } else {
          this.emit(
            { type: "system.noop", detail: `ambiguous volume chord ${overlap.toFixed(0)} ms (600–2000 ms band)` },
            [control, other],
            "diagnostics-only no-op",
          );
        }
        return;
      }
    }

    // ---- precedence 3: Play-first
    const playAt = this.down.get("play");
    if (playAt != null && downAt != null && Math.abs(downAt - playAt) <= this.timings.modifierArrivalMs) {
      if (isVolume(control)) {
        this.claim("play", control);
        this.emit(
          { type: "stem.select", dir: control === "volume-plus" ? 1 : -1 },
          ["play", control],
          `stem select ${control === "volume-plus" ? "+1" : "−1"}`,
        );
        return;
      }
      if (isTrack(control)) {
        // Correction 1: duration is the OVERLAP of the two controls, not the
        // time since the initial Play press.
        const overlap = t - Math.max(playAt, downAt);
        const stem = TRACK_INDEX[control]! as StemIndex;
        this.claim("play", control);
        if (overlap < this.timings.soloLinkMs) {
          this.emit({ type: "stem.solo", stem, overlapMs: overlap }, ["play", control], `solo (overlap ${overlap.toFixed(0)} ms)`);
        } else {
          this.emit({ type: "stem.link", stem, overlapMs: overlap }, ["play", control], `link/unlink (overlap ${overlap.toFixed(0)} ms)`);
        }
        return;
      }
    }

    // ---- precedence 4: FX chords already resolved on press; releases only end
    // momentary sound and clear macro repeat.
    if (fxOverlay) {
      if (isVolume(control)) {
        const fired = this.macroFired.has(control);
        this.clearMacro(control);
        if (fired) return; // macro hold — no algorithm cycle on release
        if (this.claimed.has(control)) {
          const { selectedBank } = this.view();
          if (selectedBank != null) {
            this.emit(
              { type: "fx.algorithm.cycle", stem: activeStem, bank: selectedBank, dir: control === "volume-plus" ? 1 : -1 },
              [control],
              `algorithm cycle ${control === "volume-plus" ? "+1" : "−1"} on bank ${selectedBank + 1}`,
            );
          }
          return;
        }
      }

      // ---- precedence 5: bare momentary release
      if (isTrack(control) && this.claimed.has(control)) {
        const bank = bankOfButton(TRACK_INDEX[control]!);
        this.emit(
          { type: "fx.momentary.end", stem: activeStem, bank },
          [control],
          `momentary bank ${bank + 1} released after ${heldMs.toFixed(0)} ms`,
        );
        return;
      }
    }


    // ---- precedence 7: bare v2.6 — nothing claimed, the base map runs.
  }
}
