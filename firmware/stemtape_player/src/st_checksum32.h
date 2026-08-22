/*
 * st_checksum32.h — FNV-1a (32-bit), the content-checksum algorithm the
 * v1.1 companion contract uses for STIX v2 index fields stemChecksums[]
 * (offset 72) and songChecksum (offset 88) -- see src/sp1/song.ts's own
 * `checksum32()` (h = 0x811c9dc5; per byte: h ^= byte; h = (h *
 * 0x01000193) mod 2^32).
 *
 * This is DELIBERATELY NOT the same algorithm as st_crc32.c (CRC-32 IEEE
 * 802.3), which the v1.1 contract uses only for a STIX record's OWN
 * integrity field (offset 252, ST11_IX_OFF_CRC32) -- confusing the two
 * silently produces plausible-looking but wrong checksums. Verified
 * byte-for-byte against handoff/v1.1/binaries/song-sectors-four-stem.bin's
 * real, companion-declared per-stem and song checksums (see
 * tests/test_stem_v11.c): every one of the 4 stem checksums matches
 * st_checksum32_compute() over that stem's full (shared-length, silence-
 * padded) decoded PCM24 bytes, and the song checksum matches
 * st_checksum32_compute() over the 16-byte little-endian digest of the
 * four stem checksums, in stem order.
 *
 * PURE: no I/O, no Zephyr.
 */

#ifndef STEMTAPE_PLAYER_CHECKSUM32_H_
#define STEMTAPE_PLAYER_CHECKSUM32_H_

#include <stddef.h>
#include <stdint.h>

#define ST_CHECKSUM32_INIT 0x811c9dc5u

/* Streaming update: pass ST_CHECKSUM32_INIT (or a prior call's return
 * value) as `h`. Unlike CRC-32, FNV-1a needs no final XOR -- the running
 * value IS the checksum at any point. */
uint32_t st_checksum32_update(uint32_t h, const uint8_t *data, size_t len);

/* One-shot compute over a single buffer. */
uint32_t st_checksum32_compute(const uint8_t *data, size_t len);

#endif /* STEMTAPE_PLAYER_CHECKSUM32_H_ */
