/*
 * st_sector_codec.h — the real, documented SP-1 sector audio codec.
 *
 * Source: timknapen/SP-1-dev wiki, "Data Structure" and "Audio format"
 * pages (fetched via https://raw.githubusercontent.com/wiki/timknapen/
 * SP-1-dev/<Page>.md — the direct github.com/.../wiki/<Page> URLs 404 on
 * this hardware's wiki, the raw wiki-git content URL does not).
 *
 * Layout of one ST_SECTOR_BYTES (8192-byte) sector:
 *   4 sub-blocks of ST_SUBBLOCK_BYTES (2048) bytes each, read/written in
 *   PHYSICAL sub-block order {0, 2, 1, 3} (NOT sequential) -- the wiki's
 *   documented interleave, preserved here exactly rather than "simplified"
 *   to sequential order.
 *
 *   Each sub-block:
 *     - ST_SUBBLOCK_FRAMES (85) audio frames, ST_FRAME_BYTES (24) bytes each
 *       = 2040 bytes
 *     - 8 reserved bytes at the sub-block's local offset 2040..2047:
 *         [0..1] sync
 *         [2..3] tempo
 *         [4..7] LED
 *
 *   4 sub-blocks * 85 frames/sub-block = ST_SECTOR_FRAME_CAPACITY (340)
 *   audio frames per sector (7.083 ms at 48 kHz).
 *
 *   Each 24-byte audio frame packs ST_STEM_COUNT (4) stem-frames of 6 bytes
 *   each (one stereo L/R sample pair per stem, 24-bit each). The per-stem
 *   6-byte layout is NOT the obvious (L-MSB,L-mid,L-LSB,R-MSB,R-mid,R-LSB)
 *   packing -- the wiki documents an interleaved byte order:
 *     byte0 = L mid, byte1 = L MSB, byte2 = R MSB,
 *     byte3 = L LSB, byte4 = R LSB, byte5 = R mid
 *   i.e. L sample = (byte1, byte0, byte3) as (MSB, mid, LSB)
 *        R sample = (byte2, byte5, byte4) as (MSB, mid, LSB)
 *   This is preserved exactly, not "corrected" to a conventional order --
 *   existing stock SP-1 albums (and this device's own codec DMA framing)
 *   depend on it.
 *
 * PURE: no I/O, no Zephyr, no dynamic allocation. Every function operates
 * on caller-supplied fixed-size buffers so this is safe to call from a
 * bounded-stack audio-adjacent context.
 */

#ifndef STEMTAPE_PLAYER_SECTOR_CODEC_H_
#define STEMTAPE_PLAYER_SECTOR_CODEC_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_storage_layout.h"

#define ST_SUBBLOCK_COUNT        4u
#define ST_SUBBLOCK_BYTES        2048u
#define ST_SUBBLOCK_FRAMES       85u
#define ST_SUBBLOCK_RESERVED_BYTES 8u
#define ST_FRAME_BYTES           24u   /* 4 stems * 6 bytes */
#define ST_STEM_FRAME_BYTES      6u

/* Physical sub-block read/write order within a sector -- NOT sequential. */
static const uint8_t ST_SUBBLOCK_PHYSICAL_ORDER[ST_SUBBLOCK_COUNT] = { 0u, 2u, 1u, 3u };

#if !defined(__cplusplus)
_Static_assert(ST_SUBBLOCK_COUNT * ST_SUBBLOCK_BYTES == ST_SECTOR_BYTES,
	       "sub-block geometry does not tile the sector exactly");
_Static_assert(ST_SUBBLOCK_FRAMES * ST_FRAME_BYTES + ST_SUBBLOCK_RESERVED_BYTES ==
		       ST_SUBBLOCK_BYTES,
	       "sub-block frame+reserved geometry does not fill the sub-block exactly");
_Static_assert(ST_SUBBLOCK_COUNT * ST_SUBBLOCK_FRAMES == ST_SECTOR_FRAME_CAPACITY,
	       "sub-block frame count does not add up to the documented per-sector capacity");
_Static_assert(ST_STEM_COUNT * ST_STEM_FRAME_BYTES == ST_FRAME_BYTES,
	       "stem-frame geometry does not fill the audio frame exactly");
#endif

/* One decoded audio frame: 4 stems * (L, R), each a sign-extended 24-bit
 * sample widened to int32_t. */
typedef struct {
	int32_t stem_l[ST_STEM_COUNT];
	int32_t stem_r[ST_STEM_COUNT];
} st_audio_frame_t;

/* Per-sector reserved metadata, decoded from all 4 sub-blocks' 8-byte
 * reserved regions (32 bytes/sector total). */
typedef struct {
	uint16_t sync[ST_SUBBLOCK_COUNT];
	uint16_t tempo[ST_SUBBLOCK_COUNT];
	uint8_t  led[ST_SUBBLOCK_COUNT][4];
} st_sector_reserved_t;

/*
 * Encodes exactly ST_SECTOR_FRAME_CAPACITY frames (caller pads with silence
 * past real content -- see st_storage_layout.h's stem_content_frames) into
 * one ST_SECTOR_BYTES sector buffer, in the documented physical sub-block
 * order and per-stem byte interleave.
 */
void st_sector_encode(const st_audio_frame_t frames[ST_SECTOR_FRAME_CAPACITY],
		       const st_sector_reserved_t *reserved,
		       uint8_t out[ST_SECTOR_BYTES]);

/*
 * Inverse of st_sector_encode(): decodes one sector buffer into
 * ST_SECTOR_FRAME_CAPACITY frames plus the reserved metadata.
 */
void st_sector_decode(const uint8_t in[ST_SECTOR_BYTES],
		       st_audio_frame_t frames_out[ST_SECTOR_FRAME_CAPACITY],
		       st_sector_reserved_t *reserved_out);

#endif /* STEMTAPE_PLAYER_SECTOR_CODEC_H_ */
