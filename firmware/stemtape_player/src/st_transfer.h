/*
 * st_transfer.h — Stem Tape companion transfer protocol v1: the
 * transactional upload state machine (docs/stem-tape-transfer-v1.md
 * section 6: begin/resume, stage, verify, commit, abort, delete, init).
 *
 * PURE: no Zephyr, no direct eMMC driver calls. Sector I/O is injected via
 * function pointers (st_sector_write_fn / st_sector_read_fn) exactly like
 * led_render_policy.c injects its physical PWM write — the real firmware
 * binds these to emmc_write_blocks()/emmc_read_blocks(); the host tests
 * bind them to an in-memory mock, so the actual transactional guarantee
 * (a failed/interrupted upload can never expose partial data, and never
 * loses the previously committed song) is proven here, not just documented.
 */

#ifndef STEMTAPE_PLAYER_TRANSFER_H_
#define STEMTAPE_PLAYER_TRANSFER_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_storage_layout.h"

typedef enum {
	ST_XFER_OK = 0,
	ST_XFER_ERR_BAD_SLOT,
	ST_XFER_ERR_BAD_SECTOR,
	ST_XFER_ERR_SECTOR_CRC,
	ST_XFER_ERR_PAYLOAD_CRC,
	ST_XFER_ERR_NOT_VERIFIED,
	ST_XFER_ERR_NO_TRANSACTION,
	ST_XFER_ERR_WRITE_FAILED,
	ST_XFER_ERR_READ_FAILED,
	ST_XFER_ERR_BAD_TOKEN,
	ST_XFER_ERR_TOO_LARGE,
	ST_XFER_ERR_BAD_TIMING,        /* a stem_content_frames[i] > frame_count, or a
					 * downbeat_frame >= frame_count */
	ST_XFER_ERR_VERSION,           /* companion's declared protocol major != ours */
	ST_XFER_ERR_FORMAT,            /* declared layout_version != ST_STORAGE_LAYOUT_VERSION */
	ST_XFER_ERR_CAPACITY,          /* no free slot / device reports fewer usable sectors
					 * than the staging region requires */
	ST_XFER_ERR_INTERRUPTED_COMMIT,/* commit gate reached with a transaction that is open
					 * but was left over from a prior, never-verified attempt */
} st_xfer_result_t;

/* Sector I/O, injected. Both return 0 on success, matching the classic
 * looper's emmc_read_blocks()/emmc_write_blocks() bool-success convention
 * inverted to an int rc (0 = ok) for consistency with led_render_policy.h's
 * write_fn convention elsewhere in this codebase. `sector` is an ABSOLUTE
 * sector number (already offset by ST_STAGING_SECTOR0 by the caller). */
typedef int (*st_sector_write_fn)(uint32_t sector, const uint8_t data[ST_SECTOR_BYTES], void *ctx);
typedef int (*st_sector_read_fn)(uint32_t sector, uint8_t out[ST_SECTOR_BYTES], void *ctx);

/* Everything the companion must declare up front for a song -- explicit,
 * independent per-stem length + checksum (task requirement: "Represent
 * title, artist, BPM, downbeat/timing information, and the independent
 * length and checksum of each of the four stems"), not inferred from the
 * shared frame_count. */
typedef struct {
	uint32_t song_id_hash;
	uint32_t frame_count;                       /* shared sector-grid length, all 4 stems */
	uint32_t expected_crc32;                    /* whole-payload CRC over every staged
						      * sector's raw encoded bytes, in order */
	uint8_t  stem_present_mask;
	uint32_t stem_content_frames[ST_STEM_COUNT]; /* per-stem REAL length, <= frame_count */
	uint32_t stem_crc32[ST_STEM_COUNT];          /* per-stem CRC over that stem's decoded
						      * L/R samples across its content frames
						      * only (silence padding excluded) */
	uint16_t bpm_q8;
	uint32_t downbeat_frame;
	char     title[32];
	char     artist[32];
} st_xfer_song_meta_t;

typedef struct {
	bool     open;
	uint16_t slot;
	st_xfer_song_meta_t meta;
	uint32_t total_sectors;      /* st_storage_song_sectors(frame_count) */
	uint32_t staged_through;     /* highest sector index CONFIRMED staged (crc-checked), i.e. the resume point */
	bool     verified;           /* K succeeded since the most recent S */
} st_xfer_txn_t;

void st_xfer_txn_reset(st_xfer_txn_t *t);

/*
 * B: begin or resume. `slot` must be < `total_slots`. `meta->frame_count`
 * must fit ST_STAGING_SECTOR_COUNT or this fails with ST_XFER_ERR_TOO_LARGE
 * (staging can never overflow into the song-data region). Each
 * `meta->stem_content_frames[i]` must be <= `meta->frame_count` or this
 * fails with ST_XFER_ERR_BAD_TIMING. Sending an IDENTICAL (slot, frame_count,
 * expected_crc32) tuple again while a transaction for that slot is already
 * open is a RESUME: `*resume_sector` reports `staged_through` unchanged. A
 * DIFFERENT tuple discards any prior staging progress for that slot first
 * (`*resume_sector` reports 0).
 */
st_xfer_result_t st_xfer_begin(st_xfer_txn_t *t, uint16_t slot, const st_xfer_song_meta_t *meta,
				uint32_t total_slots, uint32_t *resume_sector);

/*
 * S: stage one sector, 0-based `sector_index` within THIS transfer (the
 * glue adds ST_STAGING_SECTOR0 before calling write_fn). Any sector index
 * != `staged_through` is rejected (sectors must land in order — no gaps
 * that a later verify could silently skip). Any write via `write_fn`
 * failing, and any sector CRC mismatch, leaves `staged_through` unchanged
 * (so a retry of the SAME sector index is exactly what resumes) and clears
 * `verified` (any new data invalidates a prior verify).
 */
st_xfer_result_t st_xfer_stage_sector(st_xfer_txn_t *t, uint32_t sector_index,
				       const uint8_t data[ST_SECTOR_BYTES], uint32_t sector_crc32,
				       st_sector_write_fn write_fn, void *ctx);

/*
 * K: verify. Requires `staged_through == total_sectors` (every sector
 * staged). Reads every staged sector back through `read_fn`, checks the
 * running whole-payload CRC-32 against `expected_crc32` from B — a real
 * read-back of what is actually on the media, not just what was in RAM
 * before the write — AND decodes every sector (st_sector_decode(), the real
 * documented SP-1 codec) to independently accumulate each stem's CRC-32
 * over exactly its declared `stem_content_frames[i]`, checked against
 * `meta.stem_crc32[i]`. Only when the whole-payload CRC and all four
 * per-stem CRCs match does this set `verified = true`; ANY read failure or
 * mismatch leaves it false and the transaction stays open (nothing is
 * committed either way). Uses a bounded, non-stack (static) sector work
 * buffer — see st_transfer.c — never an 8192-byte automatic/stack buffer.
 */
st_xfer_result_t st_xfer_verify(st_xfer_txn_t *t, st_sector_read_fn read_fn, void *ctx);

/*
 * C precheck: the transactional GATE. Returns ST_XFER_OK only if a
 * transaction is open AND verified. The caller (Zephyr glue) performs the
 * actual eMMC flush and library-header slot commit ONLY after this
 * returns OK — "commit is refused unless verify was run and passed" holds
 * by construction, since there is no other path to a successful commit.
 * On success, clears the transaction (open = false) — matching "the
 * runtime frame is cleared" pattern used by led_frame_release().
 */
st_xfer_result_t st_xfer_commit_precheck(st_xfer_txn_t *t);

/* A: abort. Always succeeds; the target slot's previously committed song
 * (if any) was never touched by anything above, since every write above
 * only ever targets the staging region. */
void st_xfer_abort(st_xfer_txn_t *t);

/* D / I: destructive-confirmation token check. `token` must be exactly
 * ST_DESTRUCTIVE_CONFIRM_LEN bytes and match ST_DESTRUCTIVE_CONFIRM_TOKEN;
 * anything else (including a short/missing token) fails closed. */
bool st_xfer_check_token(const uint8_t *token, uint32_t len);

/*
 * Fills `meta` with the committed slot record for a just-verified
 * transaction. Performance-state fields (mixer/mute/solo/FX/scrub) are
 * reset to firmware defaults for a NEW upload — never silently carried
 * over from whatever slot index happened to be reused previously. Returns
 * false (leaves `meta` unmodified) if the transaction is not both open and
 * verified.
 */
bool st_xfer_build_slot_meta(const st_xfer_txn_t *t, uint32_t start_sector,
			      st_slot_meta_t *meta);

#endif /* STEMTAPE_PLAYER_TRANSFER_H_ */
