/**
 * CRC-32 (IEEE 802.3), reflected, polynomial 0xEDB88320, init 0xFFFFFFFF,
 * final XOR 0xFFFFFFFF. This is the only checksum algorithm used by the STIX
 * v2 index record. It is NOT used anywhere in the inherited Tape Looper block
 * protocol, which carries no CRC at all.
 */

const TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

/**
 * CRC-32 over `bytes[from, to)`. Byte positions listed in `zeroed` are treated
 * as 0x00 during calculation regardless of their stored value — this is how the
 * STIX validity magic is excluded from CRC coverage so that the very same CRC
 * verifies both the uncommitted (magic = 0) and committed (magic = 'STIX')
 * image of one index record.
 */
export function crc32(bytes: Uint8Array, from = 0, to = bytes.length, zeroed?: { from: number; to: number }): number {
  let c = 0xffffffff;
  for (let i = from; i < to; i++) {
    const masked = zeroed && i >= zeroed.from && i < zeroed.to ? 0 : bytes[i]!;
    c = TABLE[(c ^ masked) & 0xff]! ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}
