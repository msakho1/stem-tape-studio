/*
 * st_ab_session.c — see st_ab_session.h.
 */

#include "st_ab_session.h"

#include <string.h>

#include "st_checksum32.h"
#include "st_sector_v11.h"

static uint32_t get_u32le(const uint8_t *in, uint32_t off)
{
	return (uint32_t)in[off + 0] | ((uint32_t)in[off + 1] << 8) |
	       ((uint32_t)in[off + 2] << 16) | ((uint32_t)in[off + 3] << 24);
}

static uint64_t generation64(const st_stix_record_t *r)
{
	return ((uint64_t)r->generation_hi << 32) | (uint64_t)r->generation_lo;
}

static uint32_t region_start_of_slot(const st11_region_layout_t *layout, uint32_t slot)
{
	return (slot == ST11_SLOT_A) ? layout->song_a_start : layout->song_b_start;
}

static uint32_t region_blocks_of_slot(const st11_region_layout_t *layout, uint32_t slot)
{
	return (slot == ST11_SLOT_A) ? layout->song_a_blocks : layout->song_b_blocks;
}

st_ab_open_result_t st_ab_session_open_replace(st_ab_session_t *s,
						const uint8_t block_a[ST11_PHYSICAL_BLOCK_BYTES],
						const uint8_t block_b[ST11_PHYSICAL_BLOCK_BYTES],
						const st11_region_layout_t *layout,
						uint32_t needed_song_blocks)
{
	memset(s, 0, sizeof(*s));

	st_stix_library_state_t lib;

	st_stix_read_library(block_a, block_b, layout->song_a_start, layout->song_a_blocks, layout->song_b_start,
			      layout->song_b_blocks, &lib);

	if (lib.requires_initialization) {
		return ST_AB_OPEN_ERR_NOT_INITIALIZED;
	}

	uint32_t capacity = region_blocks_of_slot(layout, lib.inactive_song_slot);

	if (needed_song_blocks > capacity) {
		return ST_AB_OPEN_ERR_CAPACITY;
	}

	s->open = true;
	s->closed = false;
	s->song_verified = false;
	s->kind = ST_AB_SESSION_REPLACE;
	s->layout = *layout;
	s->active_song_slot = lib.active_song_slot;
	s->active_index_slot = lib.active_index_slot;
	s->inactive_song_slot = lib.inactive_song_slot;
	s->inactive_index_slot = lib.inactive_index_slot;
	s->active_generation = lib.generation;
	s->needed_song_blocks = needed_song_blocks;
	return ST_AB_OPEN_OK;
}

st_ab_open_result_t st_ab_session_open_init(st_ab_session_t *s, const uint8_t block_a[ST11_PHYSICAL_BLOCK_BYTES],
					     const uint8_t block_b[ST11_PHYSICAL_BLOCK_BYTES],
					     const st11_region_layout_t *layout, bool confirmed)
{
	memset(s, 0, sizeof(*s));

	st_stix_library_state_t lib;

	st_stix_read_library(block_a, block_b, layout->song_a_start, layout->song_a_blocks, layout->song_b_start,
			      layout->song_b_blocks, &lib);

	if (!lib.requires_initialization) {
		return ST_AB_OPEN_ERR_ALREADY_INITIALIZED;
	}
	if (!confirmed) {
		return ST_AB_OPEN_ERR_NOT_CONFIRMED;
	}

	s->open = true;
	s->closed = false;
	s->song_verified = false;
	s->kind = ST_AB_SESSION_INIT;
	s->layout = *layout;
	s->active_song_slot = ST11_NO_SLOT;
	s->active_index_slot = ST11_NO_SLOT;
	s->inactive_song_slot = lib.inactive_song_slot;   /* ST11_SLOT_A: where the real record goes */
	s->inactive_index_slot = lib.inactive_index_slot; /* ST11_SLOT_A */
	s->active_generation = 0;
	s->needed_song_blocks = 0;
	return ST_AB_OPEN_OK;
}

void st_ab_session_close(st_ab_session_t *s)
{
	s->closed = true;
}

static bool block_all_zero(const uint8_t data[ST11_PHYSICAL_BLOCK_BYTES])
{
	uint32_t i;

	for (i = 0; i < ST11_PHYSICAL_BLOCK_BYTES; i++) {
		if (data[i] != 0) {
			return false;
		}
	}
	return true;
}

/* Every content rule a candidate commit/draft record must satisfy beyond
 * st_stix_validate_fields_only()'s generic checks: it must name exactly
 * THIS session's own frozen destination, not merely "a" valid region. */
static bool candidate_matches_session(const st_ab_session_t *s, const st_stix_record_t *cand)
{
	bool present = (cand->flags & ST11_IX_FLAG_SONG_PRESENT) != 0u;

	if (cand->song_slot != s->inactive_song_slot) {
		return false;
	}
	if (s->kind == ST_AB_SESSION_INIT) {
		return !present; /* an init record must never claim a song */
	}
	/* REPLACE: must claim a song, at exactly this session's frozen
	 * start block and exactly the block count THIS song needs (not
	 * merely "fits somewhere in the region"). */
	if (!present) {
		return false;
	}
	uint32_t region_start = region_start_of_slot(&s->layout, s->inactive_song_slot);

	return cand->song_start_block == region_start && cand->song_block_count == s->needed_song_blocks;
}

st_ab_write_check_t st_ab_session_check_write(st_ab_session_t *s, uint32_t block,
					       const uint8_t data[ST11_PHYSICAL_BLOCK_BYTES])
{
	if (!s->open) {
		return ST_AB_WRITE_ERR_NO_SESSION;
	}
	if (s->closed) {
		return ST_AB_WRITE_ERR_SESSION_CLOSED;
	}

	st11_region_id_t region = st11_region_of_block(&s->layout, block);

	bool is_active_song = (s->active_song_slot == ST11_SLOT_A && region == ST11_REGION_SONG_A) ||
			       (s->active_song_slot == ST11_SLOT_B && region == ST11_REGION_SONG_B);
	bool is_active_index = (s->active_index_slot == ST11_SLOT_A && region == ST11_REGION_INDEX_A) ||
				(s->active_index_slot == ST11_SLOT_B && region == ST11_REGION_INDEX_B);

	if (is_active_song || is_active_index) {
		return ST_AB_WRITE_ERR_ACTIVE_REGION;
	}

	bool is_frozen_song = (s->kind == ST_AB_SESSION_REPLACE) &&
			       ((s->inactive_song_slot == ST11_SLOT_A && region == ST11_REGION_SONG_A) ||
				(s->inactive_song_slot == ST11_SLOT_B && region == ST11_REGION_SONG_B));

	if (is_frozen_song) {
		uint32_t region_start = region_start_of_slot(&s->layout, s->inactive_song_slot);

		if (block >= region_start + s->needed_song_blocks) {
			/* inside the region's total capacity, but beyond what THIS song needs */
			return ST_AB_WRITE_ERR_OUTSIDE_FROZEN_PAIR;
		}
		return ST_AB_WRITE_OK; /* opaque audio payload -- not interpreted here */
	}

	bool is_frozen_index = (s->inactive_index_slot == ST11_SLOT_A && region == ST11_REGION_INDEX_A) ||
				(s->inactive_index_slot == ST11_SLOT_B && region == ST11_REGION_INDEX_B);

	if (is_frozen_index) {
		uint32_t magic = get_u32le(data, ST11_IX_OFF_MAGIC);
		st_stix_record_t candidate;
		st_stix_validity_t v = st_stix_validate_fields_only(
			data, (uint8_t)s->inactive_index_slot, s->layout.song_a_start, s->layout.song_a_blocks,
			s->layout.song_b_start, s->layout.song_b_blocks, &candidate);

		if (v != ST_STIX_VALID || !candidate_matches_session(s, &candidate)) {
			return ST_AB_WRITE_ERR_BAD_COMMIT_RECORD;
		}

		uint64_t expected_generation = (s->kind == ST_AB_SESSION_INIT) ? 1u : s->active_generation + 1u;

		if (generation64(&candidate) != expected_generation) {
			return ST_AB_WRITE_ERR_WRONG_GENERATION;
		}

		if (magic == 0u) {
			return ST_AB_WRITE_OK; /* well-formed uncommitted draft */
		}
		if (magic != ST11_INDEX_MAGIC) {
			return ST_AB_WRITE_ERR_BAD_COMMIT_RECORD; /* garbage magic, neither 0 nor real */
		}
		/* The real commit write. */
		if (s->kind == ST_AB_SESSION_REPLACE && !s->song_verified) {
			return ST_AB_WRITE_ERR_SONG_NOT_VERIFIED;
		}
		s->closed = true; /* single-use: accepted exactly once, never reused for the
				    * now-superseded former-active pair either */
		return ST_AB_WRITE_OK;
	}

	if (s->kind == ST_AB_SESSION_INIT) {
		bool is_zero_slot = (s->inactive_index_slot == ST11_SLOT_A && region == ST11_REGION_INDEX_B) ||
				     (s->inactive_index_slot == ST11_SLOT_B && region == ST11_REGION_INDEX_A);

		if (is_zero_slot) {
			/* docs section 7: "writes index B as explicit zeros" -- not a STIX
			 * record at all, just a hygiene clear of the other slot. */
			return block_all_zero(data) ? ST_AB_WRITE_OK : ST_AB_WRITE_ERR_BAD_COMMIT_RECORD;
		}
	}

	return ST_AB_WRITE_ERR_OUTSIDE_FROZEN_PAIR;
}

bool st_ab_session_verify_song_before_commit(const st_ab_session_t *s, const st_stix_record_t *candidate,
					      st11_block_read_fn read_fn, void *ctx,
					      uint8_t scratch_sector[ST11_SECTOR_BYTES])
{
	if (s->kind != ST_AB_SESSION_REPLACE) {
		return false;
	}

	uint32_t region_start = region_start_of_slot(&s->layout, s->inactive_song_slot);
	uint32_t stem_hash[ST11_STEM_COUNT];
	uint32_t si;

	for (si = 0; si < ST11_STEM_COUNT; si++) {
		stem_hash[si] = ST_CHECKSUM32_INIT;
	}

	uint32_t sector;

	for (sector = 0; sector < candidate->sector_count; sector++) {
		uint32_t k;

		for (k = 0; k < ST11_BLOCKS_PER_SECTOR; k++) {
			uint32_t blk = region_start + sector * ST11_BLOCKS_PER_SECTOR + k;

			if (read_fn(blk, scratch_sector + (size_t)k * ST11_PHYSICAL_BLOCK_BYTES, ctx) != 0) {
				return false;
			}
		}

		st11_sector_header_t h;

		if (!st11_sector_read_header(scratch_sector, &h) || h.sector_index != sector) {
			return false;
		}

		uint32_t f;

		for (f = 0; f < h.frame_count; f++) {
			st11_audio_frame_t frame;

			st11_sector_decode_frame(scratch_sector, f, &frame);
			for (si = 0; si < ST11_STEM_COUNT; si++) {
				uint8_t sample_bytes[6];
				int32_t l = frame.stem_l[si];
				int32_t r = frame.stem_r[si];

				sample_bytes[0] = (uint8_t)(l & 0xff);
				sample_bytes[1] = (uint8_t)((l >> 8) & 0xff);
				sample_bytes[2] = (uint8_t)((l >> 16) & 0xff);
				sample_bytes[3] = (uint8_t)(r & 0xff);
				sample_bytes[4] = (uint8_t)((r >> 8) & 0xff);
				sample_bytes[5] = (uint8_t)((r >> 16) & 0xff);
				stem_hash[si] = st_checksum32_update(stem_hash[si], sample_bytes, sizeof(sample_bytes));
			}
		}
	}

	for (si = 0; si < ST11_STEM_COUNT; si++) {
		if (stem_hash[si] != candidate->stem_checksums[si]) {
			return false;
		}
	}

	uint8_t digest[ST11_STEM_COUNT * 4];

	for (si = 0; si < ST11_STEM_COUNT; si++) {
		digest[si * 4 + 0] = (uint8_t)(stem_hash[si] & 0xff);
		digest[si * 4 + 1] = (uint8_t)((stem_hash[si] >> 8) & 0xff);
		digest[si * 4 + 2] = (uint8_t)((stem_hash[si] >> 16) & 0xff);
		digest[si * 4 + 3] = (uint8_t)((stem_hash[si] >> 24) & 0xff);
	}

	uint32_t song_hash = st_checksum32_compute(digest, sizeof(digest));

	return song_hash == candidate->song_checksum;
}

void st_ab_session_mark_song_verified(st_ab_session_t *s)
{
	s->song_verified = true;
}
