/*
 * st_xfer_wire.h — pure byte-level (de)serialization for the Stem Tape
 * companion transfer protocol's 'V'/'B'/'D'/'I' payloads (see
 * docs/stem-tape-transfer-v1.md sections 2 and 6). 'S'/'K'/'C'/'A'/'X'
 * carry no interesting payload of their own beyond a raw ST_SECTOR_BYTES
 * body ('S') or nothing at all, so they are not duplicated here.
 *
 * Every multi-byte field is little-endian, explicitly packed one at a
 * time -- the SAME reasoning st_storage_layout.c documents for the
 * on-disk format applies to the wire format: never `memcpy` a struct
 * across the USB boundary. This is a faithful, independent
 * reimplementation of the exact byte layout main.c's real xfer_service()
 * uses (that function is Zephyr-only and not host-testable directly), so
 * this is the SAME parser tested here that the firmware actually runs --
 * matching the precedent st_midi_queue.c already set in this codebase for
 * a Zephyr-only receive path.
 *
 * PURE: no Zephyr, no I/O.
 */

#ifndef STEMTAPE_PLAYER_XFER_WIRE_H_
#define STEMTAPE_PLAYER_XFER_WIRE_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_transfer.h"

/* 'V' response: exactly ST_XFER_VERSION_RSP_LEN (16) bytes.
 *   bytes 0..3   "STV1"
 *   byte  4      ST_XFER_PROTOCOL_MINOR
 *   byte  5      ST_STORAGE_LAYOUT_VERSION
 *   bytes 6..7   reserved, 0
 *   bytes 8..11  capability flags (u32 LE)
 *   bytes 12..15 ST_SECTOR_BYTES (u32 LE)
 */
void st_xfer_wire_encode_version(uint8_t out[16], uint32_t capability_flags);

/*
 * 'B' request: exactly ST_XFER_WIRE_BEGIN_REQ_LEN (117) bytes --
 * slot(u16 LE) then the full st_xfer_song_meta_t, field by field:
 *   song_id_hash(u32) frame_count(u32) expected_crc32(u32)
 *   stem_present_mask(u8) stem_content_frames[4](u32 each)
 *   stem_crc32[4](u32 each) bpm_q8(u16) downbeat_frame(u32)
 *   title[32] artist[32]
 * (Corrects docs/stem-tape-transfer-v1.md's original summary table, which
 * predated st_xfer_song_meta_t's final field set and undersized
 * frame_count as u64/omitted the per-stem + title/artist fields the
 * directive requires -- ST_XFER_WIRE_BEGIN_REQ_LEN is the real,
 * authoritative size; the doc is updated to match this implementation.)
 * Returns false (does not modify slot or meta) if `len` is short.
 */
#define ST_XFER_WIRE_BEGIN_REQ_LEN 117u
bool st_xfer_wire_decode_begin_req(const uint8_t *buf, uint32_t len,
				    uint16_t *slot, st_xfer_song_meta_t *meta);

/* 'b' response: 'b' + resume_sector (u32 LE) -- a SECTOR index (matching
 * what st_xfer_begin() actually reports and what 'S' actually consumes),
 * not a byte offset (correcting the doc's original "byte offset" wording). */
#define ST_XFER_WIRE_BEGIN_RSP_LEN 5u
void st_xfer_wire_encode_begin_rsp(uint8_t out[ST_XFER_WIRE_BEGIN_RSP_LEN], uint32_t resume_sector);

/* 'D' request: slot(u16 LE) + ST_DESTRUCTIVE_CONFIRM_LEN(8)-byte token. */
#define ST_XFER_WIRE_DELETE_REQ_LEN 10u
bool st_xfer_wire_decode_delete_req(const uint8_t *buf, uint32_t len,
				     uint16_t *slot, const uint8_t **token_out);

#endif /* STEMTAPE_PLAYER_XFER_WIRE_H_ */
