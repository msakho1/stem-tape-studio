/**
 * Expected Stem Tape M0 MIDI contract, presented in BOTH decimal and
 * hexadecimal so the CC20-23 (0x14-0x17) versus CC32-35 (0x20-0x23) notation
 * error cannot recur.
 *
 * Declarative reference data for the diagnostic drawer only.
 */

export const SP1_MIDI_CHANNEL = 1;

export interface MidiContractRow {
  kind: "note" | "cc";
  /** Decimal note number / controller number. */
  dec: number;
  /** Byte form of the same number. */
  hex: string;
  name: string;
  note?: string;
}

const hx = (n: number) => `0x${n.toString(16).toUpperCase().padStart(2, "0")}`;

const note = (dec: number, name: string): MidiContractRow => ({ kind: "note", dec, hex: hx(dec), name });
const cc = (dec: number, name: string, extra?: string): MidiContractRow => ({
  kind: "cc",
  dec,
  hex: hx(dec),
  name,
  ...(extra ? { note: extra } : {}),
});

export const SP1_MIDI_CONTRACT: MidiContractRow[] = [
  note(36, "Track 1"),
  note(37, "Track 2"),
  note(38, "Track 3"),
  note(39, "Track 4"),
  note(40, "Play"),
  note(41, "Function"),
  note(42, "Volume +"),
  note(43, "Volume −"),
  note(44, "Rocker Forward"),
  note(45, "Rocker Reverse"),
  cc(20, "Fader 1"),
  cc(21, "Fader 2"),
  cc(22, "Fader 3"),
  cc(23, "Fader 4"),
  cc(24, "Battery telemetry", "telemetry only — never a mixer control"),
  cc(123, "All notes off", "release all controls / connection resync"),
];

export const SP1_BUTTON_PHASES = [
  "Note On velocity 127 → press",
  "Note Off velocity 0 → release",
  "Note On velocity 0 → release",
];

export const SP1_NOTATION_WARNING =
  "CC20–23 are DECIMAL (bytes 0x14–0x17). CC32–35 (bytes 0x20–0x23) are NOT SP-1 faders.";

/** `B0 14 7F  ·  ch1 CC20 (0x14) Fader 1 = 127` */
export function describeMidiBytes(bytes: ArrayLike<number>): string {
  const b: number[] = [];
  for (let i = 0; i < bytes.length; i++) b.push(bytes[i] ?? 0);
  const raw = b.map((n) => n.toString(16).toUpperCase().padStart(2, "0")).join(" ");
  const status = b[0] ?? 0;
  const ch = (status & 0x0f) + 1;
  const type = status & 0xf0;
  const d1 = b[1] ?? 0;
  const d2 = b[2] ?? 0;
  if (type === 0x90 || type === 0x80) {
    const row = SP1_MIDI_CONTRACT.find((r) => r.kind === "note" && r.dec === d1);
    const phase = type === 0x90 && d2 > 0 ? "press" : "release";
    return `${raw}  ·  ch${ch} note ${d1} (${hx(d1)}) ${row?.name ?? "unmapped"} ${phase} vel ${d2}`;
  }
  if (type === 0xb0) {
    const row = SP1_MIDI_CONTRACT.find((r) => r.kind === "cc" && r.dec === d1);
    return `${raw}  ·  ch${ch} CC${d1} (${hx(d1)}) ${row?.name ?? "unmapped"} = ${d2}`;
  }
  return `${raw}  ·  ch${ch} status ${hx(status)} (outside the SP-1 contract)`;
}
