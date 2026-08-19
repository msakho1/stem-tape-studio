/**
 * Bulk verified-sector upload ('U') — companion side of the frozen wire
 * contract in firmware `src/st_bulk_xfer.h`.
 *
 * One round trip transfers, writes AND verifies one complete 8,192-byte Stem
 * Tape v1.1 sector (sixteen 512-byte blocks). The device answers with the
 * CRC-32 it computed from the bytes it READ BACK off eMMC; comparing that
 * against the CRC the host declared for the payload IS the read-back
 * verification, so the companion never performs a separate 512-byte read-back
 * pass for song data again.
 *
 * Nothing here touches index records: 'W' remains the only way to write an
 * index block, and the magic-last commit ordering is unchanged.
 *
 * Every constant, offset and status value below is copied from
 * st_bulk_xfer.h; none of them are invented here.
 */

import { crc32 } from "./crc32";
import { SECTOR_BYTES, BLOCKS_PER_SECTOR } from "./stemTapeFormat";

export const BULK_CMD = 0x55; // 'U'
export const BULK_PROTO_VERSION = 1;
export const BULK_PAYLOAD_BYTES = SECTOR_BYTES; // 8192
export const BULK_BLOCKS_PER_SECTOR = BLOCKS_PER_SECTOR; // 16
export const BULK_REQ_HEADER_BYTES = 17;
export const BULK_RESP_BYTES = 14;

export const BULK_REQ_OFF = {
  version: 0,
  seq: 1,
  destBlock: 5,
  payloadLen: 9,
  payloadCrc32: 13,
} as const;

export const BULK_RESP_OFF = {
  status: 0,
  seq: 1,
  destBlock: 5,
  verifiedCrc32: 9,
  retryable: 13,
} as const;

/** Frozen wire values — never renumbered, only appended (st_bulk_status_t). */
export const BULK_STATUS = {
  OK: 0,
  UNSUPPORTED_VERSION: 1,
  BAD_LENGTH: 2,
  TIMEOUT_PAYLOAD: 3,
  CDC_OVERFLOW: 4,
  CRC_MISMATCH: 5,
  LAYOUT_NOT_READY: 6,
  NO_SESSION: 7,
  SESSION_CLOSED: 8,
  OUT_OF_SEQUENCE: 9,
  DEST_MISMATCH: 10,
  OUT_OF_BOUNDS: 11,
  ACTIVE_REGION: 12,
  OUTSIDE_FROZEN_PAIR: 13,
  EMMC_WRITE_FAIL: 14,
  EMMC_READBACK_FAIL: 15,
  READBACK_CRC_MISMATCH: 16,
} as const;

/** Exactly the set st_bulk_status_is_retryable() returns true for. */
const RETRYABLE = new Set<number>([
  BULK_STATUS.TIMEOUT_PAYLOAD,
  BULK_STATUS.CDC_OVERFLOW,
  BULK_STATUS.CRC_MISMATCH,
  BULK_STATUS.EMMC_WRITE_FAIL,
  BULK_STATUS.EMMC_READBACK_FAIL,
  BULK_STATUS.READBACK_CRC_MISMATCH,
]);

export function bulkStatusIsRetryable(status: number): boolean {
  return RETRYABLE.has(status);
}

/** Plain-language explanation for the person at the keyboard. */
export function describeBulkStatus(status: number): string {
  switch (status) {
    case BULK_STATUS.OK:
      return "sector written and verified";
    case BULK_STATUS.UNSUPPORTED_VERSION:
      return "this SP-1 speaks a different version of the fast upload command — update the Stem Tape firmware";
    case BULK_STATUS.BAD_LENGTH:
      return "the SP-1 rejected the sector size";
    case BULK_STATUS.TIMEOUT_PAYLOAD:
      return "the SP-1 did not receive the whole sector in time";
    case BULK_STATUS.CDC_OVERFLOW:
      return "the USB connection dropped bytes while the sector was arriving";
    case BULK_STATUS.CRC_MISMATCH:
      return "the sector arrived damaged over USB";
    case BULK_STATUS.LAYOUT_NOT_READY:
      return "the SP-1 has no usable storage layout";
    case BULK_STATUS.NO_SESSION:
      return "the SP-1 has no upload session open";
    case BULK_STATUS.SESSION_CLOSED:
      return "the SP-1's upload session was already completed";
    case BULK_STATUS.OUT_OF_SEQUENCE:
      return "the SP-1 expected a different sector next";
    case BULK_STATUS.DEST_MISMATCH:
      return "the SP-1 expected this sector at a different place in its storage";
    case BULK_STATUS.OUT_OF_BOUNDS:
      return "this song runs past the space reserved on the SP-1";
    case BULK_STATUS.ACTIVE_REGION:
      return "the SP-1 refused a write into the song that is currently playable";
    case BULK_STATUS.OUTSIDE_FROZEN_PAIR:
      return "the SP-1 refused a write outside the region reserved for this upload";
    case BULK_STATUS.EMMC_WRITE_FAIL:
      return "the SP-1's storage failed to accept the sector";
    case BULK_STATUS.EMMC_READBACK_FAIL:
      return "the SP-1 could not read the sector back to check it";
    case BULK_STATUS.READBACK_CRC_MISMATCH:
      return "the sector did not read back correctly on the SP-1";
    default:
      return `the SP-1 reported an unknown transfer status (${status})`;
  }
}

export interface BulkRequest {
  seq: number;
  destBlock: number;
  payload: Uint8Array;
}

export interface BulkResponse {
  status: number;
  seq: number;
  destBlock: number;
  verifiedCrc32: number;
  retryable: boolean;
}

function put32(a: Uint8Array, o: number, v: number) {
  a[o] = v & 255;
  a[o + 1] = (v >>> 8) & 255;
  a[o + 2] = (v >>> 16) & 255;
  a[o + 3] = (v >>> 24) & 255;
}
function get32(a: Uint8Array, o: number): number {
  return ((a[o]! | (a[o + 1]! << 8) | (a[o + 2]! << 16)) >>> 0) + a[o + 3]! * 0x1000000;
}

/** Command byte + 17-byte header + the 8,192-byte payload, ready for the wire. */
export function buildBulkRequest(req: BulkRequest): Uint8Array {
  if (req.payload.length !== BULK_PAYLOAD_BYTES) {
    throw new Error(`bulk payload must be exactly ${BULK_PAYLOAD_BYTES} bytes`);
  }
  const out = new Uint8Array(1 + BULK_REQ_HEADER_BYTES + BULK_PAYLOAD_BYTES);
  out[0] = BULK_CMD;
  const h = 1;
  out[h + BULK_REQ_OFF.version] = BULK_PROTO_VERSION;
  put32(out, h + BULK_REQ_OFF.seq, req.seq);
  put32(out, h + BULK_REQ_OFF.destBlock, req.destBlock);
  put32(out, h + BULK_REQ_OFF.payloadLen, BULK_PAYLOAD_BYTES);
  put32(out, h + BULK_REQ_OFF.payloadCrc32, crc32(req.payload));
  out.set(req.payload, h + BULK_REQ_HEADER_BYTES);
  return out;
}

export function parseBulkHeader(header: Uint8Array): {
  version: number;
  seq: number;
  destBlock: number;
  payloadLen: number;
  payloadCrc32: number;
} {
  return {
    version: header[BULK_REQ_OFF.version]!,
    seq: get32(header, BULK_REQ_OFF.seq),
    destBlock: get32(header, BULK_REQ_OFF.destBlock),
    payloadLen: get32(header, BULK_REQ_OFF.payloadLen),
    payloadCrc32: get32(header, BULK_REQ_OFF.payloadCrc32),
  };
}

export function buildBulkResponse(
  status: number,
  seq: number,
  destBlock: number,
  verifiedCrc32: number,
): Uint8Array {
  const out = new Uint8Array(BULK_RESP_BYTES);
  out[BULK_RESP_OFF.status] = status & 255;
  put32(out, BULK_RESP_OFF.seq, seq);
  put32(out, BULK_RESP_OFF.destBlock, destBlock);
  put32(out, BULK_RESP_OFF.verifiedCrc32, verifiedCrc32);
  out[BULK_RESP_OFF.retryable] = bulkStatusIsRetryable(status) ? 1 : 0;
  return out;
}

export function parseBulkResponse(bytes: Uint8Array): BulkResponse {
  if (bytes.length < BULK_RESP_BYTES) throw new Error("short bulk response");
  return {
    status: bytes[BULK_RESP_OFF.status]!,
    seq: get32(bytes, BULK_RESP_OFF.seq),
    destBlock: get32(bytes, BULK_RESP_OFF.destBlock),
    verifiedCrc32: get32(bytes, BULK_RESP_OFF.verifiedCrc32),
    retryable: bytes[BULK_RESP_OFF.retryable] === 1,
  };
}

/* ---------- 'Q' capability extension ("STBC") ---------- */

export const BULK_CAPS_BYTES = 12;
export const BULK_CAPS_TAG = "STBC";
export const BULK_CAPS_OFF = { tag: 0, flags: 4, maxSectorBytes: 8 } as const;
export const BULK_CAP_FLAG_SUPPORTED = 1 << 0;

export interface BulkCapabilities {
  supported: boolean;
  flags: number;
  maxSectorBytes: number;
}

export function buildBulkCaps(): Uint8Array {
  const out = new Uint8Array(BULK_CAPS_BYTES);
  out.set(new TextEncoder().encode(BULK_CAPS_TAG), BULK_CAPS_OFF.tag);
  put32(out, BULK_CAPS_OFF.flags, BULK_CAP_FLAG_SUPPORTED);
  put32(out, BULK_CAPS_OFF.maxSectorBytes, BULK_PAYLOAD_BYTES);
  return out;
}

/**
 * Support is only ever taken from this explicit, self-describing extension —
 * never inferred from a firmware or protocol version number.
 */
export function parseBulkCaps(bytes: Uint8Array | null | undefined): BulkCapabilities | null {
  if (!bytes || bytes.length < BULK_CAPS_BYTES) return null;
  if (String.fromCharCode(...bytes.slice(0, 4)) !== BULK_CAPS_TAG) return null;
  const flags = get32(bytes, BULK_CAPS_OFF.flags);
  const maxSectorBytes = get32(bytes, BULK_CAPS_OFF.maxSectorBytes);
  return {
    supported: (flags & BULK_CAP_FLAG_SUPPORTED) !== 0 && maxSectorBytes >= BULK_PAYLOAD_BYTES,
    flags,
    maxSectorBytes,
  };
}

/** The one destination rule: region_start + seq * 16. */
export function bulkDestBlock(regionStart: number, seq: number): number {
  return regionStart + seq * BULK_BLOCKS_PER_SECTOR;
}
