/**
 * Filename-driven stem assignment and song-title inference.
 *
 * Pure and deterministic: no I/O, no audio decoding. The uploader uses this so
 * a musician can drop all four exported stems at once instead of filling four
 * separate inputs.
 */

import { STEM_ORDER, type StemSlotName } from "./prepare";

/** Role aliases, longest-first inside each role so "vocals" beats "vocal". */
const ALIASES: Record<StemSlotName, string[]> = {
  vocal: ["vocals", "vocal", "voice", "vox"],
  drums: ["drums", "drum", "percussion"],
  bass: ["bass"],
  instrument: ["instruments", "instrument", "instrumental", "other", "music", "inst"],
};

/** Suffix tokens stripped when inferring the shared song title. */
export const ROLE_TOKENS = Object.values(ALIASES).flat();

const SEPARATORS = "[ _\\-.]";

function baseName(filename: string): string {
  return filename.replace(/\.[a-z0-9]+$/i, "");
}

/** Role implied by a filename, or null when nothing matches. */
export function roleForFilename(filename: string): StemSlotName | null {
  const base = baseName(filename).toLowerCase();
  let best: { role: StemSlotName; index: number; length: number } | null = null;
  for (const role of STEM_ORDER) {
    for (const alias of ALIASES[role]) {
      const re = new RegExp(`(^|${SEPARATORS})${alias}($|${SEPARATORS})`, "g");
      let m: RegExpExecArray | null;
      while ((m = re.exec(base))) {
        const index = m.index;
        // The last (right-most) role token wins; ties go to the longer alias.
        if (!best || index > best.index || (index === best.index && alias.length > best.length)) {
          best = { role, index, length: alias.length };
        }
      }
    }
  }
  return best?.role ?? null;
}

/** Strip the trailing role token, if any, from a filename base. */
export function stripRoleToken(filename: string): string {
  const base = baseName(filename);
  const alt = ROLE_TOKENS.join("|");
  return base.replace(new RegExp(`${SEPARATORS}*(${alt})\\s*$`, "i"), "").trim();
}

export interface AssignmentResult {
  assigned: Partial<Record<StemSlotName, File>>;
  /** Files whose role could not be resolved, or that collided with another file. */
  ambiguous: File[];
}

/**
 * Assign a batch of files to the four roles. A role is only auto-filled when
 * exactly one file claims it; every other file is reported as ambiguous so the
 * interface can ask about that file alone.
 */
export function assignFiles(files: File[]): AssignmentResult {
  const claims = new Map<StemSlotName, File[]>();
  const ambiguous: File[] = [];
  for (const file of files) {
    const role = roleForFilename(file.name);
    if (!role) {
      ambiguous.push(file);
      continue;
    }
    const list = claims.get(role) ?? [];
    list.push(file);
    claims.set(role, list);
  }
  const assigned: Partial<Record<StemSlotName, File>> = {};
  for (const role of STEM_ORDER) {
    const list = claims.get(role) ?? [];
    if (list.length === 1) assigned[role] = list[0]!;
    else ambiguous.push(...list);
  }
  return { assigned, ambiguous };
}

/**
 * Song title from the shared filename prefix, with the role token removed.
 *  "Wont do - J Dilla_Vocal.wav" + siblings -> "Wont do - J Dilla"
 */
export function inferTitle(filenames: string[]): string {
  const bases = filenames.map(stripRoleToken).filter((b) => b.length > 0);
  if (bases.length === 0) return "";
  if (bases.length === 1) return bases[0]!;

  let prefix = bases[0]!;
  for (const b of bases.slice(1)) {
    let i = 0;
    while (i < prefix.length && i < b.length && prefix[i] === b[i]) i++;
    prefix = prefix.slice(0, i);
  }
  const trimmed = prefix.replace(new RegExp(`${SEPARATORS}+$`), "").trim();
  return trimmed.length >= 2 ? trimmed : bases[0]!;
}
