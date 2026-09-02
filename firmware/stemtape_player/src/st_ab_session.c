/*
 * st_ab_session.c — see st_ab_session.h.
 */

#include "st_ab_session.h"

#include <string.h>

#include "st_checksum32.h"
#include "st_planar.h"
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

/* Defined below; used by st_ab_session_verify_song_before_commit() above them
 * so both verification paths share one derivation. */
static bool stem_hashes_match_candidate(const uint32_t stem_hash[ST11_STEM_COUNT],
					 const st_stix_record_t *candidate);
static bool accumulate_one_sector_v11(uint32_t stem_hash[ST11_STEM_COUNT], uint32_t sector_index,
				       const uint8_t sector[ST11_SECTOR_BYTES]);
static bool accumulate_one_sector_planar(uint32_t stem_hash[ST11_STEM_COUNT],
					  const uint8_t sector[ST11_SECTOR_BYTES],
					  uint32_t *next_stem, uint32_t *next_group,
					  uint32_t *groups_per_stem);
static bool finish_stem_hashes(uint32_t stem_hash[ST11_STEM_COUNT], uint32_t groups,
				uint32_t frames);

/* The incremental accumulator's real starting state. Both open_*() paths
 * memset the session to zero first, but zero is NOT a valid FNV-1a seed --
 * the running hashes must start at ST_CHECKSUM32_INIT exactly as the full
 * re-read path seeds its own locals, or an accumulated hash could never
 * equal a re-read one. acc_valid likewise starts true and only ever goes
 * false. */
static void accumulator_reset(st_ab_session_t *s)
{
	uint32_t si;

	s->acc_valid = true;
	s->acc_sectors = 0;
	s->acc_next_stem = 0;
	s->acc_next_group = 0;
	s->acc_groups_per_stem = 0;
	for (si = 0; si < ST11_STEM_COUNT; si++) {
		s->acc_stem_hash[si] = ST_CHECKSUM32_INIT;
	}
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
	accumulator_reset(s);
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
	accumulator_reset(s);
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
	/* REPLACE: must claim a song, starting at exactly this session's
	 * frozen region start, using no more than needed_song_blocks (a
	 * CEILING -- the real wire protocol (docs section 1) has no verb to
	 * declare a song's size before writes begin, so a caller can only
	 * ever pass the frozen region's own capacity here, not the specific
	 * song's exact size; st_stix_validate_fields_only()'s own bounds
	 * check already rejects anything that doesn't fit or isn't sector-
	 * aligned, so this is not the only guard against an oversized claim). */
	if (!present) {
		return false;
	}
	uint32_t region_start = region_start_of_slot(&s->layout, s->inactive_song_slot);

	return cand->song_start_block == region_start && cand->song_block_count <= s->needed_song_blocks;
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

	/*
	 * THE LAYOUT IS THE ONE THE RECORD DECLARES. See
	 * accumulate_one_sector_v11()'s own comment for why this layer reads
	 * the format version rather than enforcing a policy about it: an
	 * unrecognised version is refused here, but a recognised one is
	 * verified in its own layout, which is what keeps this function a
	 * total check on "do these bytes hash to this record".
	 */
	bool planar;

	if (candidate->format_major != ST11_FORMAT_MAJOR) {
		return false;
	} else if (candidate->format_minor == 3u || candidate->format_minor == 2u) {
		/* v1.2 and v1.3 are both SONG-PLANAR. They differ only in the
		 * stored sample width, and the accumulator does not care: it
		 * hashes whatever st11_sector_decode_frame() returns, at
		 * whatever width this build was compiled for. What it cannot
		 * do is hash a record whose LAYOUT it does not know, which is
		 * the distinction this dispatch exists to draw. */
		planar = true;
	} else if (candidate->format_minor == 1u) {
		planar = false;
	} else {
		return false;
	}

	uint32_t region_start = region_start_of_slot(&s->layout, s->inactive_song_slot);
	uint32_t stem_hash[ST11_STEM_COUNT];
	uint32_t si;

	for (si = 0; si < ST11_STEM_COUNT; si++) {
		stem_hash[si] = ST_CHECKSUM32_INIT;
	}

	/* Deliberately NOT seeded from candidate->sector_count. This path could
	 * start the scan already knowing the geometry, and then it would be
	 * checking the media against a number it had assumed rather than one it
	 * had read. It learns the group count the same way the incremental path
	 * has to, and the two are compared afterwards. */
	uint32_t next_stem = 0;
	uint32_t next_group = 0;
	uint32_t groups_per_stem = 0;
	uint32_t sector;

	for (sector = 0; sector < candidate->sector_count; sector++) {
		uint32_t k;

		for (k = 0; k < ST11_BLOCKS_PER_SECTOR; k++) {
			uint32_t blk = region_start + sector * ST11_BLOCKS_PER_SECTOR + k;

			if (read_fn(blk, scratch_sector + (size_t)k * ST11_PHYSICAL_BLOCK_BYTES, ctx) != 0) {
				return false;
			}
		}

		/* Same per-sector step the incremental path uses, so a fully
		 * re-read hash and an accumulated one are equal by
		 * construction rather than by two copies of this logic
		 * happening to agree. */
		bool ok = planar ? accumulate_one_sector_planar(stem_hash, scratch_sector, &next_stem,
								 &next_group, &groups_per_stem)
				 : accumulate_one_sector_v11(stem_hash, sector, scratch_sector);

		if (!ok) {
			return false;
		}
	}

	if (planar) {
		/* The scan walked stem 0 group 0 through stem 3's last group,
		 * so a complete song leaves it one past the end of the LAST
		 * stem, and the group count it learned along the way has to be
		 * the one the record declares. A region holding the right
		 * NUMBER of well-formed groups arranged as some other geometry
		 * fails here. */
		if (next_stem != ST_PL_STEMS - 1u || next_group != candidate->sector_count ||
		    groups_per_stem != candidate->sector_count) {
			return false;
		}
		if (!finish_stem_hashes(stem_hash, candidate->sector_count, candidate->frames)) {
			return false;
		}
	}

	return stem_hashes_match_candidate(stem_hash, candidate);
}

void st_ab_session_mark_song_verified(st_ab_session_t *s)
{
	s->song_verified = true;
}

/* The ONE derivation of "these four stem hashes imply this song checksum",
 * shared by both verification paths so they can never drift apart. */
static bool stem_hashes_match_candidate(const uint32_t stem_hash[ST11_STEM_COUNT],
					 const st_stix_record_t *candidate)
{
	uint32_t si;

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

	return st_checksum32_compute(digest, sizeof(digest)) == candidate->song_checksum;
}

/*
 * v1.1 INTERLEAVED, the retired layout: one sector is 340 frames of all four
 * stems, and its STSC header declares how many of those frames are real.
 *
 * KEPT, DELIBERATELY, and reached only through the format_minor dispatch in
 * st_ab_session_verify_song_before_commit(). This module is the STORAGE-SAFETY
 * layer: its whole job is to refuse any commit whose media bytes do not hash
 * to the record being committed. A record carries its own format version, so
 * verifying it in the layout it declares is what makes that check total rather
 * than accidentally version-coupled.
 *
 * Version POLICY -- "this device will not accept or play a v1.1 song" -- is
 * enforced where it already lives and is already tested: the STCP capability
 * exchange refuses a companion whose format version differs, and the boot
 * library check refuses a v1.1 library outright. Making the hash verifier a
 * SECOND, implicit version gate would put one policy in two places, one of
 * them by accident.
 */
static bool accumulate_one_sector_v11(uint32_t stem_hash[ST11_STEM_COUNT], uint32_t sector_index,
				       const uint8_t sector[ST11_SECTOR_BYTES])
{
	st11_sector_header_t h;

	if (!st11_sector_read_header(sector, &h) || h.sector_index != sector_index) {
		return false;
	}

	uint32_t f;

	for (f = 0; f < h.frame_count; f++) {
		st11_audio_frame_t frame;
		uint32_t si;

		st11_sector_decode_frame(sector, f, &frame);
		for (si = 0; si < ST11_STEM_COUNT; si++) {
			/*
			 * THE STEREO FRAME'S WIDTH, NOT A LITERAL SIX.
			 *
			 * This wrote six bytes per stem -- three per sample --
			 * which was ST11_STEM_FRAME_BYTES exactly while samples
			 * were 24-bit. It is the commit-time verification of
			 * every upload, so at v1.3's 4-byte frame it would have
			 * hashed two bytes of nothing per stem per frame and
			 * refused every song the companion sent, with a
			 * checksum mismatch pointing at the transfer rather
			 * than at this loop.
			 *
			 * Derived now, so the next width change moves it or
			 * fails to compile.
			 */
			uint8_t sample_bytes[ST11_STEM_FRAME_BYTES];
			int32_t l = frame.stem_l[si];
			int32_t r = frame.stem_r[si];
			uint32_t byte_i;

			for (byte_i = 0u; byte_i < ST11_BYTES_PER_SAMPLE; byte_i++) {
				sample_bytes[byte_i] =
					(uint8_t)((uint32_t)l >> (8u * byte_i));
				sample_bytes[ST11_BYTES_PER_SAMPLE + byte_i] =
					(uint8_t)((uint32_t)r >> (8u * byte_i));
			}
			stem_hash[si] = st_checksum32_update(stem_hash[si], sample_bytes, sizeof(sample_bytes));
		}
	}

	return true;
}

/*
 * THE ONE PER-SECTOR v1.2 ACCUMULATION STEP, shared by both v1.2 verification
 * paths so an incrementally accumulated hash and a fully re-read one are the
 * same value by construction rather than by two parallel implementations
 * agreeing today and drifting tomorrow.
 *
 * v1.2 SONG-PLANAR. A sector is no longer 340 frames of all four stems; it is
 * four consecutive 2048-byte GROUPS, each belonging to exactly one stem, laid
 * out stem-major across the whole song region:
 *
 *     | stem 0 timeline | stem 1 timeline | stem 2 timeline | stem 3 |
 *
 * so the flat group ordinal 4*sector_index + i walks stem 0's groups in order,
 * then stem 1's, and so on. Each stem's own bytes therefore still arrive in
 * playback order, which is what makes its FNV-1a checksum identical to the one
 * the companion computed over that stem's contiguous PCM -- unchanged from
 * v1.1, and the reason this format migration moves no checksum at all.
 *
 * WHAT THIS VALIDATES, AND WHY IT NEEDS NO GEOMETRY. Every group names the
 * stem and span it belongs to in its own header. The scan below requires them
 * to arrive in exactly the stem-major order above: stem 0 counting up from
 * group 0, then a single step to stem 1 group 0, and so on. The first stem
 * transition also TEACHES it how many groups a stem has, and every later
 * transition must then happen at exactly that count. So it needs no frame
 * count, no sector count and no index record -- which matters, because during
 * a bulk upload none of those have been written yet. The commit check
 * cross-examines the number it learned against the record that finally arrives.
 *
 * THE PADDING. A group is always 340 frames, so the tail of each stem's LAST
 * group is zero padding, and a group header deliberately carries identity
 * rather than timing -- there is nothing here that says where the real audio
 * stops. Every group is therefore folded WHOLE, and finish_stem_hashes() below
 * takes the padding back off once a record supplies the frame count.
 */
static bool accumulate_one_sector_planar(uint32_t stem_hash[ST11_STEM_COUNT],
					  const uint8_t sector[ST11_SECTOR_BYTES],
					  uint32_t *next_stem, uint32_t *next_group,
					  uint32_t *groups_per_stem)
{
	uint32_t i;

	for (i = 0; i < ST_PL_GROUPS_PER_SECTOR; i++) {
		const uint8_t *group = sector + (size_t)i * ST_PL_GROUP_BYTES;
		st_pl_header_t h;

		if (!st_pl_read_header(group, &h)) {
			return false;
		}

		if (h.stem == *next_stem && h.group_index == *next_group) {
			/* the run continues inside this stem */
			(*next_group)++;
		} else if (h.stem == *next_stem + 1u && h.group_index == 0u && *next_group > 0u &&
			   (*groups_per_stem == 0u || *groups_per_stem == *next_group)) {
			/* one step to the next stem, at the group count this
			 * song uses. The first such step establishes that
			 * count; every later one has to match it. */
			*groups_per_stem = *next_group;
			*next_stem = h.stem;
			*next_group = 1u;
		} else {
			return false;
		}

		uint32_t f;

		for (f = 0; f < ST_PL_FRAMES_PER_GROUP; f++) {
			const uint8_t *frame = group + st_pl_frame_off(f);

			/* The stored bytes ARE the checksummed bytes: L then
			 * R, signed little-endian at ST11_PCM_BIT_DEPTH,
			 * exactly the order src/sp1/song.ts hashes them in --
			 * four bytes a frame at v1.3, six at v1.2. Decoding to
			 * int32 and re-encoding would be the same bytes back
			 * again, which is why ST_PL_FRAME_BYTES is folded
			 * whole rather than sample by sample. */
			stem_hash[h.stem] = st_checksum32_update(stem_hash[h.stem], frame,
								   ST_PL_FRAME_BYTES);
		}
	}

	return true;
}

/*
 * THE SHARED FINISH, for the same reason accumulate_one_sector() is shared.
 * Removes the zero padding at the tail of every stem's last group, which is
 * the last thing folded into that stem's hash because the layout is
 * stem-major. Exact, not approximate -- see st_checksum32_unfold_zeros().
 *
 * Returns false if `candidate`'s own geometry is not self-consistent, so a
 * record claiming a frame count its sector count cannot hold is refused here
 * rather than silently producing some other song's checksums.
 */
static bool finish_stem_hashes(uint32_t stem_hash[ST11_STEM_COUNT], uint32_t groups,
				uint32_t frames)
{
	if (groups == 0u || frames == 0u || st_pl_groups_for_frames(frames) != groups) {
		return false;
	}

	uint32_t pad_frames = groups * ST_PL_FRAMES_PER_GROUP - frames;
	size_t pad_bytes = (size_t)pad_frames * ST_PL_FRAME_BYTES;
	uint32_t si;

	for (si = 0; si < ST11_STEM_COUNT; si++) {
		stem_hash[si] = st_checksum32_unfold_zeros(stem_hash[si], pad_bytes);
	}
	return true;
}

void st_ab_session_accumulate_sector(st_ab_session_t *s, uint32_t sector_index,
				      const uint8_t sector[ST11_SECTOR_BYTES])
{
	if (!s->open || s->closed || s->kind != ST_AB_SESSION_REPLACE) {
		return;
	}
	if (!s->acc_valid) {
		return; /* already invalidated: stays that way for this session */
	}
	if (sector_index < s->acc_sectors) {
		return; /* duplicate of an already-accumulated sector (idempotent retry) */
	}
	if (sector_index != s->acc_sectors) {
		s->acc_valid = false; /* a gap: the accumulation can no longer be complete */
		return;
	}
	if (!accumulate_one_sector_planar(s->acc_stem_hash, sector, &s->acc_next_stem,
					   &s->acc_next_group, &s->acc_groups_per_stem)) {
		s->acc_valid = false;
		return;
	}
	s->acc_sectors++;
}

bool st_ab_session_verify_accumulated(const st_ab_session_t *s, const st_stix_record_t *candidate)
{
	if (s->kind != ST_AB_SESSION_REPLACE || !s->acc_valid) {
		return false;
	}
	/* Exactly the declared song: a shortfall means part of it was never
	 * read back at all, and an overshoot means the accumulation does not
	 * describe THIS record. Neither may pass. */
	/* The fast path exists only for the layout this firmware actually
	 * stores. A record declaring anything else is not a failure here --
	 * it is simply not accumulable, and the caller's documented fallback
	 * (the full re-read, which does dispatch on the declared version)
	 * handles it. */
	if (candidate->format_major != ST11_FORMAT_MAJOR ||
	    (candidate->format_minor != 3u && candidate->format_minor != 2u)) {
		return false;
	}
	if (candidate->sector_count == 0u || s->acc_sectors != candidate->sector_count) {
		return false;
	}
	/* Same end-of-scan position check the full re-read makes, against the
	 * SAME record: the group count the scan learned off the media must be
	 * the one the record declares, and the scan must have ended one past
	 * the last stem's last group. */
	if (s->acc_next_stem != ST_PL_STEMS - 1u || s->acc_next_group != candidate->sector_count ||
	    s->acc_groups_per_stem != candidate->sector_count) {
		return false;
	}

	uint32_t stem_hash[ST11_STEM_COUNT];
	uint32_t si;

	for (si = 0; si < ST11_STEM_COUNT; si++) {
		stem_hash[si] = s->acc_stem_hash[si];
	}
	if (!finish_stem_hashes(stem_hash, candidate->sector_count, candidate->frames)) {
		return false;
	}

	return stem_hashes_match_candidate(stem_hash, candidate);
}
