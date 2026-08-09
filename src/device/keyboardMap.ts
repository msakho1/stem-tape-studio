/**
 * Desktop keyboard bindings, DERIVED from the Stem Tape v1 registry.
 *
 * The panel never hard-codes behaviour: every row here comes from
 * `STEM_TAPE_V1_MAP` (id, command, plain-language tutorial text). Only the
 * control → physical key translation lives locally, because that mapping is a
 * property of the desktop shell, not of the instrument.
 */

import { STEM_TAPE_V1_MAP, type StemTapeRow } from "@/machine/stemTapeV1Map";

/** Control id → the key code the desktop shell listens for. */
export const CONTROL_CODE: Record<string, string> = {
  play: "Space",
  function: "KeyF",
  "rocker-fwd": "KeyQ",
  "rocker-rwd": "KeyA",
  "volume-minus": "Minus",
  "volume-plus": "Equal",
  "track-button-1": "Digit1",
  "track-button-2": "Digit2",
  "track-button-3": "Digit3",
  "track-button-4": "Digit4",
};

export const CODE_LABEL: Record<string, string> = {
  Space: "space",
  KeyF: "F",
  KeyQ: "Q",
  KeyA: "A",
  Minus: "−",
  Equal: "=",
  Digit1: "1",
  Digit2: "2",
  Digit3: "3",
  Digit4: "4",
  Escape: "esc",
  KeyY: "Y",
  KeyH: "H",
  KeyU: "U",
  KeyJ: "J",
  KeyI: "I",
  KeyK: "K",
  KeyO: "O",
  KeyL: "L",
};

export type KeyContext = "base" | "fx" | "heads" | "record";

export interface KeyBinding {
  /** Registry row id, or a shell-owned id for non-instrument keys. */
  id: string;
  /** Key codes that must be held together. */
  codes: string[];
  label: string;
  context: KeyContext;
  /** Plain-language text — registry tutorial copy when the row has it. */
  detail: string;
  source: "registry" | "shell";
}

function contextOf(row: StemTapeRow): KeyContext {
  if (row.layer === "fx-overlay") return "fx";
  if (/^(rec|grid|print|export)\./.test(row.id)) return "record";
  return "base";
}

function codesFor(row: StemTapeRow): string[][] {
  if (row.keys?.length) return row.keys.map((k) => k.split("+"));
  const codes = row.controls.map((c) => CONTROL_CODE[c]).filter((c): c is string => !!c);
  return codes.length === row.controls.length ? [codes] : [];
}

export function keyboardBindings(): KeyBinding[] {
  const out: KeyBinding[] = [];
  for (const row of STEM_TAPE_V1_MAP) {
    for (const codes of codesFor(row)) {
      out.push({
        id: row.id,
        codes,
        label: codes.map((c) => CODE_LABEL[c] ?? c).join(" + "),
        context: contextOf(row),
        detail: row.tutorial?.plainLanguage ?? row.command,
        source: "registry",
      });
    }
  }
  // Shell-owned keys: continuous faders and the safety release.
  const faders: [string, string, number][] = [
    ["KeyY", "KeyH", 1],
    ["KeyU", "KeyJ", 2],
    ["KeyI", "KeyK", 3],
    ["KeyO", "KeyL", 4],
  ];
  for (const [up, down, n] of faders) {
    out.push({
      id: `fader.${n}`,
      codes: [up, down],
      label: `${CODE_LABEL[up]} / ${CODE_LABEL[down]}`,
      context: "base",
      detail: `Fader ${n} up / down — hold several at once for true simultaneous moves.`,
      source: "shell",
    });
    out.push({
      id: `fader.${n}.heads`,
      codes: [up, down],
      label: `${CODE_LABEL[up]} / ${CODE_LABEL[down]}`,
      context: "heads",
      detail: `Head ${n} level; hold F as well to scrub head ${n}.`,
      source: "shell",
    });
    out.push({
      id: `fader.${n}.scrub`,
      codes: [up, down],
      label: `F + ${CODE_LABEL[up]} / ${CODE_LABEL[down]}`,
      context: "fx",
      detail: `Scrub stem ${n} audibly; release to park it, then double-tap track ${n} to capture one bar there.`,
      source: "shell",
    });
  }

  out.push({
    id: "shell.release",
    codes: ["Escape"],
    label: "esc",
    context: "base",
    detail: "Release everything: stops the shuttle and clears any latched control.",
    source: "shell",
  });
  return out;
}

/** True when every code of the binding is currently held. */
export function isLit(binding: KeyBinding, held: readonly string[]): boolean {
  if (binding.codes.length > 1 && binding.id.startsWith("fader.")) return binding.codes.some((c) => held.includes(c));
  return binding.codes.every((c) => held.includes(c));
}
