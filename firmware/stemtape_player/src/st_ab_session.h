/*
 * st_ab_session.h — the v1.1 write-safety SESSION gate.
 *
 * st11_region_of_block() (st_stcp.h) only proves an address falls inside
 * ONE OF THE FOUR permitted v1.1 regions -- it says nothing about which
 * pair is the CURRENT active (protected) one and which is the frozen
 * inactive (writable) destination for a specific in-flight replacement.
 * That is not a stateless, per-call fact: it depends on a snapshot of the
 * library taken once at the start of a replacement (or explicit-init)
 * attempt and held fixed for its whole duration, exactly as docs section 5
 * step 1 requires ("Re-query Q immediately before writing... or nothing is
 * written") and step 7 requires ("The active song and index are never
 * touched from here on").
 *
 * This module owns that snapshot. A caller (main.c's real 'W' handler,
 * once wired) must:
 *   1. Open exactly one session (st_ab_session_open_replace() for a normal
 *      upload, st_ab_session_open_init() ONLY for an explicit,
 *      already-token-confirmed initialization) before any write.
 *   2. Call st_ab_session_check_write() for EVERY block before writing it
 *      -- never write first and check after.
 *   3. For a REPLACE session, actually perform
 *      st_ab_session_verify_song_before_commit() (real I/O: reads the
 *      frozen song region back and recomputes its checksums) and call
 *      st_ab_session_mark_song_verified() BEFORE attempting the magic-
 *      committing write -- the gate itself refuses that write otherwise;
 *      this firmware never takes the companion's read-back claims on
 *      faith.
 *   4. Once st_ab_session_check_write() accepts a magic-committing write,
 *      the session becomes single-use (st_ab_session_close() or the gate's
 *      own internal latch) -- every FURTHER write, including to the just-
 *      superseded former-active pair (now the rollback copy), is refused
 *      until a NEW session is opened, which re-reads the (now-updated)
 *      library fresh rather than reusing any stale state.
 *
 * PURE except st_ab_session_verify_song_before_commit(), which needs
 * caller-injected block-read I/O (matching st_transfer.h's convention) to
 * do its job; every other function here is pure computation over
 * caller-supplied bytes.
 */

#ifndef STEMTAPE_PLAYER_AB_SESSION_H_
#define STEMTAPE_PLAYER_AB_SESSION_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_stcp.h"
#include "st_stix.h"
#include "st_v11_format.h"

typedef enum {
	ST_AB_SESSION_NONE = 0,
	ST_AB_SESSION_REPLACE, /* normal upload: replaces the currently active song+index */
	ST_AB_SESSION_INIT,    /* explicit initialization: only legal when both indexes are invalid/blank */
} st_ab_session_kind_t;

typedef struct {
	bool open;
	bool closed;         /* single-use latch: set once a magic-committing write is accepted */
	bool song_verified;  /* REPLACE only: set only via st_ab_session_mark_song_verified() */
	st_ab_session_kind_t kind;
	st11_region_layout_t layout; /* frozen at open time */
	uint32_t active_song_slot;   /* ST11_NO_SLOT if none */
	uint32_t active_index_slot;  /* ST11_NO_SLOT if none */
	uint32_t inactive_song_slot; /* the ONLY song slot this session may ever write */
	uint32_t inactive_index_slot; /* the ONLY index slot this session may ever write */
	uint64_t active_generation;   /* generation at open time; a commit record must declare
					* exactly active_generation+1 (or exactly 1 for INIT) */
	uint32_t needed_song_blocks;  /* REPLACE only: the exact block count THIS song needs --
					* tighter than the whole frozen region's capacity */
} st_ab_session_t;

typedef enum {
	ST_AB_OPEN_OK = 0,
	ST_AB_OPEN_ERR_NOT_INITIALIZED,     /* REPLACE: the library requires initialization --
					      * there is no valid active record to replace */
	ST_AB_OPEN_ERR_ALREADY_INITIALIZED, /* INIT: the library does NOT require initialization --
					      * refuses to clobber a valid library; docs section 7:
					      * "legal only when both index records are invalid
					      * or blank" */
	ST_AB_OPEN_ERR_NOT_CONFIRMED,       /* INIT: caller did not pass an explicit confirmation --
					      * this function never infers consent */
	ST_AB_OPEN_ERR_CAPACITY,            /* REPLACE: the song does not fit the frozen inactive
					      * song region (docs section 5 step 6,
					      * InsufficientStagingCapacityError) */
} st_ab_open_result_t;

/*
 * Opens a REPLACE session: reads both index blocks fresh (st_stix_read_library()),
 * freezes the resulting inactive_song_slot, inactive_index_slot, active
 * slots, and generation,
 * and checks `needed_song_blocks` fits the frozen song region's real capacity.
 * Refuses (session left closed/unusable) if the library requires
 * initialization or the song does not fit.
 */
st_ab_open_result_t st_ab_session_open_replace(st_ab_session_t *s,
						const uint8_t block_a[ST11_PHYSICAL_BLOCK_BYTES],
						const uint8_t block_b[ST11_PHYSICAL_BLOCK_BYTES],
						const st11_region_layout_t *layout,
						uint32_t needed_song_blocks);

/*
 * Opens an INIT session: legal ONLY when st_stix_read_library() reports
 * requires_initialization (both index blocks invalid or blank) AND
 * `confirmed` is true -- `confirmed` must come from the caller's own
 * verification of an explicit destructive-confirmation token (mirroring
 * st_transfer.h's st_xfer_check_token()), never inferred here. An INIT
 * session's only two writable blocks are index B (docs section 7: written
 * as explicit zeros) and index A (the new generation-1, song-free
 * record); no song region is ever writable in an INIT session.
 */
st_ab_open_result_t st_ab_session_open_init(st_ab_session_t *s,
					     const uint8_t block_a[ST11_PHYSICAL_BLOCK_BYTES],
					     const uint8_t block_b[ST11_PHYSICAL_BLOCK_BYTES],
					     const st11_region_layout_t *layout, bool confirmed);

typedef enum {
	ST_AB_WRITE_OK = 0,
	ST_AB_WRITE_ERR_NO_SESSION,          /* no session open (or it was never opened successfully) */
	ST_AB_WRITE_ERR_SESSION_CLOSED,      /* this session already committed -- single-use */
	ST_AB_WRITE_ERR_ACTIVE_REGION,       /* block is inside the CURRENT active song or index region */
	ST_AB_WRITE_ERR_OUTSIDE_FROZEN_PAIR, /* block is a v1.1 region address, or unrelated storage,
					       * but not this session's frozen destination */
	ST_AB_WRITE_ERR_BAD_COMMIT_RECORD,   /* the block IS the frozen index destination, `data`
					       * carries the validity magic, but the record fails
					       * CRC/version/slot-identity/geometry/bounds, or its
					       * song reference does not name THIS session's own
					       * frozen song slot/start/size */
	ST_AB_WRITE_ERR_WRONG_GENERATION,    /* the commit record's generation != active_generation+1
					       * (INIT: != 1) */
	ST_AB_WRITE_ERR_SONG_NOT_VERIFIED,   /* REPLACE: a magic-committing write was attempted before
					       * st_ab_session_mark_song_verified() was ever called --
					       * this firmware never activates a record on the
					       * companion's word alone */
} st_ab_write_check_t;

/*
 * THE per-block gate. Must be called, and must return ST_AB_WRITE_OK,
 * before `data` is ever actually written to `block`. `data` is examined
 * only when `block` is this session's frozen index destination -- for any
 * other accepted block (a song-region write) the bytes are opaque audio
 * payload and are not interpreted here at all.
 */
st_ab_write_check_t st_ab_session_check_write(st_ab_session_t *s, uint32_t block,
					       const uint8_t data[ST11_PHYSICAL_BLOCK_BYTES]);

typedef int (*st11_block_read_fn)(uint32_t block, uint8_t out[ST11_PHYSICAL_BLOCK_BYTES], void *ctx);

/*
 * REPLACE only. Reads back every block of the session's frozen song
 * region via `read_fn` (real I/O, injected -- matches st_transfer.h's
 * convention so this is host-testable against an in-memory mock),
 * decodes it with the real STSC codec (st_sector_v11.h), and recomputes
 * every stem's FNV-1a checksum plus the song checksum EXACTLY as
 * test_stem_v11.c's own proven song-fixture logic does. Returns true
 * only if all five recomputed checksums (4 stems + song) match
 * `candidate`'s declared values -- this firmware verifies the bytes
 * actually on the media, never trusting the companion's own claim that
 * its upload succeeded. `scratch_sector` is a caller-owned
 * ST11_SECTOR_BYTES buffer (the one physical sector-sized buffer a
 * commit session needs, not allocated here). Does not open, close, or
 * otherwise mutate `s`; call st_ab_session_mark_song_verified() yourself
 * once this returns true.
 */
bool st_ab_session_verify_song_before_commit(const st_ab_session_t *s, const st_stix_record_t *candidate,
					      st11_block_read_fn read_fn, void *ctx,
					      uint8_t scratch_sector[ST11_SECTOR_BYTES]);

/*
 * Records that st_ab_session_verify_song_before_commit() was actually run
 * and returned true. This function does not itself re-verify anything --
 * it is the caller's explicit attestation, exactly like
 * st_xfer_commit_precheck()'s existing `verified` flag in st_transfer.h.
 * Calling it without having genuinely verified first defeats the
 * guarantee this module exists to provide; nothing else in this API
 * calls it for you.
 */
void st_ab_session_mark_song_verified(st_ab_session_t *s);

/* Explicitly ends a session (idempotent). Every subsequent
 * st_ab_session_check_write() call on it returns ST_AB_WRITE_ERR_SESSION_CLOSED.
 * st_ab_session_check_write() also calls this internally the moment it
 * accepts a magic-committing write, so an explicit call here is only
 * needed to abandon a session early (e.g. on error/abort). */
void st_ab_session_close(st_ab_session_t *s);

#endif /* STEMTAPE_PLAYER_AB_SESSION_H_ */
