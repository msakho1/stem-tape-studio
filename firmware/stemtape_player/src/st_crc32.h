/*
 * st_crc32.h — CRC-32 (IEEE 802.3, reflected 0xEDB88320), matching the
 * polynomial the classic SP-1 Tape Looper firmware already uses for its own
 * index-repair checks. PURE, bitwise (no table): transfer mode already
 * pauses audio, so this never runs under a real-time deadline.
 */

#ifndef STEMTAPE_PLAYER_CRC32_H_
#define STEMTAPE_PLAYER_CRC32_H_

#include <stddef.h>
#include <stdint.h>

/* Streaming update: pass ST_CRC32_INIT (or a prior call's return value) as
 * `crc`. The caller XORs the FINAL accumulated value with 0xFFFFFFFF to get
 * the standard CRC-32 output (st_crc32_compute() does this for a one-shot
 * buffer already). */
uint32_t st_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

/* One-shot compute over a single buffer; returns the final CRC-32 value
 * (already XOR-finalized), matching what a companion tool's standard CRC-32
 * library produces. */
uint32_t st_crc32_compute(const uint8_t *data, size_t len);

#endif /* STEMTAPE_PLAYER_CRC32_H_ */
