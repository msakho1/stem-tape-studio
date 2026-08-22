/*
 * st_sector_v11.h — the real, verified v1.1 companion sector codec ("STSC").
 *
 * Source: docs/stem-tape-transfer-v1.1.md section 8 + 12.3, and the
 * companion's own src/sp1/sector.ts (frozen at handoff/v1.1/) -- NOT the
 * timknapen wiki format st_sector_codec.c implements for the classic
 * looper's own disjoint storage region (see st_v11_format.h's own comment
 * for why those are two different, non-interchangeable formats).
 *
 * Layout of one ST11_SECTOR_BYTES (8192-byte) sector:
 *   ST11_SECTOR_HEADER_BYTES (32) bytes: magic('STSC'), sectorIndex,
 *   firstFrame, frameCount, bpmQ8, downbeatFrame, 4 firmware-owned LED
 *   bytes, 4 reserved bytes -- see st_v11_format.h's ST11_SECTOR_OFF_*.
 *   Then ST11_FRAMES_PER_SECTOR (340) frames of ST11_BYTES_PER_FRAME (24)
 *   bytes each, LINEARLY, no sub-block splitting or reordering.
 *
 *   Each 24-byte frame packs ST11_STEM_COUNT (4) stems' stereo pairs,
 *   stem-major then channel-major: vocal L, vocal R, drums L, drums R,
 *   bass L, bass R, inst L, inst R -- each sample a CONVENTIONAL signed
 *   24-bit little-endian value (byte0=LSB, byte1=mid, byte2=MSB). Verified
 *   byte-for-byte and sample-for-sample against handoff/v1.1/binaries/
 *   song-sectors-four-stem.bin (see tests/test_stem_v11.c): decoding that
 *   fixture's real sectors with this codec and FNV-1a-checksumming each
 *   stem's full recovered PCM24 stream reproduces the companion's own
 *   declared per-stem and song checksums exactly.
 *
 * The final, partial sector of a song is short (frameCount <
 * ST11_FRAMES_PER_SECTOR); this codec only touches that many frames on
 * decode, and st11_sector_encode() only WRITES that many (any trailing
 * frame slots in a short sector are the caller's -- typically zeroed --
 * concern, matching the companion's own encodeSector(), which never emits
 * more than `frameCount` frames of real data per sector).
 *
 * PURE: no I/O, no Zephyr, no dynamic allocation. Every function operates
 * on caller-supplied fixed-size buffers.
 */

#ifndef STEMTAPE_PLAYER_SECTOR_V11_H_
#define STEMTAPE_PLAYER_SECTOR_V11_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_v11_format.h"

/* One decoded audio frame: 4 stems * (L, R), each a sign-extended 24-bit
 * sample widened to int32_t -- same shape as st_sector_codec.h's
 * st_audio_frame_t, kept as a separate type since the two codecs are not
 * interchangeable (see this file's own doc comment). */
typedef struct {
	int32_t stem_l[ST11_STEM_COUNT];
	int32_t stem_r[ST11_STEM_COUNT];
} st11_audio_frame_t;

typedef struct {
	uint32_t sector_index;
	uint32_t first_frame;
	uint32_t frame_count; /* <= ST11_FRAMES_PER_SECTOR; short on the final sector */
	uint32_t bpm_q8;
	uint32_t downbeat_frame;
} st11_sector_header_t;

/*
 * Encodes `frame_count` frames (<= ST11_FRAMES_PER_SECTOR; the caller pads
 * with silence past real content, matching the companion's own
 * encodeSector()) into one ST11_SECTOR_BYTES sector buffer. Any sector
 * bytes past the header + frame_count*ST11_BYTES_PER_FRAME are zeroed
 * (matching a freshly zero-initialized companion Uint8Array).
 */
void st11_sector_encode(uint32_t sector_index, uint32_t first_frame, uint32_t frame_count,
			 uint32_t bpm_q8, uint32_t downbeat_frame,
			 const st11_audio_frame_t *frames, /* [frame_count] */
			 uint8_t out[ST11_SECTOR_BYTES]);

/*
 * Reads and validates just the 32-byte header (magic check only -- callers
 * needing full STIX-level validation do that separately). Returns false
 * (header_out unmodified) if the magic does not match ST11_SECTOR_MAGIC.
 */
bool st11_sector_read_header(const uint8_t in[ST11_SECTOR_BYTES], st11_sector_header_t *header_out);

/*
 * Decodes ONE frame (frame_index in [0, ST11_FRAMES_PER_SECTOR)) out of a
 * sector buffer, without expanding the whole sector to an array -- the
 * v1.1 analogue of st_sector_codec.h's st_sector_decode_frame(), for
 * callers (a future verify/playback path) that only need one decoded frame
 * at a time and want to keep the RAM cost to sizeof(st11_audio_frame_t)
 * (32 bytes) instead of a full 340-frame (10,880-byte) buffer.
 */
void st11_sector_decode_frame(const uint8_t in[ST11_SECTOR_BYTES], uint32_t frame_index,
			       st11_audio_frame_t *frame_out);

#endif /* STEMTAPE_PLAYER_SECTOR_V11_H_ */
