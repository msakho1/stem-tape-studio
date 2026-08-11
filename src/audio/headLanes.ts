/**
 * Heads Mode as a FOUR-LANE INSTRUMENT (corrective task).
 *
 * The old model was "four virtual heads reading ONE source track". That is
 * gone. Head N now reads LANE N (stem N) and plays on its own clock, wholly
 * independent of the main transport: heads can sound while the song is paused,
 * and the song's playhead never moves because a head moved.
 *
 * Truth model
 *  - Every head position is DERIVED from ctx.currentTime through its own
 *    anchor (anchorCtx / anchorPos), never ticked by a timer.
 *  - Audibility is a pure function of (heldMask, latched, muted): a momentary
 *    hold soloes exactly the held group and the release restores the previous
 *    mix bit-for-bit because nothing about the previous mix was overwritten.
 *  - A head with no loop stops at the end of its source rather than wrapping.
 */

/**
 * Entry descriptor: which stem the four heads read, and its display name.
 */
export interface HeadsEntry {
  source: number;
  sourceName: string;
}


export interface HeadLaneSnapshot {
  lane: number;
  /** Audible + moving right now. */
  playing: boolean;
  /** Triple-tap independent playback. */
  latched: boolean;
  /** Held under a momentary Track hold. */
  held: boolean;
  muted: boolean;
  reverse: boolean;
  level: number;
  position: number;
  loop: { startS: number; lengthS: number; bars: number } | null;
  scrubCandidate: number | null;
  /** True while the head has run past its source end with no loop. */
  ended: boolean;
}

interface LaneRuntime {
  latched: boolean;
  held: boolean;
  muted: boolean;
  reverse: boolean;
  level: number;
  /** Parked position, seconds in the source. Authoritative while stopped. */
  posS: number;
  moving: boolean;
  anchorCtx: number;
  anchorPos: number;
  loop: { startS: number; lengthS: number; bars: number } | null;
  scrubCandidate: number | null;
  ended: boolean;
  voice: { node: AudioBufferSourceNode; gain: GainNode } | null;
}

export interface HeadLaneHost {
  ctx(): AudioContext | null;
  /** Where the heads bus lands. Deliberately NOT the per-track chain: heads
   *  must survive stem mutes and the transport being stopped. */
  destination(): AudioNode | null;
  /** Per-lane landing node: lane N's head rides stem N's FX chain, entering
   *  AFTER the stem's mute gate so a muted stem still passes its head. */
  laneDestination(lane: number): AudioNode | null;
  buffer(lane: number): AudioBuffer | null;
  reversed(lane: number): AudioBuffer | null;
  barSeconds(): number;
  /** Song position used as the default parking spot on entry. */
  songPosition(): number;
}

const FADE_S = 0.008;
const GRAIN_S = 0.07;

function mod(x: number, n: number): number {
  return ((x % n) + n) % n;
}

function freshLane(): LaneRuntime {
  return {
    latched: false,
    held: false,
    muted: false,
    reverse: false,
    level: 0.85,
    posS: 0,
    moving: false,
    anchorCtx: 0,
    anchorPos: 0,
    loop: null,
    scrubCandidate: null,
    ended: false,
    voice: null,
  };
}

export class HeadLanes {
  active = false;
  private lanes: LaneRuntime[] = [0, 1, 2, 3].map(freshLane);
  private bus: GainNode | null = null;
  /** One gain per lane, landing in that lane's own FX chain. */
  private laneBuses: (GainNode | null)[] = [null, null, null, null];
  private tap: AnalyserNode | null = null;
  private scrubbing: (boolean | null)[] = [null, null, null, null];
  /** Ordered evidence trail; the browser proof reads it verbatim. */
  readonly log: { t: number; event: string; lane: number; detail: string }[] = [];

  constructor(private host: HeadLaneHost) {}

  private note(event: string, lane: number, detail: string) {
    this.log.push({ t: Date.now(), event, lane, detail });
    if (this.log.length > 200) this.log.shift();
  }

  // ------------------------------------------------------------- lifecycle

  enter(entries?: HeadEntryLane[]): { ok: boolean; detail: string } {
    const ctx = this.host.ctx();
    this.note("heads.enter.requested", -1, `entry requested — ctx ${ctx ? ctx.state : "absent"}`);
    if (!ctx) {
      this.note("heads.enter.rejected", -1, "audio not unlocked");
      return { ok: false, detail: "audio not unlocked" };
    }
    if (this.active) return { ok: true, detail: "heads already active" };
    const loaded = [0, 1, 2, 3].filter((i) => this.host.buffer(i) != null);
    if (loaded.length === 0) {
      this.note("heads.enter.rejected", -1, "no decoded lane to read");
      return { ok: false, detail: "heads rejected — no decoded lane to read" };
    }
    const dest0 = this.host.destination();
    if (!dest0) {
      this.note("heads.enter.rejected", -1, "no output bus");
      return { ok: false, detail: "heads rejected — no output bus" };
    }
    const bus = ctx.createGain();
    bus.gain.value = 1;
    bus.connect(dest0);
    const tap = ctx.createAnalyser();
    tap.fftSize = 2048;
    bus.connect(tap);
    this.bus = bus;
    this.tap = tap;
    this.laneBuses = [0, 1, 2, 3].map((i) => {
      const g = ctx.createGain();
      g.gain.value = 1;
      const dest = this.host.laneDestination(i) ?? dest0;
      g.connect(dest);
      // Measurement tap only — the audible path is the lane FX chain.
      g.connect(tap);
      return g;
    });
    // POSITION FIRST. Every lane is parked exactly where the song is before any
    // moving/latch/anchor state is written, so no stale anchor can recompute an
    // entry position (the reverse-continuity bug).
    const at = Math.max(0, this.host.songPosition());
    this.lanes = [0, 1, 2, 3].map(() => {
      const l = freshLane();
      l.posS = at;
      return l;
    });
    this.active = true;
    // Musical continuity: a stem that was audible at the instant of entry keeps
    // sounding as its own head from `at`. Muted/inactive stems (and everything,
    // when the transport is paused) stay parked at the same position.
    const carried: number[] = [];
    for (let i = 0; i < 4; i++) {
      if (entries?.[i]?.audible && this.host.buffer(i) != null) {
        this.lanes[i]!.latched = true;
        carried.push(i + 1);
      }
    }
    if (carried.length > 0) this.reconcile("entry continuation");
    const detail =
      carried.length > 0
        ? `heads on — heads ${carried.join("+")} carried the playing stems from ${at.toFixed(3)}s, the rest parked there`
        : `heads on — head N reads lane N, parked at ${at.toFixed(3)}s`;
    this.note("heads.enter.accepted", -1, detail);
    for (let i = 0; i < 4; i++) {
      const l = this.lanes[i]!;
      this.note("head.state", i, `pos ${l.posS.toFixed(3)}s moving ${l.moving} latched ${l.latched} muted ${l.muted} reverse ${l.reverse}`);
    }
    return { ok: true, detail };
  }


  exit(): { ok: boolean; detail: string } {
    if (!this.active) return { ok: true, detail: "heads already off" };
    for (let i = 0; i < 4; i++) this.stopLane(i, "exit");
    for (const g of this.laneBuses) {
      try {
        g?.disconnect();
      } catch {
        /* noop */
      }
    }
    this.laneBuses = [null, null, null, null];
    if (this.bus) {
      try {
        this.bus.disconnect();
        this.tap?.disconnect();
      } catch {
        /* noop */
      }
    }
    this.bus = null;
    this.tap = null;
    this.active = false;
    this.lanes = [0, 1, 2, 3].map(freshLane);
    this.note("heads.exit", -1, "heads off — stem mixer restored, transport untouched");
    return { ok: true, detail: "heads off — four lanes released, transport untouched" };
  }

  // ------------------------------------------------------------- positions

  duration(i: number): number {
    return this.host.buffer(i)?.duration ?? 0;
  }

  position(i: number): number {
    const l = this.lanes[i];
    if (!l) return 0;
    const ctx = this.host.ctx();
    if (!l.moving || !ctx) return l.posS;
    const dir = l.reverse ? -1 : 1;
    const raw = l.anchorPos + (ctx.currentTime - l.anchorCtx) * dir;
    const dur = this.duration(i);
    if (l.loop) {
      const looped = l.loop.startS + mod(raw - l.loop.startS, Math.max(1e-4, l.loop.lengthS));
      return Math.min(Math.max(0, looped), Math.max(0, dur));
    }
    return Math.min(Math.max(0, raw), Math.max(0, dur));
  }

  /** Called from the status poll: parks any unlooped head that ran off its end. */
  tick() {
    if (!this.active) return;
    const ctx = this.host.ctx();
    if (!ctx) return;
    for (let i = 0; i < 4; i++) {
      const l = this.lanes[i]!;
      if (!l.moving || l.loop) continue;
      const dur = this.duration(i);
      const dir = l.reverse ? -1 : 1;
      const raw = l.anchorPos + (ctx.currentTime - l.anchorCtx) * dir;
      if (raw >= dur || raw <= 0) {
        l.posS = Math.min(Math.max(0, raw), dur);
        l.ended = true;
        this.stopLane(i, "source end");
        l.latched = false;
        l.held = false;
        this.note("heads.end", i, `head ${i + 1} reached the source end with no loop — stopped at ${l.posS.toFixed(3)}s`);
      }
    }
  }

  // --------------------------------------------------------------- voicing

  /** Should head i be audible given the current hold/latch/mute picture? */
  private audible(i: number): boolean {
    const l = this.lanes[i]!;
    const anyHeld = this.lanes.some((x) => x.held);
    if (anyHeld) return l.held; // momentary group solo — hear exactly that group
    return l.latched && !l.muted;
  }

  private shouldMove(i: number): boolean {
    const l = this.lanes[i]!;
    return l.held || l.latched;
  }

  private startLane(i: number, why: string) {
    const ctx = this.host.ctx();
    const l = this.lanes[i]!;
    const laneBus = this.laneBuses[i] ?? this.bus;
    if (!ctx || !laneBus || l.moving) return;
    const buf = this.host.buffer(i);
    if (!buf) return;
    if (l.ended && !l.loop && l.posS >= buf.duration - 1e-3) l.posS = 0;
    l.ended = false;
    const at = ctx.currentTime + 0.01;
    const gain = ctx.createGain();
    gain.connect(laneBus);
    const node = ctx.createBufferSource();
    const dur = buf.duration;
    const rev = l.reverse;
    const src = rev ? this.host.reversed(i) ?? buf : buf;
    node.buffer = src;
    let offset = rev ? dur - l.posS : l.posS;
    if (l.loop) {
      const s = l.loop.startS;
      const e = Math.min(dur, s + l.loop.lengthS);
      node.loop = true;
      node.loopStart = rev ? Math.max(0, dur - e) : Math.max(0, s);
      node.loopEnd = rev ? Math.min(dur, dur - s) : Math.min(dur, Math.max(s + 1e-3, e));
      offset = Math.min(Math.max(node.loopStart, offset), node.loopEnd - 1e-4);
    }
    node.connect(gain);
    const level = this.audible(i) ? l.level : 0;
    gain.gain.setValueAtTime(0, at);
    gain.gain.linearRampToValueAtTime(level, at + FADE_S);
    node.start(at, Math.min(Math.max(0, offset), Math.max(0, src.duration - 1e-4)));
    l.voice = { node, gain };
    l.moving = true;
    l.anchorCtx = at;
    l.anchorPos = l.posS;
    this.note("heads.play", i, `head ${i + 1} playing from ${l.posS.toFixed(3)}s (${why}${rev ? ", reverse" : ""}${l.loop ? `, ${l.loop.bars} bar loop` : ""})`);
  }

  private stopLane(i: number, why: string) {
    const l = this.lanes[i]!;
    const ctx = this.host.ctx();
    if (!l.moving && !l.voice) return;
    l.posS = this.position(i);
    l.moving = false;
    const v = l.voice;
    l.voice = null;
    if (v && ctx) {
      const at = ctx.currentTime;
      try {
        v.gain.gain.cancelScheduledValues(at);
        v.gain.gain.setValueAtTime(v.gain.gain.value, at);
        v.gain.gain.linearRampToValueAtTime(0, at + FADE_S);
        v.node.stop(at + FADE_S * 2);
      } catch {
        /* already stopped */
      }
      setTimeout(() => {
        try {
          v.node.disconnect();
          v.gain.disconnect();
        } catch {
          /* noop */
        }
      }, 200);
    }
    this.note("heads.pause", i, `head ${i + 1} paused at ${l.posS.toFixed(3)}s (${why})`);
  }

  /** Reconcile every lane's voice + gain with the current logical state. */
  private reconcile(why: string) {
    const ctx = this.host.ctx();
    for (let i = 0; i < 4; i++) {
      const l = this.lanes[i]!;
      if (this.shouldMove(i) && !l.moving) this.startLane(i, why);
      else if (!this.shouldMove(i) && l.moving) this.stopLane(i, why);
      const v = l.voice;
      if (v && ctx) {
        const target = this.scrubbing[i] ? 0 : this.audible(i) ? l.level : 0;
        v.gain.gain.cancelScheduledValues(ctx.currentTime);
        v.gain.gain.setValueAtTime(v.gain.gain.value, ctx.currentTime);
        v.gain.gain.linearRampToValueAtTime(target, ctx.currentTime + FADE_S);
      }
    }
  }

  // ------------------------------------------------------------- gestures

  /** Momentary hold. `mask` is a 4-char "0110" of currently held Tracks. */
  setHeld(mask: string): { ok: boolean; detail: string } {
    if (!this.active) return { ok: false, detail: "heads mode is not active" };
    for (let i = 0; i < 4; i++) this.lanes[i]!.held = mask[i] === "1";
    this.reconcile(mask.includes("1") ? `momentary hold ${mask}` : "hold released");
    const playing = this.lanes.map((l, i) => (l.moving ? i + 1 : 0)).filter(Boolean);
    return {
      ok: true,
      detail: mask.includes("1")
        ? `momentary heads ${mask} — playing ${playing.join("+") || "none"}, latched heads silenced for the hold`
        : `hold released — momentary heads paused, previous heads mix restored (playing ${playing.join("+") || "none"})`,
    };
  }

  toggleLatch(i: number): { ok: boolean; detail: string } {
    if (!this.active) return { ok: false, detail: "heads mode is not active" };
    const l = this.lanes[i]!;
    l.latched = !l.latched;
    if (l.latched) l.muted = false;
    this.reconcile(l.latched ? "latched" : "unlatched");
    return { ok: true, detail: `head ${i + 1} independent playback ${l.latched ? "LATCHED" : "released"} — main transport untouched` };
  }

  toggleMute(i: number): { ok: boolean; detail: string } {
    if (!this.active) return { ok: false, detail: "heads mode is not active" };
    const l = this.lanes[i]!;
    if (l.loop) {
      // A single tap while looping releases the loop back into normal playback.
      l.loop = null;
      if (l.moving) {
        this.stopLane(i, "loop released");
        this.reconcile("loop released");
      }
      return { ok: true, detail: `head ${i + 1} loop released — back to straight playback from ${l.posS.toFixed(3)}s` };
    }
    l.muted = !l.muted;
    this.reconcile("mute");
    return { ok: true, detail: `head ${i + 1} ${l.muted ? "muted" : "unmuted"}` };
  }

  captureLoop(i: number, bars: number): { ok: boolean; detail: string } {
    if (!this.active) return { ok: false, detail: "heads mode is not active" };
    const l = this.lanes[i]!;
    if (l.loop) {
      l.loop = null;
      if (l.moving) {
        this.stopLane(i, "loop toggled off");
        this.reconcile("loop toggled off");
      }
      return { ok: true, detail: `head ${i + 1} loop released` };
    }
    const dur = this.duration(i);
    const lengthS = Math.min(Math.max(0.05, bars * this.host.barSeconds()), Math.max(0.05, dur));
    const raw = l.scrubCandidate != null ? l.scrubCandidate : this.position(i);
    // A loop must live INSIDE the source. Parking at the very end (a scrub
    // landing at duration) previously produced a loop window past the end,
    // which read silence and reported positions beyond the buffer.
    const start = Math.min(Math.max(0, raw), Math.max(0, dur - lengthS));
    l.scrubCandidate = null;
    l.posS = start;
    l.loop = { startS: start, lengthS, bars };
    if (l.moving) this.stopLane(i, "loop capture reseat");
    if (!l.latched && !l.held) l.latched = true; // a captured loop repeats
    this.reconcile("loop capture");
    return {
      ok: true,
      detail: `head ${i + 1} captured a ${bars} bar loop at ${start.toFixed(3)}s (${lengthS.toFixed(3)}s) — repeating independently`,
    };
  }

  resizeLoop(i: number, dir: 1 | -1): { ok: boolean; detail: string } {
    if (!this.active) return { ok: false, detail: "heads mode is not active" };
    const l = this.lanes[i]!;
    if (!l.loop) return { ok: false, detail: `head ${i + 1} has no loop to resize` };
    const bars = Math.max(0.25, Math.min(8, dir > 0 ? l.loop.bars * 2 : l.loop.bars / 2));
    l.loop = { startS: l.loop.startS, bars, lengthS: Math.max(0.05, bars * this.host.barSeconds()) };
    if (l.moving) {
      this.stopLane(i, "loop resize");
      l.posS = l.loop.startS;
      this.reconcile("loop resize");
    }
    return { ok: true, detail: `head ${i + 1} loop → ${bars} bar (${l.loop.lengthS.toFixed(3)}s)` };
  }

  toggleReverse(i: number): { ok: boolean; detail: string } {
    if (!this.active) return { ok: false, detail: "heads mode is not active" };
    const l = this.lanes[i]!;
    // Freeze the CURRENT (still forward) read position BEFORE flipping, and
    // re-assert it after stopLane. stopLane recomputes posS from the anchor,
    // and with the direction already flipped that recomputation ran backwards
    // from the old anchor and clamped the head to 0.
    const at = this.position(i);
    l.reverse = !l.reverse;
    if (l.moving) {
      this.stopLane(i, "reverse");
      l.posS = at;
      this.reconcile("reverse");
    } else {
      l.posS = at;
    }
    return { ok: true, detail: `head ${i + 1} → ${l.reverse ? "REVERSE" : "forward"} at ${at.toFixed(3)}s` };
  }


  // --------------------------------------------------------------- scrub

  /** FUNCTION + fader N: audible positional scrub of head N. */
  beginScrub(i: number): { ok: boolean; detail: string } {
    if (!this.active) return { ok: false, detail: "heads mode is not active" };
    this.scrubbing[i] = true;
    this.lanes[i]!.posS = this.position(i);
    this.reconcile("scrub start");
    this.note("heads.scrub.start", i, `head ${i + 1} scrub armed at ${this.lanes[i]!.posS.toFixed(3)}s`);
    return { ok: true, detail: `head ${i + 1} scrub armed at ${this.lanes[i]!.posS.toFixed(3)}s` };
  }

  previewScrub(i: number, normalized: number): { ok: boolean; detail: string } {
    if (!this.active) return { ok: false, detail: "heads mode is not active" };
    if (!this.scrubbing[i]) this.beginScrub(i);
    const dur = this.duration(i);
    const target = Math.max(0, Math.min(dur, normalized * dur));
    const l = this.lanes[i]!;
    l.posS = target;
    this.grain(i, target);
    return { ok: true, detail: `head ${i + 1} scrub → ${target.toFixed(3)}s` };
  }

  endScrub(i: number, normalized: number): { ok: boolean; detail: string } {
    if (!this.active) return { ok: false, detail: "heads mode is not active" };
    const dur = this.duration(i);
    const l = this.lanes[i]!;
    const target = Math.max(0, Math.min(dur, normalized * dur));
    l.posS = target;
    l.scrubCandidate = target;
    this.scrubbing[i] = null;
    if (l.moving) this.stopLane(i, "scrub landing");
    this.reconcile("scrub end");
    this.note("heads.scrub.end", i, `head ${i + 1} landed at ${target.toFixed(3)}s — stored as the next loop start`);
    return { ok: true, detail: `head ${i + 1} landed at ${target.toFixed(3)}s — double-tap Track ${i + 1} to capture a loop there` };
  }

  private grain(i: number, atS: number) {
    const ctx = this.host.ctx();
    const buf = this.host.buffer(i);
    const laneBus = this.laneBuses[i] ?? this.bus;
    if (!ctx || !buf || !laneBus) return;
    const l = this.lanes[i]!;
    const g = ctx.createGain();
    g.connect(laneBus);
    const n = ctx.createBufferSource();
    n.buffer = buf;
    n.connect(g);
    const at = ctx.currentTime;
    const lvl = l.muted ? 0.5 * l.level : l.level;
    g.gain.setValueAtTime(0, at);
    g.gain.linearRampToValueAtTime(lvl, at + 0.005);
    g.gain.linearRampToValueAtTime(0, at + GRAIN_S);
    try {
      n.start(at, Math.min(Math.max(0, atS), Math.max(0, buf.duration - GRAIN_S)), GRAIN_S);
    } catch {
      /* noop */
    }
    setTimeout(() => {
      try {
        n.disconnect();
        g.disconnect();
      } catch {
        /* noop */
      }
    }, 400);
  }

  // ----------------------------------------------------------- diagnostics

  rms(): number {
    const a = this.tap;
    if (!a) return 0;
    const buf = new Float32Array(a.fftSize);
    a.getFloatTimeDomainData(buf);
    let sum = 0;
    for (let i = 0; i < buf.length; i++) sum += buf[i]! * buf[i]!;
    return Math.sqrt(sum / buf.length);
  }

  setLevel(i: number, level: number) {
    const l = this.lanes[i];
    if (!l) return;
    l.level = Math.max(0, Math.min(1, level));
    this.reconcile("level");
  }

  snapshot(): HeadLaneSnapshot[] {
    return this.lanes.map((l, i) => ({
      lane: i,
      playing: l.moving,
      latched: l.latched,
      held: l.held,
      muted: l.muted,
      reverse: l.reverse,
      level: l.level,
      position: this.position(i),
      loop: l.loop ? { ...l.loop } : null,
      scrubCandidate: l.scrubCandidate,
      ended: l.ended,
    }));
  }

  summary(): string {
    if (!this.active) return "heads off";
    return this.snapshot()
      .map(
        (h) =>
          `H${h.lane + 1} ${h.position.toFixed(2)}s${h.playing ? " ▶" : " ‖"}${h.latched ? " LATCH" : ""}${h.held ? " HOLD" : ""}${h.reverse ? " REV" : ""}${h.muted ? " MUTE" : ""}${h.loop ? ` LOOP ${h.loop.bars}b` : ""}`,
      )
      .join(" · ");
  }
}
