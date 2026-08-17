/**
 * Versioned Stem Tape M0 firmware artifact registry.
 *
 * EXPECTED build metadata only. The device cannot transmit or attest its
 * flashed image, so nothing here verifies device identity. Reported identity
 * comes exclusively from the CDC banner; when the console is closed the
 * reported firmware is `unknown` / null and must never be back-filled from
 * this registry.
 */

export interface FirmwareProfile {
  id: string;
  product: string;
  firmwareBanner: string;
  usbVendorId: string;
  usbProductId: string;
  binarySha256: string;
  sourceCommit: string;
  /** Host→device LED protocol version implemented by this build, 0 = none. */
  ledProtocolVersion: number;
  physicalLeds: number;
  status: "current" | "legacy";
  note: string;
}

export const M0_V1_1_2: FirmwareProfile = {
  id: "m0-v1.1.2",
  product: "Stem Tape SP-1",
  firmwareBanner: "Stem Tape M0 v1.1.2",
  usbVendorId: "0x1915",
  usbProductId: "0x5211",
  binarySha256: "e08f8200500c2dff4ecf7ea2268c3f88c795ef6bbf33b3e0dfd552e07c3adeea",
  sourceCommit: "134c0ffb3c955b14e76eeeb19b1dc2ffc1c59bf6",
  ledProtocolVersion: 1,
  physicalLeds: 8,
  status: "current",
  note: "expected build metadata — receives host LED frames on MIDI channel 16 (LED protocol v1)",
};

export const M0_V1_0_0: FirmwareProfile = {
  id: "m0-v1.0.0",
  product: "Stem Tape SP-1",
  firmwareBanner: "Stem Tape M0 v1.0.0",
  usbVendorId: "0x1915",
  usbProductId: "0x5211",
  binarySha256: "53de4c003047e20b7e18c45034eab89b79d58ba1677651348e62f9a85b257eeb",
  sourceCommit: "ea354f32c8c484c1d48e68804a4f1695a8a7b131",
  ledProtocolVersion: 0,
  physicalLeds: 8,
  status: "legacy",
  note: "legacy input-only diagnostic build — no host→device LED path",
};

export const FIRMWARE_PROFILES: readonly FirmwareProfile[] = [M0_V1_1_2, M0_V1_0_0];

/** The default EXPECTED profile. Never a claim about the flashed image. */
export const CURRENT_FIRMWARE_PROFILE = M0_V1_1_2;

/** Matches a reported CDC banner to a known profile, or null when unknown. */
export function profileForBanner(banner: string | null): FirmwareProfile | null {
  if (!banner) return null;
  return FIRMWARE_PROFILES.find((p) => banner.trim().startsWith(p.firmwareBanner)) ?? null;
}
