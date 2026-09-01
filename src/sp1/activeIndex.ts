/**
 * The ONE active-index selector.
 *
 * Used identically by mock boot, connection, transfer preflight, reconnection
 * recovery and post-commit confirmation. There is deliberately no mutable
 * "active pointer" authority anywhere: the two valid generation records decide
 * which song is active. The device's own `activeIndexSlot` capability field is
 * advisory and is never trusted for selection.
 *
 * Rules (in order):
 *   1. Parse both index slots independently.
 *   2. Reject any record with invalid magic, CRC, version, bounds, slot
 *      identity or song metadata.
 *   3. Exactly one valid -> select it.
 *   4. Both valid -> select the strictly greater generation (tie: slot A).
 *   5. Neither valid -> blank (all zero) or corrupt; explicit init required.
 *   6. A corrupt newer record therefore falls back to the previous generation.
 *   7. One invalid slot NEVER requires reinitialization.
 */

import {
  otherSlot,
  slotName,
  type AbSlot,
  SLOT_A,
  SLOT_B,
} from "./stemTapeFormat";
import {
  parseIndexRecord,
  validateIndexRecord,
  type IndexValidation,
  type RegionContext,
  type StemTapeIndexRecord,
} from "./stemIndex";

export type LibraryStatus = "ok" | "blank" | "legacy" | "corrupt";

export interface SlotReading {
  slot: AbSlot;
  /** Raw bytes of the index record as read from the device. */
  bytes: Uint8Array;
  record: StemTapeIndexRecord;
  validation: IndexValidation;
  /** True when the whole record image is zero — never written. */
  blank: boolean;
}

export interface LibraryState {
  slots: [SlotReading, SlotReading];
  status: LibraryStatus;
  /** Index slot holding the selected record, or null when none is valid. */
  activeIndexSlot: AbSlot | null;
  active: StemTapeIndexRecord | null;
  /** Song region the active record points at, or null. */
  activeSongSlot: AbSlot | null;
  generation: number;
  /** Index slot a replacement must be written to. */
  inactiveIndexSlot: AbSlot;
  /** Song region a replacement must be written to. */
  inactiveSongSlot: AbSlot;
  /** True only when neither slot holds a valid record. */
  requiresInitialization: boolean;
  explanation: string;
}

function isBlank(bytes: Uint8Array): boolean {
  return bytes.every((b) => b === 0);
}

/**
 * A record written by an EARLIER format version: it is a committed STIX record
 * whose CRC verifies, so nothing is damaged — this firmware simply no longer
 * accepts that `formatMinor`. This is never corruption and is never reported as
 * such.
 */
export function isLegacyRecord(s: SlotReading): boolean {
  const r = s.record;
  return (
    !s.validation.valid &&
    r.committed &&
    r.crc === r.crcComputed &&
    r.indexVersion === STIX_VERSION &&
    r.formatMajor === FORMAT_MAJOR &&
    r.formatMinor !== FORMAT_MINOR
  );
}

export function readSlot(
  slot: AbSlot,
  bytes: Uint8Array,
  regions: RegionContext,
  /** Frozen-bundle audits pass 1 to read a historical v1.1 record. */
  expectFormatMinor?: number,
): SlotReading {
  const record = parseIndexRecord(bytes);
  return {
    slot,
    bytes: bytes.slice(0),
    record,
    validation: validateIndexRecord(record, slot, regions, expectFormatMinor),
    blank: isBlank(bytes),
  };
}

export function selectActiveIndex(a: SlotReading, b: SlotReading): LibraryState {
  const slots: [SlotReading, SlotReading] = [a, b];
  const valid = slots.filter((s) => s.validation.valid);

  if (valid.length === 0) {
    const blank = a.blank && b.blank;
    const legacySlots = slots.filter((s) => isLegacyRecord(s));
    const legacy = legacySlots.length > 0;
    return {
      slots,
      status: blank ? "blank" : legacy ? "legacy" : "corrupt",
      activeIndexSlot: null,
      active: null,
      activeSongSlot: null,
      generation: 0,
      inactiveIndexSlot: SLOT_A,
      inactiveSongSlot: SLOT_A,
      requiresInitialization: true,
      explanation: blank
        ? "Both index slots are blank: this storage has never been initialized."
        : legacy
          ? `This SP-1 was set up by an earlier version of the format (index ${legacySlots
              .map((s) => slotName(s.slot))
              .join(" and ")} carries a CRC-valid v1.${legacySlots[0]!.record.formatMinor} record). The records read correctly; this firmware simply no longer accepts that layout.`
          : `Neither index slot holds a readable record (A: ${a.validation.reason}; B: ${b.validation.reason}). This SP-1 has not been set up for this firmware.`,
    };
  }

  const chosen =
    valid.length === 1
      ? valid[0]!
      : valid[0]!.record.generation >= valid[1]!.record.generation
        ? valid[0]!
        : valid[1]!;
  const other = slots[chosen.slot === SLOT_A ? 1 : 0]!;
  const rec = chosen.record;
  const activeSongSlot = rec.songPresent ? rec.songSlot : null;
  const inactiveSongSlot: AbSlot = rec.songPresent ? otherSlot(rec.songSlot) : rec.songSlot;

  return {
    slots,
    status: "ok",
    activeIndexSlot: chosen.slot,
    active: rec,
    activeSongSlot,
    generation: rec.generation,
    inactiveIndexSlot: otherSlot(chosen.slot),
    inactiveSongSlot,
    requiresInitialization: false,
    explanation:
      valid.length === 2
        ? `Index ${slotName(chosen.slot)} generation ${rec.generation} is newer than index ${slotName(other.slot)} generation ${other.record.generation}.`
        : `Only index ${slotName(chosen.slot)} is valid (generation ${rec.generation}); index ${slotName(other.slot)} is ignored: ${other.validation.reason}.`,
  };
}

export const AB_SLOTS: AbSlot[] = [SLOT_A, SLOT_B];
