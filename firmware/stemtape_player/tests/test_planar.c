/*
 * test_planar.c -- v1.2 song-planar addressing.
 *
 * The case that matters most is case_cost_is_flat_in_which_stem_diverges():
 * the entire justification for changing the storage format is that reversing
 * a stem changes only WHERE a read goes, never how many reads or how many
 * bytes. Under v1.1 that was false, and measurably so -- reversing a middle
 * stem cost about what two do, which failed on hardware with 742 dropouts.
 * If that property ever stops holding, the format has no reason to exist.
 */

#include <stdio.h>
#include <string.h>

#include "st_planar.h"
#include "st_sector_v11.h"

static int g_fail;
static int g_checks;

static void ck(bool cond, const char *what)
{
	g_checks++;
	if (!cond) {
		printf("    FAIL: %s\n", what);
		g_fail++;
	}
}

static void case_geometry_matches_v11(void)
{
	printf("  a group is its header plus 340 frames, and a song is the same length as v1.1\n");
	ck(ST_PL_GROUP_BYTES == 2048u, "group is 2048 bytes");
	ck(ST_PL_HEADER_BYTES + ST_PL_FRAMES_PER_GROUP * ST_PL_FRAME_BYTES ==
		   ST_PL_GROUP_BYTES,
	   "8 + 340*6 == 2048");
	ck(ST_PL_BLOCKS_PER_GROUP_ALL == ST11_BLOCKS_PER_SECTOR,
	   "one group index across 4 stems costs a v1.1 sector");
	ck(ST_PL_FRAMES_PER_GROUP == ST11_FRAMES_PER_SECTOR,
	   "frames per group == v1.1 frames per sector");

	/* The real parity claim: a song converted from v1.1 occupies exactly
	 * the same number of blocks, so every STIX geometry field carries. */
	for (uint32_t frames = 1u; frames < 400000u; frames += 3571u) {
		uint32_t groups = st_pl_groups_for_frames(frames);
		uint32_t v11_sectors =
			(frames + ST11_FRAMES_PER_SECTOR - 1u) / ST11_FRAMES_PER_SECTOR;

		ck(groups == v11_sectors, "group count == v1.1 sector count");
		ck(st_pl_song_blocks(groups) ==
			   v11_sectors * ST11_BLOCKS_PER_SECTOR,
		   "song blocks == v1.1 song blocks");
	}
}

static void case_addresses_tile_the_region_exactly_once(void)
{
	printf("  every stem/group lands on its own 4 blocks -- no gap, no overlap\n");
	const uint32_t start = 4096u, groups = 37u;
	const uint32_t total = st_pl_song_blocks(groups);
	static uint8_t seen[37u * ST_PL_BLOCKS_PER_GROUP_ALL];

	memset(seen, 0, sizeof(seen));
	for (uint32_t s = 0u; s < ST_PL_STEMS; s++) {
		for (uint32_t g = 0u; g < groups; g++) {
			uint32_t b = st_pl_group_block(start, groups, s, g);

			ck(b >= start && b + ST_PL_GROUP_BLOCKS <= start + total,
			   "group lies inside the song region");
			for (uint32_t i = 0u; i < ST_PL_GROUP_BLOCKS; i++) {
				uint32_t off = b - start + i;

				ck(off < total, "block offset inside the region");
				if (off < total) {
					ck(seen[off] == 0u, "no block is claimed twice");
					seen[off] = 1u;
				}
			}
		}
	}
	for (uint32_t i = 0u; i < total; i++) {
		ck(seen[i] == 1u, "no block is left unclaimed");
	}
}

static void case_each_stem_is_contiguous(void)
{
	printf("  a stem's whole timeline is one unbroken run -- the property batching needs\n");
	const uint32_t start = 512u, groups = 61u;

	for (uint32_t s = 0u; s < ST_PL_STEMS; s++) {
		for (uint32_t g = 1u; g < groups; g++) {
			uint32_t prev = st_pl_group_block(start, groups, s, g - 1u);
			uint32_t cur  = st_pl_group_block(start, groups, s, g);

			ck(cur == prev + ST_PL_GROUP_BLOCKS,
			   "consecutive groups of one stem are adjacent");
		}
	}
	/* And the stems do not interleave: stem s ends where stem s+1 begins. */
	for (uint32_t s = 1u; s < ST_PL_STEMS; s++) {
		uint32_t end_prev =
			st_pl_group_block(start, groups, s - 1u, groups - 1u) +
			ST_PL_GROUP_BLOCKS;
		uint32_t begin = st_pl_group_block(start, groups, s, 0u);

		ck(begin == end_prev, "stem regions abut with no gap");
	}
}

static void case_header_round_trip(void)
{
	printf("  a group says which stem and which span it is\n");
	uint8_t g[ST_PL_GROUP_BYTES];
	st_pl_header_t h;

	for (uint32_t s = 0u; s < ST_PL_STEMS; s++) {
		const uint32_t idx[] = { 0u, 1u, 255u, 256u, 65535u, 65536u, 4000000u };

		for (size_t i = 0u; i < sizeof(idx) / sizeof(idx[0]); i++) {
			memset(g, 0xAB, sizeof(g));
			st_pl_write_header(g, s, idx[i]);
			ck(st_pl_read_header(g, &h), "header parses");
			ck(h.stem == s, "stem round-trips");
			ck(h.group_index == idx[i], "group index round-trips");
			ck(h.flags == 0u, "flags are zero");
			ck(st_pl_validate(g, s, idx[i]), "validates against itself");
		}
	}
}

static void case_validation_catches_a_misaddressed_read(void)
{
	printf("  a well-formed group from the WRONG address is refused\n");
	uint8_t g[ST_PL_GROUP_BYTES];

	st_pl_write_header(g, 2u, 100u);
	ck(st_pl_validate(g, 2u, 100u), "the right group passes");
	ck(!st_pl_validate(g, 3u, 100u), "wrong stem is refused");
	ck(!st_pl_validate(g, 2u, 101u), "wrong group index is refused");
	ck(!st_pl_validate(g, 2u, 99u), "off-by-one the other way is refused");

	/* This is the case the per-group header exists for: a diverging stem's
	 * read is a group-only read, so a mis-address has nothing else to be
	 * caught by. */
	uint8_t other[ST_PL_GROUP_BYTES];
	st_pl_write_header(other, 2u, 101u);
	ck(!st_pl_validate(other, 2u, 100u),
	   "the neighbouring group of the same stem is refused");
}

static void case_malformed_groups_are_refused(void)
{
	printf("  garbage, a bad stem id and a reserved flag are all refused\n");
	uint8_t g[ST_PL_GROUP_BYTES];
	st_pl_header_t h;

	memset(g, 0, sizeof(g));
	ck(!st_pl_read_header(g, &h), "all zeroes has no magic");

	memset(g, 0xFF, sizeof(g));
	ck(!st_pl_read_header(g, &h), "all ones has no magic");

	st_pl_write_header(g, 1u, 7u);
	g[ST_PL_OFF_MAGIC_0] ^= 0xFFu;
	ck(!st_pl_read_header(g, &h), "corrupt first magic byte");
	st_pl_write_header(g, 1u, 7u);
	g[ST_PL_OFF_MAGIC_1] ^= 0xFFu;
	ck(!st_pl_read_header(g, &h), "corrupt second magic byte");

	st_pl_write_header(g, 1u, 7u);
	g[ST_PL_OFF_STEM] = (uint8_t)ST_PL_STEMS;
	ck(!st_pl_read_header(g, &h), "a stem id past the last stem");
	g[ST_PL_OFF_STEM] = 0xFFu;
	ck(!st_pl_read_header(g, &h), "a wildly out of range stem id");

	st_pl_write_header(g, 1u, 7u);
	g[ST_PL_OFF_FLAGS] = 1u;
	ck(!st_pl_read_header(g, &h), "a reserved flag bit set");
}

static void case_forward_batch(void)
{
	printf("  a forward batch is N groups from the head, one read\n");
	st_pl_read_t p[ST_PL_STEMS];
	const uint32_t start = 1024u, groups = 50u;
	const uint32_t head[ST_PL_STEMS] = { 10u, 10u, 10u, 10u };
	const st_pl_dir_t dir[ST_PL_STEMS] = { ST_PL_FWD, ST_PL_FWD, ST_PL_FWD, ST_PL_FWD };

	ck(st_pl_plan_batch(p, start, groups, head, dir, 4u) == ST_PL_STEMS,
	   "one read per stem");
	for (uint32_t k = 0u; k < ST_PL_STEMS; k++) {
		ck(p[k].stem == k, "read names its stem");
		ck(p[k].first_group == 10u, "starts at the head");
		ck(p[k].groups == 4u, "covers the batch");
		ck(p[k].blocks == 16u, "4 groups is 16 blocks");
		ck(p[k].block == st_pl_group_block(start, groups, k, 10u),
		   "addresses that stem's own group 10");
	}
}

static void case_reverse_batch_reads_ascending(void)
{
	printf("  a REVERSED stem's batch is [head-N+1, head], still one ascending run\n");
	st_pl_read_t p[ST_PL_STEMS];
	const uint32_t start = 0u, groups = 50u;
	const uint32_t head[ST_PL_STEMS] = { 20u, 20u, 20u, 20u };
	const st_pl_dir_t dir[ST_PL_STEMS] = { ST_PL_REV, ST_PL_FWD, ST_PL_FWD, ST_PL_FWD };

	st_pl_plan_batch(p, start, groups, head, dir, 4u);
	ck(p[0].first_group == 17u, "reverse batch begins N-1 below the head");
	ck(p[0].groups == 4u, "reverse batch is still N groups");
	ck(p[0].blocks == 16u, "reverse batch is still one 16-block run");
	ck(p[0].block == st_pl_group_block(start, groups, 0u, 17u),
	   "reverse batch addresses the LOW end of its range");
	/* The forward stems are unaffected by a neighbour reversing. */
	for (uint32_t k = 1u; k < ST_PL_STEMS; k++) {
		ck(p[k].first_group == 20u, "a forward stem still starts at its head");
		ck(p[k].blocks == 16u, "a forward stem's read is unchanged");
	}
}

static void case_cost_is_flat_in_which_stem_diverges(void)
{
	printf("  THE REASON THE FORMAT EXISTS: cost does not depend on which stem,\n");
	printf("  which direction, or how many diverge\n");
	const uint32_t start = 2048u, groups = 200u;
	const uint32_t N = 4u;
	st_pl_read_t p[ST_PL_STEMS];
	uint32_t base_reads = 0u, base_blocks = 0u;

	/* Baseline: everything forward, all heads together. */
	{
		const uint32_t head[ST_PL_STEMS] = { 80u, 80u, 80u, 80u };
		const st_pl_dir_t dir[ST_PL_STEMS] = { ST_PL_FWD, ST_PL_FWD, ST_PL_FWD, ST_PL_FWD };

		st_pl_plan_batch(p, start, groups, head, dir, N);
		for (uint32_t k = 0u; k < ST_PL_STEMS; k++) {
			if (p[k].groups > 0u) base_reads++;
			base_blocks += p[k].blocks;
		}
		ck(base_reads == 4u, "four reads with nothing diverging");
		ck(base_blocks == 64u, "64 blocks per batch of 4");
	}

	/* Every single-stem reversal, and every stem as the reversed one --
	 * this is exactly what v1.1 could not do for stems 1 and 2. */
	for (uint32_t rev = 0u; rev < ST_PL_STEMS; rev++) {
		uint32_t head[ST_PL_STEMS] = { 80u, 80u, 80u, 80u };
		st_pl_dir_t dir[ST_PL_STEMS] = { ST_PL_FWD, ST_PL_FWD, ST_PL_FWD, ST_PL_FWD };
		uint32_t reads = 0u, blocks = 0u;

		dir[rev] = ST_PL_REV;
		head[rev] = 150u;            /* and somewhere else entirely */
		st_pl_plan_batch(p, start, groups, head, dir, N);
		for (uint32_t k = 0u; k < ST_PL_STEMS; k++) {
			if (p[k].groups > 0u) reads++;
			blocks += p[k].blocks;
		}
		ck(reads == base_reads, "reversing any stem costs the same READS");
		ck(blocks == base_blocks, "reversing any stem costs the same BLOCKS");
	}

	/* All four diverging, to four different places, two of them backwards.
	 * Still the same cost -- which is why the one-at-a-time rule is a
	 * product choice rather than something the format forces. */
	{
		const uint32_t head[ST_PL_STEMS] = { 10u, 60u, 120u, 190u };
		const st_pl_dir_t dir[ST_PL_STEMS] = { ST_PL_REV, ST_PL_FWD, ST_PL_REV, ST_PL_FWD };
		uint32_t reads = 0u, blocks = 0u;

		st_pl_plan_batch(p, start, groups, head, dir, N);
		for (uint32_t k = 0u; k < ST_PL_STEMS; k++) {
			if (p[k].groups > 0u) reads++;
			blocks += p[k].blocks;
		}
		ck(reads == base_reads, "four divergent heads cost the same READS");
		ck(blocks == base_blocks, "four divergent heads cost the same BLOCKS");
	}
}

static void case_batch_size_is_what_pays_for_it(void)
{
	printf("  batching is the whole economy: 1 group per read is the failure case\n");
	const uint32_t start = 0u, groups = 400u;
	const uint32_t head[ST_PL_STEMS] = { 100u, 100u, 100u, 100u };
	const st_pl_dir_t dir[ST_PL_STEMS] = { ST_PL_FWD, ST_PL_FWD, ST_PL_FWD, ST_PL_FWD };
	st_pl_read_t p[ST_PL_STEMS];

	/* Reads per SPAN of audio: four reads per batch, one batch per N spans.
	 * N=1 is four reads a span (the measured failure); N=4 is one. */
	const uint32_t want_blocks[] = { 4u, 8u, 16u, 32u };
	const uint32_t Ns[] = { 1u, 2u, 4u, 8u };

	for (size_t i = 0u; i < 4u; i++) {
		st_pl_plan_batch(p, start, groups, head, dir, Ns[i]);
		for (uint32_t k = 0u; k < ST_PL_STEMS; k++) {
			ck(p[k].groups == Ns[i], "batch holds N groups");
			ck(p[k].blocks == want_blocks[i], "blocks scale with N");
		}
	}
}

static void case_edges(void)
{
	printf("  song ends, group zero, and empty songs do not read past anything\n");
	st_pl_read_t p[ST_PL_STEMS];
	const uint32_t start = 64u, groups = 10u;
	st_pl_dir_t fwd[ST_PL_STEMS] = { ST_PL_FWD, ST_PL_FWD, ST_PL_FWD, ST_PL_FWD };
	st_pl_dir_t rev[ST_PL_STEMS] = { ST_PL_REV, ST_PL_REV, ST_PL_REV, ST_PL_REV };

	/* Forward at the last group: the batch is clamped, never past the end. */
	{
		const uint32_t head[ST_PL_STEMS] = { 8u, 8u, 8u, 8u };

		st_pl_plan_batch(p, start, groups, head, fwd, 4u);
		for (uint32_t k = 0u; k < ST_PL_STEMS; k++) {
			ck(p[k].groups == 2u, "clamped to what is left");
			ck(p[k].first_group + p[k].groups <= groups,
			   "never addresses past the last group");
		}
	}
	/* Reverse at group 0: the batch is clamped, never below zero. */
	{
		const uint32_t head[ST_PL_STEMS] = { 0u, 1u, 2u, 3u };

		st_pl_plan_batch(p, start, groups, head, rev, 4u);
		ck(p[0].first_group == 0u && p[0].groups == 1u, "reverse at 0 is one group");
		ck(p[1].first_group == 0u && p[1].groups == 2u, "reverse at 1 is two");
		ck(p[2].first_group == 0u && p[2].groups == 3u, "reverse at 2 is three");
		ck(p[3].first_group == 0u && p[3].groups == 4u, "reverse at 3 is a full batch");
	}
	/* A head past the end, an empty song and a zero batch all read nothing. */
	{
		const uint32_t past[ST_PL_STEMS] = { 10u, 10u, 10u, 10u };
		const uint32_t zero[ST_PL_STEMS] = { 0u, 0u, 0u, 0u };

		st_pl_plan_batch(p, start, groups, past, fwd, 4u);
		for (uint32_t k = 0u; k < ST_PL_STEMS; k++) {
			ck(p[k].groups == 0u && p[k].blocks == 0u, "head past the end reads nothing");
		}
		/*
		 * THE SAME HEAD, REVERSED, and it is the case that actually
		 * needs the guard. Forward, a head at or past the last group
		 * yields nothing from the arithmetic alone (groups - head is
		 * zero or wraps into the clamp). Reverse does not: it counts
		 * DOWN from the head, so without the bounds check it happily
		 * plans a run whose top end is off the end of the song. A
		 * mutation removing that check survived until this existed.
		 */
		st_pl_plan_batch(p, start, groups, past, rev, 4u);
		for (uint32_t k = 0u; k < ST_PL_STEMS; k++) {
			ck(p[k].groups == 0u && p[k].blocks == 0u,
			   "a REVERSED head past the end reads nothing");
		}
		{
			const uint32_t way_past[ST_PL_STEMS] = { 999u, 999u, 999u, 999u };

			st_pl_plan_batch(p, start, groups, way_past, rev, 4u);
			for (uint32_t k = 0u; k < ST_PL_STEMS; k++) {
				ck(p[k].groups == 0u, "a reversed head far past the end reads nothing");
			}
		}
		/* And whatever a plan says, it never names a group off the end. */
		for (uint32_t h = 0u; h < groups + 4u; h++) {
			const uint32_t hh[ST_PL_STEMS] = { h, h, h, h };

			st_pl_plan_batch(p, start, groups, hh, rev, 4u);
			for (uint32_t k = 0u; k < ST_PL_STEMS; k++) {
				ck(p[k].first_group + p[k].groups <= groups,
				   "no reverse plan addresses past the last group");
			}
			st_pl_plan_batch(p, start, groups, hh, fwd, 4u);
			for (uint32_t k = 0u; k < ST_PL_STEMS; k++) {
				ck(p[k].first_group + p[k].groups <= groups,
				   "no forward plan addresses past the last group");
			}
		}
		st_pl_plan_batch(p, start, 0u, zero, fwd, 4u);
		for (uint32_t k = 0u; k < ST_PL_STEMS; k++) {
			ck(p[k].groups == 0u, "an empty song reads nothing");
		}
		st_pl_plan_batch(p, start, groups, zero, fwd, 0u);
		for (uint32_t k = 0u; k < ST_PL_STEMS; k++) {
			ck(p[k].groups == 0u, "a zero batch reads nothing");
		}
	}
}

static void case_conversion_refuses_a_malformed_sector(void)
{
	printf("  a v1.1 sector claiming more frames than one holds is refused\n");
	uint8_t sector[ST11_SECTOR_BYTES];
	uint8_t groups[ST_PL_STEMS][ST_PL_GROUP_BYTES];

	/* A well-formed sector converts. Real frames, with a distinct value per
	 * stem and channel so a converter that shuffled them could not pass. */
	static st11_audio_frame_t frames[ST11_FRAMES_PER_SECTOR];

	for (uint32_t f = 0u; f < ST11_FRAMES_PER_SECTOR; f++) {
		for (uint32_t k = 0u; k < ST11_STEM_COUNT; k++) {
			frames[f].stem_l[k] = (int32_t)(f * 8u + k * 2u);
			frames[f].stem_r[k] = (int32_t)(f * 8u + k * 2u + 1u);
		}
	}
	memset(sector, 0, sizeof(sector));
	st11_sector_encode(3u, 3u * ST11_FRAMES_PER_SECTOR, ST11_FRAMES_PER_SECTOR,
			    120u * 256u, 0u, frames, sector);
	ck(st_pl_from_v11_sector(sector, groups), "a full, valid sector converts");
	ck(st_pl_validate(groups[0], 0u, 3u), "and carries the sector's own index");

	/* And every sample landed where it belongs -- checked against the v1.1
	 * decoder rather than against this test's own offset arithmetic. */
	for (uint32_t f = 0u; f < ST11_FRAMES_PER_SECTOR; f += 37u) {
		st11_audio_frame_t got;

		st11_sector_decode_frame(sector, f, &got);
		for (uint32_t k = 0u; k < ST_PL_STEMS; k++) {
			ck(memcmp(groups[k] + st_pl_frame_off(f),
				   sector + ST11_SECTOR_HEADER_BYTES +
					   f * ST11_BYTES_PER_FRAME + k * ST_PL_FRAME_BYTES,
				   ST_PL_FRAME_BYTES) == 0,
			   "stem bytes land in that stem's group, unchanged");
			ck(got.stem_l[k] == (int32_t)(f * 8u + k * 2u),
			   "the v1.1 decoder still sees what was encoded");
		}
	}

	/*
	 * THE GUARD THAT MATTERS. frame_count is read from the sector, and the
	 * copy loop walks it. A value past ST11_FRAMES_PER_SECTOR would read
	 * off the end of the 8192-byte input -- so it is refused outright
	 * rather than clamped, because a sector claiming 400 frames is
	 * malformed, not a sector to take 340 frames of.
	 *
	 * The recorded fixture cannot cover this: it contains no malformed
	 * sector, and a mutation removing the check survived the fixture test
	 * until this case existed.
	 */
	{
		const uint32_t bad[] = { ST11_FRAMES_PER_SECTOR + 1u, 400u,
					  1000u, 0xFFFFFFFFu };

		for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); i++) {
			sector[ST11_SECTOR_OFF_FRAME_COUNT + 0u] = (uint8_t)(bad[i] & 0xffu);
			sector[ST11_SECTOR_OFF_FRAME_COUNT + 1u] = (uint8_t)((bad[i] >> 8) & 0xffu);
			sector[ST11_SECTOR_OFF_FRAME_COUNT + 2u] = (uint8_t)((bad[i] >> 16) & 0xffu);
			sector[ST11_SECTOR_OFF_FRAME_COUNT + 3u] = (uint8_t)((bad[i] >> 24) & 0xffu);
			ck(!st_pl_from_v11_sector(sector, groups),
			   "an overlong frame count is refused, not clamped");
		}
	}

	/*
	 * A SHORT SECTOR PADS WITH SILENCE. The destination is prefilled with
	 * noise first: a converter that wrote only the real frames would leave
	 * it there, and the last sector of every song is short. Caught here so
	 * the module's own suite stands alone rather than leaning on the
	 * recorded-fixture test for a property of this function.
	 */
	{
		const uint32_t short_count = 17u;
		uint8_t noisy[ST_PL_STEMS][ST_PL_GROUP_BYTES];

		st11_sector_encode(9u, 9u * ST11_FRAMES_PER_SECTOR, short_count,
				    120u * 256u, 0u, frames, sector);
		memset(noisy, 0x5A, sizeof(noisy));
		ck(st_pl_from_v11_sector(sector, noisy), "a short sector converts");
		for (uint32_t k = 0u; k < ST_PL_STEMS; k++) {
			bool clean = true;

			for (uint32_t off = st_pl_frame_off(short_count);
			     off < ST_PL_GROUP_BYTES; off++) {
				if (noisy[k][off] != 0u) { clean = false; break; }
			}
			ck(clean, "everything past the last real frame is silence");
			ck(memcmp(noisy[k] + st_pl_frame_off(short_count - 1u),
				   sector + ST11_SECTOR_HEADER_BYTES +
					   (short_count - 1u) * ST11_BYTES_PER_FRAME +
					   k * ST_PL_FRAME_BYTES,
				   ST_PL_FRAME_BYTES) == 0,
			   "and the last real frame is still there");
		}
	}

	/* And a sector with no valid magic converts to nothing at all. */
	memset(sector, 0, sizeof(sector));
	ck(!st_pl_from_v11_sector(sector, groups), "a sector with no magic is refused");
}

int main(void)
{
	printf("Stem Tape v1.2 SONG-PLANAR addressing\n");
	case_geometry_matches_v11();
	case_addresses_tile_the_region_exactly_once();
	case_each_stem_is_contiguous();
	case_header_round_trip();
	case_validation_catches_a_misaddressed_read();
	case_malformed_groups_are_refused();
	case_forward_batch();
	case_reverse_batch_reads_ascending();
	case_cost_is_flat_in_which_stem_diverges();
	case_batch_size_is_what_pays_for_it();
	case_edges();
	case_conversion_refuses_a_malformed_sector();

	printf("%s -- 12 cases, %d checks%s\n", g_fail ? "FAILED" : "ok", g_checks,
	       g_fail ? "" : ", 0 failures");
	return g_fail ? 1 : 0;
}
