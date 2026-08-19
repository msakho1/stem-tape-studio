/*
 * st_bulk_xfer.h — Stem Tape bulk verified-sector upload: wire contract +
 * pure per-session sequence state machine.
 *
 * REPLACES the production song-data write path's per-block granularity
 * (512 bytes/round-trip -> 509,024 round trips for a real 248.5 MiB song,
 * the proven cause this slice exists to fix) with ONE new command, 'U',
 * that transfers, writes, and verifies one COMPLETE 8192-byte Stem Tape
 * v1.1 sector (16 physical blocks) per round trip -- 31,814 round trips
 * for the same song. Additive only: 'P'/'Q'/'R'/'W'/'F'/'X' are unchanged,
 * and 'W' remains the ONLY way to write an index record (this module is
 * never used for index blocks -- see st_ab_session.h's own doc on why the
 * magic-commit detection stays exclusively on the single-block path).
 *
 * WIRE CONTRACT (frozen -- see docs/stem-tape-bulk-upload-v1.md for the
 * full companion-facing spec, transcripts, and Q/STCP capability
 * extension):
 *
 *   host -> device: 'U' <request header, 17 bytes, LE> <payload, exactly
 *                    ST_BULK_PAYLOAD_BYTES = 8192 bytes>
 *   device -> host: <response, 14 bytes, LE>
 *
 *   Request header:
 *     [0]      version      u8  -- must equal ST_BULK_PROTO_VERSION
 *     [1..5)   seq          u32 -- transaction/sequence number, 0-based,
 *                                  strictly sequential per open session
 *     [5..9)   dest_block   u32 -- absolute physical block this sector's
 *                                  first block must land at
 *     [9..13)  payload_len  u32 -- must equal ST_BULK_PAYLOAD_BYTES
 *     [13..17) payload_crc32 u32 -- CRC-32 (IEEE 802.3, st_crc32.c) of the
 *                                  8192-byte payload that follows
 *
 *   Response:
 *     [0]      status       u8  -- st_bulk_status_t (0 = OK)
 *     [1..5)   seq          u32 -- echoed
 *     [5..9)   dest_block   u32 -- echoed
 *     [9..13)  verified_crc32 u32 -- CRC-32 the device actually computed
 *                                  from the bytes READ BACK off eMMC after
 *                                  writing (0 if write/readback never
 *                                  happened); comparing this against the
 *                                  request's own payload_crc32 IS the
 *                                  read-back verification -- the host
 *                                  never needs a separate 512-byte-at-a-
 *                                  time readback pass for song data again.
 *     [13]     retryable    u8  -- 1 if resending this EXACT request
 *                                  (same seq/dest_block/payload) is safe
 *                                  and may succeed; 0 if the request
 *                                  itself is structurally wrong (retrying
 *                                  it verbatim can never succeed -- the
 *                                  host must fix its own state first)
 *
 * SEQUENCING / IDEMPOTENCY: seq must equal the session's next expected
 * value (a genuinely new sector) OR the immediately preceding, already-
 * accepted value (a lost-ACK retry of the last successful sector) --
 * st_bulk_seq_check() below is the pure decision function for this, and
 * ST_BULK_SEQ_RETRY is always safe to reprocess in full (rewriting the
 * same bytes to the same block is idempotent). Any other seq value fails
 * closed as out-of-order. dest_block must independently agree with what
 * seq implies (region_start + seq*16) -- a request that gets the sequence
 * number right but the destination wrong is rejected, never guessed at.
 *
 * PURE: this module does no I/O and touches no eMMC/CDC state -- it only
 * parses/builds the fixed-width wire structures above and tracks the
 * per-session sequence/bounds bookkeeping over caller-supplied values.
 * The REAL bounds/active-region safety boundary remains exactly
 * st_ab_session_check_write(), called once per physical block by the real
 * command handler (main.c) -- st_bulk_seq_check()'s own bounds check
 * below is a cheap, early, redundant-but-harmless fast-rejection, never
 * the authoritative gate.
 */

#ifndef STEMTAPE_PLAYER_BULK_XFER_H_
#define STEMTAPE_PLAYER_BULK_XFER_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_v11_format.h"

#define ST_BULK_CMD            'U' /* 0x55 -- unused by every prior/current verb (P/Q/R/W/F/X/Y) */
#define ST_BULK_PROTO_VERSION  1u

#define ST_BULK_BLOCKS_PER_SECTOR ST11_BLOCKS_PER_SECTOR /* 16 */
#define ST_BULK_PAYLOAD_BYTES     ST11_SECTOR_BYTES      /* 8192 */

#define ST_BULK_REQ_HEADER_BYTES 17u
#define ST_BULK_RESP_BYTES       14u

#define ST_BULK_REQ_OFF_VERSION       0u  /* u8 */
#define ST_BULK_REQ_OFF_SEQ           1u  /* u32 */
#define ST_BULK_REQ_OFF_DEST_BLOCK    5u  /* u32 */
#define ST_BULK_REQ_OFF_PAYLOAD_LEN   9u  /* u32 */
#define ST_BULK_REQ_OFF_PAYLOAD_CRC32 13u /* u32 */

#define ST_BULK_RESP_OFF_STATUS        0u  /* u8 */
#define ST_BULK_RESP_OFF_SEQ           1u  /* u32 */
#define ST_BULK_RESP_OFF_DEST_BLOCK    5u  /* u32 */
#define ST_BULK_RESP_OFF_VERIFIED_CRC  9u  /* u32 */
#define ST_BULK_RESP_OFF_RETRYABLE     13u /* u8 */

typedef struct {
	uint8_t  version;
	uint32_t seq;
	uint32_t dest_block;
	uint32_t payload_len;
	uint32_t payload_crc32;
} st_bulk_req_header_t;

/* Parses the ST_BULK_REQ_HEADER_BYTES-byte request header already received
 * off the wire. Does not touch or require the payload itself -- pure
 * byte-order decode only. */
void st_bulk_parse_header(const uint8_t in[ST_BULK_REQ_HEADER_BYTES], st_bulk_req_header_t *out);

/* Every precise failure/success code this contract can report -- see this
 * header's own top comment and docs/stem-tape-bulk-upload-v1.md for the
 * full description of each. Values are frozen wire constants: do not
 * renumber an existing entry, only append. */
typedef enum {
	ST_BULK_OK = 0,
	ST_BULK_ERR_UNSUPPORTED_VERSION = 1,   /* request header's version != ST_BULK_PROTO_VERSION */
	ST_BULK_ERR_BAD_LENGTH = 2,            /* payload_len != ST_BULK_PAYLOAD_BYTES */
	ST_BULK_ERR_TIMEOUT_PAYLOAD = 3,       /* payload receive stalled/timed out before all bytes arrived */
	ST_BULK_ERR_CDC_OVERFLOW = 4,          /* the CDC RX ring dropped bytes during this payload's receive */
	ST_BULK_ERR_CRC_MISMATCH = 5,          /* received payload's own CRC-32 != declared payload_crc32 */
	ST_BULK_ERR_LAYOUT_NOT_READY = 6,      /* g_v11_layout_ready is false -- no v1.1 layout at all */
	ST_BULK_ERR_NO_SESSION = 7,            /* no v1.1 write session currently open */
	ST_BULK_ERR_SESSION_CLOSED = 8,        /* the open session already committed (single-use latch tripped) */
	ST_BULK_ERR_OUT_OF_SEQUENCE = 9,       /* seq is neither the expected next value nor the one legal retry */
	ST_BULK_ERR_DEST_MISMATCH = 10,        /* seq is acceptable but dest_block disagrees with what it implies */
	ST_BULK_ERR_OUT_OF_BOUNDS = 11,        /* this sector would run past the frozen inactive region's own capacity */
	ST_BULK_ERR_ACTIVE_REGION = 12,        /* st_ab_session_check_write() reports the active song/index region */
	ST_BULK_ERR_OUTSIDE_FROZEN_PAIR = 13,  /* st_ab_session_check_write() reports outside the frozen pair entirely */
	ST_BULK_ERR_EMMC_WRITE_FAIL = 14,      /* the real multi-block eMMC program operation failed */
	ST_BULK_ERR_EMMC_READBACK_FAIL = 15,   /* the real multi-block eMMC read-back operation failed */
	ST_BULK_ERR_READBACK_CRC_MISMATCH = 16, /* bytes actually read back off eMMC do not CRC-match what was sent */
} st_bulk_status_t;

/* Builds the ST_BULK_RESP_BYTES-byte response. `verified_crc32` should be
 * 0 on any status that never reached a real read-back. */
void st_bulk_build_response(st_bulk_status_t status, uint32_t seq, uint32_t dest_block, uint32_t verified_crc32,
			     uint8_t out[ST_BULK_RESP_BYTES]);

/* Whether resending the EXACT same request (unchanged seq/dest_block/
 * payload) is safe and may succeed -- transient/physical failures are
 * retryable; structural/protocol errors are not (the host must fix its
 * own state -- e.g. re-query 'Q', or abort -- before trying again). */
bool st_bulk_status_is_retryable(st_bulk_status_t status);

/* ---- per-session sequence + destination bookkeeping (pure) ------------ */

typedef struct {
	bool     has_committed;   /* true once at least one sector has been accepted this session */
	uint32_t next_seq;        /* the seq value a genuinely NEW sector must present next */
	uint32_t region_start;    /* the session's frozen inactive song region's own base block */
	uint32_t region_cap;      /* that region's own capacity, in physical blocks */
} st_bulk_seq_t;

/* Resets tracking to "session just (re)opened, expect seq=0 at
 * region_start". Call whenever the v1.1 write session (re)opens (i.e.
 * every real 'Q', matching xfer_v11_refresh_session()'s own existing
 * cadence) -- a session always starts a bulk upload at the frozen
 * region's own first sector, by construction (docs section 5 step 8: the
 * song is written into the inactive region from its start). */
void st_bulk_seq_reset(st_bulk_seq_t *sq, uint32_t region_start, uint32_t region_cap);

typedef enum {
	ST_BULK_SEQ_NEW,           /* seq == next_seq, dest_block matches, in bounds: a genuinely new sector */
	ST_BULK_SEQ_RETRY,         /* seq == next_seq - 1 (the most recently accepted one), dest_block matches:
				     * a legal idempotent retry -- safe to reprocess in full, does not advance
				     * next_seq again */
	ST_BULK_SEQ_OUT_OF_ORDER,  /* seq is neither of the above */
	ST_BULK_SEQ_DEST_MISMATCH, /* seq is NEW- or RETRY-eligible but dest_block disagrees with region_start +
				     * seq*ST_BULK_BLOCKS_PER_SECTOR */
	ST_BULK_SEQ_OUT_OF_BOUNDS, /* this sector's own 16 blocks would run past region_start + region_cap */
} st_bulk_seq_check_t;

/* Pure decision function -- does NOT mutate `sq`. The caller (main.c's
 * real handler) advances the tracker only via st_bulk_seq_advance(),
 * only after the whole write+read-back+CRC round trip for a NEW sector
 * has genuinely succeeded -- never on the strength of an ACK alone (a
 * lost ACK simply re-enters this same check as ST_BULK_SEQ_RETRY next
 * time, without ever having advanced falsely). */
st_bulk_seq_check_t st_bulk_seq_check(const st_bulk_seq_t *sq, uint32_t seq, uint32_t dest_block);

/* Advances the tracker's next_seq by one. Call ONLY after a genuinely NEW
 * (st_bulk_seq_check() returned ST_BULK_SEQ_NEW) sector's write+read-
 * back+CRC all actually succeeded -- never on a retry (which must leave
 * next_seq unchanged) and never speculatively before the write is
 * verified. Defensive no-op if `seq` != sq->next_seq at the time of the
 * call (guards against a caller-side ordering bug silently corrupting
 * the tracker). */
void st_bulk_seq_advance(st_bulk_seq_t *sq, uint32_t seq);

#endif /* STEMTAPE_PLAYER_BULK_XFER_H_ */
