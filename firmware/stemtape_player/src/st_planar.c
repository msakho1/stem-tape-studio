/*
 * st_planar.c -- see st_planar.h for why the layout changes and why batching
 * is what makes it cost nothing.
 */

#include <string.h>

#include "st_planar.h"
#include "st_sector_v11.h"

/*
 * THE GEOMETRY, PINNED. Every one of these is load-bearing somewhere in the
 * header's argument, and a silent change to any would make a v1.2 song a
 * different length from the v1.1 song it was converted from -- which the STIX
 * geometry fields would then describe wrongly.
 */
_Static_assert(ST_PL_HEADER_BYTES + ST_PL_FRAMES_PER_GROUP * ST_PL_FRAME_BYTES ==
		       ST_PL_GROUP_BYTES,
	       "a group must be exactly its header plus its frames");
_Static_assert(ST_PL_GROUP_BYTES == 2048u, "a group is 4 physical blocks");
_Static_assert(ST_PL_BLOCKS_PER_GROUP_ALL == ST11_BLOCKS_PER_SECTOR,
	       "one group index across four stems must cost what a v1.1 sector cost");
_Static_assert(ST_PL_FRAMES_PER_GROUP == ST11_FRAMES_PER_SECTOR,
	       "frames per group must equal v1.1 frames per sector");
_Static_assert(ST_PL_STEMS * ST_PL_FRAME_BYTES == ST11_BYTES_PER_FRAME,
	       "four stems of one frame must be a v1.1 frame");
_Static_assert(ST_PL_OFF_FRAMES == ST_PL_HEADER_BYTES, "frames follow the header");

uint32_t st_pl_group_block(uint32_t song_start_block, uint32_t groups,
			    uint32_t stem, uint32_t group_index)
{
	return song_start_block +
	       (stem * groups + group_index) * ST_PL_GROUP_BLOCKS;
}

void st_pl_write_header(uint8_t group[ST_PL_GROUP_BYTES], uint32_t stem,
			 uint32_t group_index)
{
	group[ST_PL_OFF_MAGIC_0] = ST_PL_MAGIC_0;
	group[ST_PL_OFF_MAGIC_1] = ST_PL_MAGIC_1;
	group[ST_PL_OFF_STEM]    = (uint8_t)stem;
	group[ST_PL_OFF_FLAGS]   = (uint8_t)ST_PL_FORMAT_V13;
	group[ST_PL_OFF_GROUP + 0u] = (uint8_t)(group_index & 0xffu);
	group[ST_PL_OFF_GROUP + 1u] = (uint8_t)((group_index >> 8) & 0xffu);
	group[ST_PL_OFF_GROUP + 2u] = (uint8_t)((group_index >> 16) & 0xffu);
	group[ST_PL_OFF_GROUP + 3u] = (uint8_t)((group_index >> 24) & 0xffu);
}

bool st_pl_read_header(const uint8_t group[ST_PL_GROUP_BYTES],
			st_pl_header_t *out)
{
	if (group[ST_PL_OFF_MAGIC_0] != ST_PL_MAGIC_0 ||
	    group[ST_PL_OFF_MAGIC_1] != ST_PL_MAGIC_1) {
		return false;
	}
	out->stem  = group[ST_PL_OFF_STEM];
	out->flags = group[ST_PL_OFF_FLAGS];
	out->group_index = (uint32_t)group[ST_PL_OFF_GROUP + 0u] |
			   ((uint32_t)group[ST_PL_OFF_GROUP + 1u] << 8) |
			   ((uint32_t)group[ST_PL_OFF_GROUP + 2u] << 16) |
			   ((uint32_t)group[ST_PL_OFF_GROUP + 3u] << 24);
	/* A stem index outside the four that exist is a malformed group, not a
	 * group belonging to some other stem -- reject rather than let it
	 * compare unequal and read as an ordinary miss. */
	if (out->stem >= ST_PL_STEMS) {
		return false;
	}
	/* THE FLAGS BYTE IS THE PAYLOAD-WIDTH VERSION, and only the version
	 * this build decodes is accepted.
	 *
	 * It used to be "reserved, must be zero", with the right reasoning
	 * attached: a format this build does not understand must not be played
	 * as though the byte meant nothing. v1.3 gives the byte a value instead
	 * of reserving it, and the rule is unchanged -- an unrecognised version
	 * is refused. What that now also catches is the case the old rule could
	 * not: a v1.2 group, whose flags are 0, whose magic and stem and group
	 * index are all correct, and whose 24-bit samples would otherwise be
	 * decoded as 16-bit ones and played at full scale. See
	 * ST_PL_FORMAT_V13. */
	if (out->flags != ST_PL_FORMAT_V13) {
		return false;
	}
	return true;
}

bool st_pl_validate(const uint8_t group[ST_PL_GROUP_BYTES], uint32_t want_stem,
		     uint32_t want_group)
{
	st_pl_header_t h;

	if (!st_pl_read_header(group, &h)) {
		return false;
	}
	return h.stem == want_stem && h.group_index == want_group;
}

uint32_t st_pl_plan_batch(st_pl_read_t out[ST_PL_STEMS],
			   uint32_t song_start_block, uint32_t groups,
			   const uint32_t head_group[ST_PL_STEMS],
			   const st_pl_dir_t dir[ST_PL_STEMS],
			   uint32_t groups_per_batch)
{
	uint32_t k;

	for (k = 0u; k < ST_PL_STEMS; k++) {
		uint32_t head = head_group[k];
		uint32_t first, n;

		if (groups_per_batch == 0u || groups == 0u || head >= groups) {
			out[k].stem = k;
			out[k].first_group = 0u;
			out[k].groups = 0u;
			out[k].block = song_start_block;
			out[k].blocks = 0u;
			continue;
		}

		if (dir[k] == ST_PL_REV) {
			/*
			 * A REVERSED STEM STILL READS ASCENDING. Its next N
			 * groups to PLAY are head, head-1, ... head-N+1, which
			 * as an address range is [head-N+1, head]. Reading it
			 * back-to-front would turn one contiguous run into N
			 * separate reads and give away the entire saving.
			 */
			n = (head + 1u < groups_per_batch) ? (head + 1u)
							    : groups_per_batch;
			first = head + 1u - n;
		} else {
			first = head;
			n = groups - head;
			if (n > groups_per_batch) {
				n = groups_per_batch;
			}
		}

		out[k].stem        = k;
		out[k].first_group = first;
		out[k].groups      = n;
		out[k].block       = st_pl_group_block(song_start_block, groups,
						        k, first);
		out[k].blocks      = n * ST_PL_GROUP_BLOCKS;
	}
	return ST_PL_STEMS;
}

bool st_pl_from_v11_sector(const uint8_t sector[ST11_SECTOR_BYTES],
			    uint8_t groups[ST_PL_STEMS][ST_PL_GROUP_BYTES])
{
	st11_sector_header_t hdr;
	uint32_t f, k;

	if (!st11_sector_read_header(sector, &hdr)) {
		return false;
	}
	/* A sector claiming more frames than one holds is malformed, not a
	 * sector to copy as much of as fits. */
	if (hdr.frame_count > ST11_FRAMES_PER_SECTOR) {
		return false;
	}

	for (k = 0u; k < ST_PL_STEMS; k++) {
		memset(groups[k], 0, ST_PL_GROUP_BYTES);
		st_pl_write_header(groups[k], k, hdr.sector_index);
	}

	/*
	 * BYTES ARE MOVED, NOT RECOMPUTED. Stem k's six bytes for frame f sit
	 * at a fixed offset inside the v1.1 frame; they are copied verbatim.
	 * Decoding to samples and re-encoding would be a second chance to lose
	 * a bit, and would break the property the fixture test relies on --
	 * that every per-stem checksum is untouched by the migration.
	 */
	for (f = 0u; f < hdr.frame_count; f++) {
		const uint8_t *src = sector + ST11_SECTOR_HEADER_BYTES +
				     f * ST11_BYTES_PER_FRAME;

		for (k = 0u; k < ST_PL_STEMS; k++) {
			memcpy(groups[k] + st_pl_frame_off(f),
			       src + k * ST_PL_FRAME_BYTES, ST_PL_FRAME_BYTES);
		}
	}
	return true;
}

/* Byte-for-byte the sign extension st_sector_v11.c's own get_i24le() performs.
 * Duplicated rather than exported because the two codecs are deliberately
 * separate types (see st_sector_v11.h), and a test pins the two decoders equal
 * on real recorded audio rather than trusting this comment. */
/*
 * -O2 ON THE DECODE PATH, for exactly the reason sp1_emmc.c, st_stem_mix.c,
 * st_sector_v11.c and st_stem_stream.c already carry the same attribute: this
 * project builds at Zephyr's default size optimisation, and these three
 * functions run 48,000 times a second on the thread with a hard 5.333 ms
 * deadline. st_planar.c was the ONLY file left in the audio hot path without
 * it -- an omission from the v1.2 port, not a decision.
 *
 * This changes no arithmetic. The full-playback gate hashes the decoded audio
 * over the whole recorded song (0x2a737e00) and is the mechanical proof that
 * it did not.
 */
/*
 * ONE IMPLEMENTATION, NOT TWO. This is the out-of-line entry point, and it
 * calls the header's inline primitive rather than repeating its arithmetic.
 *
 * The v1.2 version open-coded a three-byte little-endian assemble HERE and
 * again in st_planar.h, so the two could drift and only a test standing
 * between them said otherwise. With the payload now a single aligned word
 * there is even less reason to write it twice: see st_pl_decode_stem_inline()
 * for why the load is one instruction and what is asserted to keep it so.
 */
__attribute__((optimize("O2")))
void st_pl_decode_stem(const uint8_t *group, uint32_t frame_in_group,
			int32_t *out_l, int32_t *out_r)
{
	st_pl_decode_stem_inline(group, frame_in_group, out_l, out_r);
}

__attribute__((optimize("O2")))
void st_pl_decode_frame(const uint8_t *const groups[ST_PL_STEMS],
			 const uint32_t frame_in_group[ST_PL_STEMS],
			 st11_audio_frame_t *out)
{
	uint32_t k;

	for (k = 0u; k < ST_PL_STEMS; k++) {
		st_pl_decode_stem(groups[k], frame_in_group[k], &out->stem_l[k],
				   &out->stem_r[k]);
	}
}
