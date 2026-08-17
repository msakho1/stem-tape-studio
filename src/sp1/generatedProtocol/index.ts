/**
 * Integration boundary for the firmware-generated Stem Tape protocol package.
 *
 * The firmware repository will emit, from its own canonical protocol source:
 *   - exact TypeScript types
 *   - command identifiers
 *   - capability flags
 *   - serializer / deserializer
 *   - metadata schema
 *   - address-unit definitions
 *   - golden handshake / sector / resume / verify / commit fixtures
 *
 * Nothing in this directory may be hand-translated from prose documentation.
 * Until the generated package is dropped in here, `GENERATED_PROTOCOL` is
 * null and every physical mutation path stays closed.
 */

export interface GeneratedAddressUnits {
  /** Logical storage sector size in bytes, as reported by the device. */
  sectorBytes: number;
  /** USB transport chunk size — NOT assumed equal to `sectorBytes`. */
  transportChunkBytes: number;
  /** Unit in which resume offsets are expressed. */
  resumeUnit: "sector" | "byte" | "frame";
}

export interface GeneratedCapabilities {
  stereo48k24bit: boolean;
  fourStems: boolean;
  transactionalCommit: boolean;
  resume: boolean;
  metadata: boolean;
  metadataBpm: boolean;
  metadataDownbeat: boolean;
  safeInitialise: boolean;
}

export interface GeneratedHandshake {
  deviceIdentity: string;
  protocolVersion: string;
  storageLayoutVersion: string;
  formatIdentifier: string;
  capabilities: GeneratedCapabilities;
  addressUnits: GeneratedAddressUnits;
  maxTransferBytes: number;
  libraryGeneration: number;
  songCapacity: number;
}

export interface GeneratedProtocolPackage {
  schemaVersion: string;
  protocolVersion: string;
  storageLayoutVersion: string;
  commands: Record<string, number>;
  parseHandshake(bytes: Uint8Array): GeneratedHandshake;
  serializeMetadata(meta: unknown): Uint8Array;
  goldenFixtures: Record<string, Uint8Array>;
}

/** Populated only by the firmware build's generated artifacts. */
export const GENERATED_PROTOCOL: GeneratedProtocolPackage | null = null;

export function generatedProtocolAvailable(): boolean {
  return GENERATED_PROTOCOL !== null;
}

export function requireGeneratedProtocol(): GeneratedProtocolPackage {
  if (!GENERATED_PROTOCOL) {
    throw new Error(
      "the firmware-generated Stem Tape protocol package is not present — physical transfer stays locked",
    );
  }
  return GENERATED_PROTOCOL;
}
