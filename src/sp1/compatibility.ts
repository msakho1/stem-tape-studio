/**
 * Fail-closed compatibility negotiation.
 *
 * Answering the classic `SP1XFER!` handshake proves only that some SP-1-class
 * device is listening. It is NOT evidence of Stem Tape firmware. Physical
 * mutation requires every requirement below to be positively satisfied by the
 * firmware-generated protocol package; absent fields, unknown versions and
 * partial capability responses stay read-only.
 */

import { GENERATED_PROTOCOL, type GeneratedHandshake } from "./generatedProtocol";

export const LOCK_NOTICE =
  "Firmware transfer compatibility is being finalized. Preview and stem preparation remain available; physical uploads are temporarily locked.";

export const REQUIRED_DEVICE_IDENTITY = "STEM TAPE SP-1";
export const REQUIRED_PROTOCOL_VERSION = "stem-tape-transfer/1";
export const REQUIRED_STORAGE_LAYOUT_VERSION = "stem-tape-storage/1";
export const REQUIRED_FORMAT_IDENTIFIER = "pcm-s24le-48000-2ch";

export interface CompatibilityRequirement {
  id: string;
  label: string;
  satisfied: boolean;
  detail: string;
}

export interface CompatibilityVerdict {
  /** True only when every requirement is satisfied. Never true today. */
  physicalMutationAllowed: boolean;
  requirements: CompatibilityRequirement[];
  summary: string;
}

export function negotiate(handshake: GeneratedHandshake | null): CompatibilityVerdict {
  const h = handshake;
  const cap = h?.capabilities;
  const req = (id: string, label: string, satisfied: boolean, detail: string): CompatibilityRequirement => ({
    id,
    label,
    satisfied: !!satisfied,
    detail,
  });

  const requirements: CompatibilityRequirement[] = [
    req(
      "generated-protocol",
      "firmware-generated protocol package",
      GENERATED_PROTOCOL !== null,
      GENERATED_PROTOCOL ? "present" : "absent — no golden fixtures to validate against",
    ),
    req("identity", "exact Stem Tape device identity", h?.deviceIdentity === REQUIRED_DEVICE_IDENTITY, h?.deviceIdentity ?? "not reported"),
    req("protocol-version", "compatible protocol version", h?.protocolVersion === REQUIRED_PROTOCOL_VERSION, h?.protocolVersion ?? "not reported"),
    req(
      "storage-layout",
      "compatible storage-layout version",
      h?.storageLayoutVersion === REQUIRED_STORAGE_LAYOUT_VERSION,
      h?.storageLayoutVersion ?? "not reported",
    ),
    req("format", "stereo 48 kHz 24-bit support", h?.formatIdentifier === REQUIRED_FORMAT_IDENTIFIER && !!cap?.stereo48k24bit, h?.formatIdentifier ?? "not reported"),
    req("four-stems", "four-stem support", !!cap?.fourStems, cap?.fourStems ? "yes" : "not reported"),
    req(
      "transaction",
      "transaction and resume capability",
      !!cap?.transactionalCommit && !!cap?.resume,
      cap?.transactionalCommit ? "commit reported" : "not reported",
    ),
    req(
      "metadata",
      "metadata capability including BPM and downbeat",
      !!cap?.metadata && !!cap?.metadataBpm && !!cap?.metadataDownbeat,
      cap?.metadataBpm ? "bpm + downbeat storable" : "not reported",
    ),
    req(
      "address-units",
      "device-reported address units and transfer limits",
      !!h?.addressUnits?.sectorBytes && !!h?.addressUnits?.transportChunkBytes && !!h?.maxTransferBytes,
      h?.addressUnits ? `${h.addressUnits.sectorBytes} B sector` : "not reported",
    ),
    req("safe-init", "safe initialization capability", !!cap?.safeInitialise, cap?.safeInitialise ? "yes" : "not reported"),
    req(
      "generation",
      "current library generation",
      typeof h?.libraryGeneration === "number",
      typeof h?.libraryGeneration === "number" ? String(h.libraryGeneration) : "not reported",
    ),
  ];

  const physicalMutationAllowed = requirements.every((r) => r.satisfied);
  return {
    physicalMutationAllowed,
    requirements,
    summary: physicalMutationAllowed
      ? "compatible Stem Tape firmware negotiated"
      : LOCK_NOTICE,
  };
}

/** A classic SP1XFER-only device: read-only, always. */
export function legacyVerdict(): CompatibilityVerdict {
  return negotiate(null);
}
