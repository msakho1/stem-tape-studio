/*
 * st_stix.h — STIX v2 index record (docs/stem-tape-transfer-v1.1.md
 * section 3, 4, 12.6, 12.7): parse/serialize, the record's own integrity
 * CRC-32, structural validation, and the generation-based active-index
 * selector shared identically by every caller (device, upload preflight,
 * post-commit confirmation, reconnect recovery -- docs section 4's own
 * requirement: "One selector, used identically... The device's advisory
 * activeIndexSlot is never trusted.").
 *
 * Verified byte-for-byte against the frozen handoff/v1.1/binaries/ index
 * fixtures (see tests/test_stem_v11.c): index-a-valid.bin (slotIdentity=A,
 * songSlot=B, generation=3, committed), index-b-valid.bin (slotIdentity=B,
 * songSlot=A, generation=2, committed), index-uncommitted.bin (same
 * content as index-a-valid.bin except magic=0 -- the step-13 "written but
 * not yet committed" image from the 22-step replacement sequence),
 * storage-initialized-empty.bin (index A valid/no-song generation 1,
 * index B all-zero -- selection must land on A).
 *
 * PURE: no I/O, no Zephyr, no dynamic allocation. Sector/block I/O and
 * device region geometry are always caller-supplied, matching st_transfer.h's
 * injected-function convention -- this module never assumes a specific
 * device's real A/B region placement.
 */

#ifndef STEMTAPE_PLAYER_STIX_H_
#define STEMTAPE_PLAYER_STIX_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_v11_format.h"

typedef struct {
	uint32_t magic; /* 0 = uncommitted, ST11_INDEX_MAGIC = committed */
	uint16_t index_version;
	uint16_t format_major;
	uint16_t format_minor;
	uint8_t slot_identity; /* ST11_SLOT_A / ST11_SLOT_B: which region this record was read from */
	uint8_t song_slot;     /* ST11_SLOT_A / ST11_SLOT_B: which song region it describes */
	uint16_t flags;        /* ST11_IX_FLAG_SONG_PRESENT */
	uint32_t generation_lo;
	uint32_t generation_hi;
	uint32_t song_start_block;
	uint32_t song_block_count;
	uint32_t frames;
	uint32_t sector_count;
	uint32_t sample_rate;
	uint16_t channels;
	uint16_t bit_depth;
	uint32_t bpm_q8;
	uint32_t downbeat_frame;
	uint32_t original_frames[ST11_STEM_COUNT];
	uint32_t stem_checksums[ST11_STEM_COUNT]; /* FNV-1a, st_checksum32.h -- NOT CRC-32 */
	uint32_t song_checksum;                   /* FNV-1a over the 16-byte stem-checksum digest */
	char title[ST11_INDEX_TEXT_BYTES];        /* NUL-padded UTF-8; not guaranteed NUL-terminated
						    * if all 60 bytes are used */
	char artist[ST11_INDEX_TEXT_BYTES];
	uint32_t crc32; /* the record's OWN integrity CRC-32 (IEEE 802.3), last field */
} st_stix_record_t;

/* Parses the 256-byte record portion of a 512-byte physical block. Pure
 * field extraction -- performs no validation (see st_stix_validate()). */
void st_stix_deserialize(const uint8_t in[ST11_PHYSICAL_BLOCK_BYTES], st_stix_record_t *out);

/*
 * Serializes `r` into a full ST11_PHYSICAL_BLOCK_BYTES (512) block:
 * bytes [0,252) from the struct fields, `r->crc32` at [252,256), and
 * bytes [256,512) zeroed (the doc's "bytes 256..512 of the index block
 * must be zero" rule). Does NOT compute the CRC -- call
 * st_stix_record_crc_of_fields() first and set r->crc32 yourself (this
 * mirrors the real write sequence: an uncommitted record is serialized
 * with magic=0 and the SAME crc32 as the eventual committed one, since
 * the CRC excludes the magic bytes).
 */
void st_stix_serialize(const st_stix_record_t *r, uint8_t out[ST11_PHYSICAL_BLOCK_BYTES]);

/*
 * The record's own integrity CRC-32: CRC-32 IEEE 802.3 (st_crc32.c) over
 * block[0,252) with block[0,4) (the magic) normalized to zero. Works
 * directly on the raw block bytes (not a deserialized struct) so it can
 * validate a block as read off eMMC without a round-trip, and so the same
 * function computes the value to embed when writing (serialize first with
 * ANY magic, compute the CRC over the resulting bytes, then re-serialize
 * with crc32 set -- or simply build the struct, serialize once, compute,
 * and overwrite bytes [252,256) of the output buffer directly).
 */
uint32_t st_stix_block_crc(const uint8_t block[ST11_PHYSICAL_BLOCK_BYTES]);

typedef enum {
	ST_STIX_VALID = 0,
	ST_STIX_ERR_MAGIC,         /* uncommitted (magic==0) or corrupt magic */
	ST_STIX_ERR_CRC,           /* stored crc32 does not match st_stix_block_crc() */
	ST_STIX_ERR_VERSION,       /* index_version/formatMajor/formatMinor mismatch */
	ST_STIX_ERR_SLOT_IDENTITY, /* slot_identity != the region this block was read from (misaddressed write) */
	ST_STIX_ERR_SONG_METADATA, /* internal inconsistency: songPresent but frames==0 (or vice
				     * versa), sector_count/song_block_count don't match frames,
				     * or sample_rate/channels/bit_depth don't match the fixed
				     * Stem Tape audio format */
	ST_STIX_ERR_BOUNDS,        /* song_slot not in {A,B}, or the song region this record
				     * declares does not fit inside the caller-supplied bounds
				     * for that slot, or song_start_block is not sector-aligned */
} st_stix_validity_t;

/*
 * Full structural validation (docs section 4, step 2: "Reject invalid
 * magic, CRC, version, slot identity, bounds or song metadata"), checked
 * in that fixed order so the caller gets the single most relevant reason.
 * `expected_slot_identity` is which region `block` was physically read
 * from (ST11_SLOT_A or ST11_SLOT_B). `song_a_start/blocks` and
 * `song_b_start/blocks` are the device's real, capability-reported song
 * region geometry -- used only for the bounds check, and only against
 * whichever region the record's own `song_slot` field names.
 *
 * On ST_STIX_VALID, `record_out` holds the fully parsed record. On any
 * other result, `record_out` still holds the parsed fields (so a caller
 * can inspect e.g. the generation of a bounds-invalid record for
 * diagnostics), but the result code says it must NOT be trusted/selected.
 */
st_stix_validity_t st_stix_validate(const uint8_t block[ST11_PHYSICAL_BLOCK_BYTES],
				     uint8_t expected_slot_identity, uint32_t song_a_start,
				     uint32_t song_a_blocks, uint32_t song_b_start,
				     uint32_t song_b_blocks, st_stix_record_t *record_out);

/*
 * Exactly st_stix_validate() MINUS the magic check -- every other rule
 * (CRC, version, slot identity, song metadata, bounds) still applies
 * identically, including to the CRC itself (which always treats the
 * magic bytes as zero regardless of what they actually hold, so this
 * validates an UNCOMMITTED record -- magic == 0, step 13 of the 22-step
 * replacement sequence -- exactly as strictly as a committed one). Used
 * by the write-safety session gate (st_ab_session.h) to validate an
 * in-flight uncommitted index write before it is on disk, when requiring
 * magic == ST11_INDEX_MAGIC would be wrong by construction. Never call
 * this to decide whether a record may be SELECTED as active -- only
 * st_stix_validate() (which does require the real magic) may decide
 * that.
 */
st_stix_validity_t st_stix_validate_fields_only(const uint8_t block[ST11_PHYSICAL_BLOCK_BYTES],
						 uint8_t expected_slot_identity, uint32_t song_a_start,
						 uint32_t song_a_blocks, uint32_t song_b_start,
						 uint32_t song_b_blocks, st_stix_record_t *record_out);

typedef enum {
	ST_STIX_SELECT_A = 0,
	ST_STIX_SELECT_B = 1,
	ST_STIX_SELECT_NONE = 2, /* neither valid: blank or corrupt storage, explicit init required */
} st_stix_select_t;

/*
 * THE ONE selector (docs section 4, steps 1-7): validates both regions
 * (via st_stix_validate(), slot identities ST11_SLOT_A/ST11_SLOT_B fixed),
 * then:
 *   - exactly one valid -> that one
 *   - both valid -> strictly greater 64-bit generation wins; a tie
 *     (impossible in practice since generation is unique-per-commit, but
 *     specified explicitly) resolves to slot A
 *   - neither valid -> ST_STIX_SELECT_NONE
 * `selected_out` is filled with the winning record iff the result is not
 * ST_STIX_SELECT_NONE. Never trusts any device-advisory "active slot"
 * hint -- there is no such parameter here by design.
 */
st_stix_select_t st_stix_select_active(const uint8_t block_a[ST11_PHYSICAL_BLOCK_BYTES],
					const uint8_t block_b[ST11_PHYSICAL_BLOCK_BYTES],
					uint32_t song_a_start, uint32_t song_a_blocks,
					uint32_t song_b_start, uint32_t song_b_blocks,
					st_stix_record_t *selected_out);

typedef enum {
	ST_STIX_LIB_OK = 0,
	ST_STIX_LIB_BLANK,   /* both index blocks are all-zero: never initialized */
	ST_STIX_LIB_CORRUPT, /* both invalid, but not blank */
} st_stix_lib_status_t;

/*
 * The full library-selection result a real replacement sequence needs --
 * st_stix_select_active() plus the two DESTINATION slots for the next
 * replacement. Mirrors the companion's own LibraryState exactly (see
 * src/sp1/activeIndex.ts's selectActiveIndex(), not committed to this
 * repo but consulted for this exact rule -- verified against real
 * fixtures below):
 *
 *   inactive_index_slot = complement(active record's slot_identity),
 *     ALWAYS (regardless of whether a song is present).
 *   inactive_song_slot  = complement(active record's song_slot) IF that
 *     record has SONG_PRESENT set; otherwise (no song ever committed
 *     through this record) inactive_song_slot = the record's song_slot
 *     field AS-IS, UNCOMPLEMENTED -- so a fresh explicit-init record
 *     (generation 1, no song, song_slot=A by convention) sends the very
 *     first real upload to song slot A, matching
 *     docs/stem-tape-transfer-v1.1.md section 5's own worked example
 *     ("Uploads therefore alternate: song A/index B, song B/index A, ...").
 *
 * When status != ST_STIX_LIB_OK (blank or corrupt), both inactive slots
 * default to ST11_SLOT_A (matching the companion's own fallback), and
 * `active`/generation/active_index_slot/active_song_slot are all
 * "none" (ST11_NO_SLOT / 0) -- a caller must still refuse to write
 * (requires_initialization is set) even though the slots default in a
 * well-defined way.
 */
typedef struct {
	st_stix_lib_status_t status;
	bool requires_initialization;
	uint32_t active_index_slot; /* ST11_SLOT_A/B, or ST11_NO_SLOT if status != OK */
	uint32_t active_song_slot;  /* ST11_SLOT_A/B, or ST11_NO_SLOT if no song present/status != OK */
	uint64_t generation;        /* 0 if status != OK */
	uint32_t inactive_index_slot; /* always ST11_SLOT_A or ST11_SLOT_B */
	uint32_t inactive_song_slot;  /* always ST11_SLOT_A or ST11_SLOT_B */
	st_stix_record_t active;      /* valid iff status == ST_STIX_LIB_OK */
} st_stix_library_state_t;

void st_stix_read_library(const uint8_t block_a[ST11_PHYSICAL_BLOCK_BYTES],
			   const uint8_t block_b[ST11_PHYSICAL_BLOCK_BYTES], uint32_t song_a_start,
			   uint32_t song_a_blocks, uint32_t song_b_start, uint32_t song_b_blocks,
			   st_stix_library_state_t *out);

#endif /* STEMTAPE_PLAYER_STIX_H_ */
