/**
 * Tri-state staging-capacity assessment.
 *
 * Required size is known as soon as a song is prepared. AVAILABLE size is a
 * property of the connected device and is simply UNKNOWN until a compatible
 * device has answered the capability query. Unknown is never encoded as false,
 * zero, NaN or "insufficient": an unknown capacity may not enable upload and
 * may not emit insufficient-capacity wording.
 */

export type CapacityStatus = "unknown" | "fits" | "insufficient";

/** Where the capability negotiation currently stands. */
export type CapabilityQueryState =
  /** No device connected / never queried. */
  | "none"
  /** Query in flight. */
  | "pending"
  /** Query failed, or the device is not Stem Tape v1.1 A/B firmware. */
  | "unverified"
  /** Compatible device; availableSectors is authoritative. */
  | "compatible";

export interface CapacityInput {
  /** Logical 8 KiB sectors the prepared song needs; 0 when no song is prepared. */
  requiredSectors: number;
  /** Sectors per song slot, or null whenever it is not known. */
  availableSectors: number | null;
  queryState: CapabilityQueryState;
}

export interface CapacityAssessment {
  status: CapacityStatus;
  requiredSectors: number;
  /** null in every unknown case — stale device values must never survive. */
  availableSectors: number | null;
  /** Primary capacity line. */
  line: string;
  /** Secondary line; empty string when there is nothing to add. */
  note: string;
}

const PENDING_NOTE =
  "Upload remains disabled until device capacity is confirmed. No data has been written.";

export function assessCapacity(input: CapacityInput): CapacityAssessment {
  const required = Number.isFinite(input.requiredSectors) ? Math.max(0, Math.trunc(input.requiredSectors)) : 0;

  if (input.queryState === "unverified") {
    return {
      status: "unknown",
      requiredSectors: required,
      availableSectors: null,
      line: "Device storage capacity could not be verified.",
      note: PENDING_NOTE,
    };
  }

  const available =
    input.queryState === "compatible" &&
    typeof input.availableSectors === "number" &&
    Number.isFinite(input.availableSectors)
      ? Math.max(0, Math.trunc(input.availableSectors))
      : null;

  if (available === null || required <= 0) {
    return {
      status: "unknown",
      requiredSectors: required,
      availableSectors: null,
      line: `${required} sectors required. Connect a compatible Stem Tape SP-1 to check available storage.`,
      note: PENDING_NOTE,
    };
  }

  if (required <= available) {
    return {
      status: "fits",
      requiredSectors: required,
      availableSectors: available,
      line: `${required} sectors required · ${available} available · fits`,
      note: "",
    };
  }

  return {
    status: "insufficient",
    requiredSectors: required,
    availableSectors: available,
    line: `${required} sectors required · ${available} available · does not fit`,
    note: "No data will be written.",
  };
}

export interface UploadGateInput {
  /** A real compatible device is connected (transport present, index read). */
  deviceConnected: boolean;
  /** Capability negotiation passed (verdict.writable). */
  capabilitiesNegotiated: boolean;
  capacity: CapacityStatus;
  /** All four stems + metadata prepared into a canonical song. */
  songPrepared: boolean;
  /** A transfer or other device operation is running. */
  transferActive: boolean;
}

/** The single upload-enable predicate. */
export function uploadEnabled(g: UploadGateInput): boolean {
  return (
    g.deviceConnected &&
    g.capabilitiesNegotiated &&
    g.capacity === "fits" &&
    g.songPrepared &&
    !g.transferActive
  );
}
