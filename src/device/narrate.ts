import { BANKS, type BankIndex } from "@/machine/fx12";
import type { AudioCommand } from "@/audio/commands";
import type { SurfaceState } from "@/machine/surface";

const LANE_NAMES = ["vocals", "drums", "bass", "instruments"] as const;

function lane(n: unknown): string {
  const i = Number(n);
  return Number.isFinite(i) && LANE_NAMES[i] ? `lane ${i + 1} (${LANE_NAMES[i]})` : "lane ?";
}

function maskLanes(mask: unknown): string {
  const s = String(mask ?? "");
  const on = [...s].map((c, i) => (c === "1" ? i : -1)).filter((i) => i >= 0);
  if (on.length === 0) return "none";
  return on.map((i) => `${i + 1}`).join(" + ");
}

/** Human name of the algorithm currently selected in `bank` for `stem`. */
export function algorithmLabel(state: SurfaceState, stem: number, bank: number): string {
  const def = BANKS[bank as BankIndex];
  if (!def) return "FX";
  const track = state.perf.tracks[stem];
  const idx = track?.fx12.banks[bank as BankIndex]?.selectedAlgorithm ?? 0;
  return `${def.label} · ${def.algorithms[idx]?.label ?? "?"}`;
}

/**
 * Plain-language narration of one semantic command — this is what the
 * "what just happened?" readout shows. It must name the actual effect, the
 * actual lane and the actual head, never an internal command identifier.
 */
export function narrateCommand(cmd: AudioCommand, state: SurfaceState): string {
  const p = cmd.payload as Record<string, number | string | boolean | null>;
  const stem = Number(p["stem"] ?? state.perf.activeStem ?? 0);
  const bank = Number(p["bank"] ?? 0);
  const fx = () => algorithmLabel(state, stem, bank);

  switch (cmd.type) {
    case "transport.play":
      return "PLAY — tape spins up to speed";
    case "transport.stop":
      return "STOP — tape winds down and parks";
    case "transport.restart":
      return "restart — playhead back to the top";
    case "transport.cue":
      return "cue — parked at the cue point";
    case "transport.scrub":
    case "transport.scrub.start":
      return `global shuttle ${Number(p["dir"]) < 0 ? "backwards" : "forwards"} — all four stems together`;
    case "transport.scrub.end":
      return "shuttle released — stems land at the new position";
    case "track.mute":
      return `${lane(p["track"] ?? stem)} muted`;
    case "track.unmute":
      return `${lane(p["track"] ?? stem)} unmuted`;
    case "track.gain":
      return `${lane(p["track"] ?? stem)} level ${Math.round(Number(p["value"] ?? 0) * 100)}%`;
    case "master.gain":
      return `master level ${Math.round(Number(p["value"] ?? 0) * 100)}%`;
    case "rate.set":
      return `varispeed ${Number(p["rate"] ?? 1).toFixed(2)}×`;
    case "loop.chop":
      return `chop 1/${p["div"]} on ${lane(stem)}`;
    case "loop.set":
      return `loop window set on ${lane(stem)}`;
    case "loop.capture":
      return `one-bar loop captured on ${lane(p["lane"] ?? stem)}`;
    case "loop.release":
      return `loop released on ${lane(p["lane"] ?? stem)}`;
    case "loop.resize":
      return `loop resized ${Number(p["dir"]) < 0 ? "shorter" : "longer"} on ${lane(p["lane"] ?? stem)}`;
    case "tape.reverse":
      return `tape reverse ${p["on"] ? "on" : "off"}`;
    case "lane.reverse":
      return `${lane(p["lane"] ?? stem)} playing ${p["on"] === false ? "forwards again" : "backwards"}`;
    case "lane.audition":
      return `momentary solo — hearing ${maskLanes(p["mask"])}`;
    case "lane.scrub.start":
      return `scrubbing ${lane(p["lane"] ?? stem)} with its fader`;
    case "lane.scrub.end":
      return `${lane(p["lane"] ?? stem)} parked — double-tap its Track button to grab a bar`;
    case "lane.scrub.park":
      return `${lane(p["lane"] ?? stem)} parked at the scrub landing`;
    case "filter.set":
      return `filter ${p["mode"] === "off" ? "bypassed" : String(p["mode"])}`;
    case "song.load":
      return "song loaded — transport parked, press PLAY";
    case "stem.select":
      return `selected ${lane(state.perf.activeStem)}`;
    case "stem.solo":
      return `${lane(p["stem"] ?? stem)} solo latched`;
    case "stem.link":
      return `${lane(p["stem"] ?? stem)} link toggled`;

    // ---- FX -------------------------------------------------------------
    case "fx.overlay":
      return `FX mode ${p["on"] ? "opened" : "closed"}`;
    case "fx.bank.select":
      return `${fx()} selected on ${lane(stem)}`;
    case "fx.momentary.start":
      return `${fx()} ON (held) — ${lane(stem)}`;
    case "fx.momentary.end":
      return `${fx()} released — ${lane(stem)}`;
    case "fx.latch":
      return `${fx()} latched on ${lane(stem)}`;
    case "fx.clearLatches":
      return `all FX cleared on ${lane(stem)}`;
    case "fx.algorithm.cycle":
      return `${BANKS[bank as BankIndex]?.label ?? "FX"} bank → ${fx()}`;
    case "fx.macro":
      return `${fx()} amount ${Number(p["dir"]) < 0 ? "down" : "up"}`;
    case "fx.variation":
      return `${fx()} variation changed`;

    // ---- Heads ------------------------------------------------------------
    case "heads.enter":
      return "HEADS mode — four independent tape heads, one per stem";
    case "heads.exit":
      return "left HEADS — heads-only loops and reverses discarded, stems rejoin the song";
    case "heads.level":
      return `head ${Number(p["head"] ?? stem) + 1} level ${Math.round(Number(p["value"] ?? 0) * 100)}%`;
    case "heads.mute":
      return `head ${Number(p["head"] ?? stem) + 1} ${p["on"] ? "muted" : "unmuted"}`;
    case "heads.play.hold":
      return `head audition — hearing ${maskLanes(p["mask"])}`;
    case "heads.latch":
      return `head ${Number(p["head"] ?? stem) + 1} latched ${p["on"] === false ? "off" : "playing"}`;
    case "heads.loop.capture":
      return `head ${Number(p["head"] ?? stem) + 1} captured a one-bar loop`;
    case "heads.loop.resize":
      return `head ${Number(p["head"] ?? stem) + 1} loop ${Number(p["dir"]) < 0 ? "shorter" : "longer"}`;
    case "heads.scrub":
      return `scrubbing head ${Number(p["head"] ?? stem) + 1}`;

    case "grid.quantise":
      return "grid quantise";
    case "rollback":
      return "gesture revised — previous action rolled back";
    default:
      return cmd.type;
  }
}
