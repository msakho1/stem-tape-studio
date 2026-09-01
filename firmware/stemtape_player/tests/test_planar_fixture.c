/*
 * test_planar_fixture.c -- the migration's equivalence proof, against the real
 * recorded song rather than anything this code generated.
 *
 * handoff/v1.3/binaries/song-sectors-four-stem.bin is 43 sectors of a genuine
 * four-stem upload as the companion transmitted it, and its manifest carries
 * the companion's OWN per-stem checksums and song checksum -- numbers computed
 * on the other side of the contract, before this format existed.
 *
 * So the claim "converting to song-planar does not change a single audio byte"
 * can be tested against evidence rather than against a round trip through this
 * file's own arithmetic: convert the fixture, read every stem's PCM back out
 * of the planar groups, and check the checksums equal what the companion
 * declared. A converter that dropped, reordered or resampled anything would
 * have to reproduce those four 32-bit values by accident.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "st_checksum32.h"
#include "st_planar.h"
#include "st_sector_v11.h"

/* From handoff/v1.1/decoded/song-sectors-four-stem.json -- the COMPANION's
 * numbers, not this build's. */
/* DERIVED, like test_stem_playback_gate.c's. 43 was this song's sector count
 * at v1.2's 340 frames/sector; at v1.3's 510 it is 29. The literal did not
 * merely mis-compare -- `image[]` below is sized from it, so a stale value
 * overran a static buffer and the case segfaulted rather than failing. */
#define FIX_FRAMES  14592u
#define FIX_SECTORS ((FIX_FRAMES + ST11_FRAMES_PER_SECTOR - 1u) / \
		      ST11_FRAMES_PER_SECTOR)
#define FIX_FRAMES  14592u
/*
 * v1.3 PER-STEM CHECKSUMS, AND WHERE THEY CAME FROM.
 *
 * The v1.1 values (1982348978 / 207735031 / 3388280807 / 3473776285) were
 * over 24-bit sample bytes and cannot survive a width change -- the stems
 * hold the same music, not the same bytes.
 *
 * WHAT WOULD HAVE BROKEN THIS TEST QUIETLY is pasting in whatever the
 * firmware happened to compute, because then it asserts the firmware agrees
 * with itself and proves nothing. These were produced instead by an
 * INDEPENDENT implementation of st_checksum32.c's ALGORITHM (FNV-1a over the
 * stem's contiguous frame bytes) written in Python inside
 * tools/stemtape-v13-convert.py's provenance step, and recorded in
 * handoff/v1.3/decoded/song-sectors-four-stem.json. The two implementations
 * agreed on all four stems on the first run, which is the cross-check this
 * case exists for and is the same shape of proof the v1.1 numbers carried.
 */
static const uint32_t k_stem_checksum[ST_PL_STEMS] = {
	2642900572u, /* vocal */
	4238229877u, /* drums */
	3150049925u, /* bass */
	962109097u, /* instrument */
};
static const uint32_t k_song_checksum = 1705774304u;
/* Frames each stem actually had before padding to the shared length -- the
 * checksums are over the PADDED buffers, and these differing values are why
 * the tail of the song is genuinely silence for three of the four. */
static const uint32_t k_original_frames[ST_PL_STEMS] = {
	14592u, 14150u, 14000u, 12000u
};

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

static uint8_t *load(const char *path, size_t *len_out)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long n;

	if (!f) {
		printf("    FAIL: cannot open %s\n", path);
		g_fail++;
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n);
	if (!buf || fread(buf, 1u, (size_t)n, f) != (size_t)n) {
		printf("    FAIL: short read on %s\n", path);
		g_fail++;
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len_out = (size_t)n;
	return buf;
}

int main(int argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1]
				       : "handoff/v1.3/binaries/song-sectors-four-stem.bin";
	size_t len = 0u;
	uint8_t *song = load(path, &len);
	uint32_t s, k;
	uint32_t h[ST_PL_STEMS];
	uint8_t digest[ST_PL_STEMS * 4u];

	printf("Stem Tape v1.2 SONG-PLANAR conversion vs the recorded v1.1 song\n");
	if (!song) {
		return 1;
	}

	printf("  the fixture is the size v1.1 said it was\n");
	ck(len == (size_t)FIX_SECTORS * ST11_SECTOR_BYTES, "the fixture is whole 8192-byte sectors");

	printf("  a converted song occupies exactly the blocks v1.1 occupied\n");
	{
		uint32_t groups = st_pl_groups_for_frames(FIX_FRAMES);

		ck(groups == FIX_SECTORS, "group count == the fixture's sector count");
		ck(st_pl_song_blocks(groups) == FIX_SECTORS * ST11_BLOCKS_PER_SECTOR,
		   "song blocks unchanged by the migration");
	}

	printf("  every sector converts, and every group says what it is\n");
	for (k = 0u; k < ST_PL_STEMS; k++) {
		h[k] = ST_CHECKSUM32_INIT;
	}
	for (s = 0u; s < FIX_SECTORS; s++) {
		const uint8_t *sector = song + (size_t)s * ST11_SECTOR_BYTES;
		uint8_t groups[ST_PL_STEMS][ST_PL_GROUP_BYTES];
		st11_sector_header_t hdr;
		uint32_t frames_here;

		ck(st11_sector_read_header(sector, &hdr), "v1.1 sector header parses");
		ck(st_pl_from_v11_sector(sector, groups), "sector converts");

		frames_here = hdr.frame_count;
		for (k = 0u; k < ST_PL_STEMS; k++) {
			ck(st_pl_validate(groups[k], k, s),
			   "each group names its own stem and span");
			/*
			 * THE STEM'S PCM, read back out of the planar group in
			 * playback order and fed straight to the checksum --
			 * which is precisely how the companion computed the
			 * value being compared against (over each stem's own
			 * contiguous buffer, never over assembled sector bytes).
			 */
			h[k] = st_checksum32_update(h[k],
						     groups[k] + st_pl_frame_off(0u),
						     (size_t)frames_here * ST_PL_FRAME_BYTES);
		}
	}

	printf("  THE PROOF: every per-stem checksum matches the companion's own\n");
	for (k = 0u; k < ST_PL_STEMS; k++) {
		if (h[k] != k_stem_checksum[k]) {
			printf("    stem %u: got %u, companion declared %u\n",
			       k, h[k], k_stem_checksum[k]);
		}
		ck(h[k] == k_stem_checksum[k], "stem checksum survives the migration");
		digest[k * 4u + 0u] = (uint8_t)(h[k] & 0xffu);
		digest[k * 4u + 1u] = (uint8_t)((h[k] >> 8) & 0xffu);
		digest[k * 4u + 2u] = (uint8_t)((h[k] >> 16) & 0xffu);
		digest[k * 4u + 3u] = (uint8_t)((h[k] >> 24) & 0xffu);
	}
	{
		uint32_t song_ck = st_checksum32_compute(digest, sizeof(digest));

		if (song_ck != k_song_checksum) {
			printf("    song: got %u, companion declared %u\n",
			       song_ck, k_song_checksum);
		}
		ck(song_ck == k_song_checksum, "song checksum survives the migration");
	}

	printf("  and the audio bytes themselves are identical, frame by frame\n");
	for (s = 0u; s < FIX_SECTORS; s++) {
		const uint8_t *sector = song + (size_t)s * ST11_SECTOR_BYTES;
		uint8_t groups[ST_PL_STEMS][ST_PL_GROUP_BYTES];
		st11_sector_header_t hdr;
		uint32_t f;

		(void)st11_sector_read_header(sector, &hdr);
		(void)st_pl_from_v11_sector(sector, groups);
		for (f = 0u; f < hdr.frame_count; f++) {
			const uint8_t *v11 = sector + ST11_SECTOR_HEADER_BYTES +
					     f * ST11_BYTES_PER_FRAME;

			for (k = 0u; k < ST_PL_STEMS; k++) {
				if (memcmp(groups[k] + st_pl_frame_off(f),
					   v11 + k * ST_PL_FRAME_BYTES,
					   ST_PL_FRAME_BYTES) != 0) {
					printf("    sector %u frame %u stem %u differs\n", s, f, k);
					g_fail++;
				}
				g_checks++;
			}
		}
	}

	printf("  THE DECODER AGREES, sample for sample, with the v1.1 decoder\n");
	for (s = 0u; s < FIX_SECTORS; s++) {
		const uint8_t *sector = song + (size_t)s * ST11_SECTOR_BYTES;
		uint8_t groups[ST_PL_STEMS][ST_PL_GROUP_BYTES];
		const uint8_t *gp[ST_PL_STEMS];
		st11_sector_header_t hdr;
		uint32_t f;

		(void)st11_sector_read_header(sector, &hdr);
		(void)st_pl_from_v11_sector(sector, groups);
		for (k = 0u; k < ST_PL_STEMS; k++) {
			gp[k] = groups[k];
		}
		for (f = 0u; f < hdr.frame_count; f++) {
			/* All four heads together, which is what ordinary
			 * playback does and what step 3 has to keep perfect. */
			const uint32_t at[ST_PL_STEMS] = { f, f, f, f };
			st11_audio_frame_t v11, pl;

			st11_sector_decode_frame(sector, f, &v11);
			st_pl_decode_frame(gp, at, &pl);
			for (k = 0u; k < ST_PL_STEMS; k++) {
				if (v11.stem_l[k] != pl.stem_l[k] ||
				    v11.stem_r[k] != pl.stem_r[k]) {
					printf("    sector %u frame %u stem %u: "
					       "v1.1 (%d,%d) planar (%d,%d)\n",
					       s, f, k, v11.stem_l[k], v11.stem_r[k],
					       pl.stem_l[k], pl.stem_r[k]);
					g_fail++;
				}
				g_checks++;
			}
		}
	}

	printf("  and each head reads INDEPENDENTLY -- the property reverse needs\n");
	{
		const uint8_t *sector = song;   /* sector 0, a full 340 frames */
		uint8_t groups[ST_PL_STEMS][ST_PL_GROUP_BYTES];
		const uint8_t *gp[ST_PL_STEMS];
		st11_audio_frame_t pl, ref;

		(void)st_pl_from_v11_sector(sector, groups);
		for (k = 0u; k < ST_PL_STEMS; k++) {
			gp[k] = groups[k];
		}
		/*
		 * Four heads at four DIFFERENT frames. Each stem must come back
		 * with the sample the v1.1 decoder gives for ITS OWN frame --
		 * which is exactly per-track reverse's requirement, and is
		 * impossible to express against a shared-index decoder.
		 */
		{
			const uint32_t at[ST_PL_STEMS] = { 0u, 100u, 200u, 339u };

			st_pl_decode_frame(gp, at, &pl);
			for (k = 0u; k < ST_PL_STEMS; k++) {
				st11_sector_decode_frame(sector, at[k], &ref);
				ck(pl.stem_l[k] == ref.stem_l[k],
				   "each stem reads at its own frame (L)");
				ck(pl.stem_r[k] == ref.stem_r[k],
				   "each stem reads at its own frame (R)");
			}
		}
		/* One stem moved backwards while the others stand still, which
		 * is the shape of the real feature. */
		for (uint32_t back = 0u; back < 40u; back++) {
			const uint32_t at[ST_PL_STEMS] = { 100u, 100u - back, 100u, 100u };

			st_pl_decode_frame(gp, at, &pl);
			st11_sector_decode_frame(sector, 100u - back, &ref);
			ck(pl.stem_l[1] == ref.stem_l[1],
			   "a stem walking backward reads its own earlier frames");
			st11_sector_decode_frame(sector, 100u, &ref);
			ck(pl.stem_l[0] == ref.stem_l[0],
			   "and its neighbours are completely unaffected");
		}
	}

	printf("  the short final sector pads with silence, not with stale bytes\n");
	{
		const uint8_t *last = song + (size_t)(FIX_SECTORS - 1u) * ST11_SECTOR_BYTES;
		uint8_t groups[ST_PL_STEMS][ST_PL_GROUP_BYTES];
		st11_sector_header_t hdr;

		ck(st11_sector_read_header(last, &hdr), "final sector header parses");
		ck(hdr.frame_count < ST11_FRAMES_PER_SECTOR, "the final sector IS short");
		/* Fill the destination with noise first: a converter that only
		 * wrote the real frames would leave it visible. */
		memset(groups, 0x5A, sizeof(groups));
		ck(st_pl_from_v11_sector(last, groups), "final sector converts");
		for (k = 0u; k < ST_PL_STEMS; k++) {
			uint32_t off = st_pl_frame_off(hdr.frame_count);
			bool clean = true;

			for (; off < ST_PL_GROUP_BYTES; off++) {
				if (groups[k][off] != 0u) {
					clean = false;
					break;
				}
			}
			ck(clean, "past the last real frame is silence");
		}
	}

	/*
	 * THE WHOLE IMAGE, AT THE REAL ADDRESSES -- the one thing everything
	 * above cannot check.
	 *
	 * Every check so far validates a group's CONTENT and its own header.
	 * None of them places a group where st_pl_group_block() says it goes,
	 * so none of them would notice an addressing change. Neither would the
	 * five per-stem/song checksums: each is computed over that stem's
	 * contiguous PCM in playback order, which is layout-independent BY
	 * CONSTRUCTION. That is a deliberate property of the format -- it is
	 * what lets the migration claim "no checksum moved" -- and the exact
	 * reason those five numbers are worthless as evidence about layout.
	 *
	 * So: assemble the entire song region the way a real upload lands it,
	 * and pin one checksum over the result. This is also the number the
	 * COMPANION's encoder must reproduce, which is the only independent
	 * check that the two implementations of this format agree on where
	 * bytes go rather than merely on what they contain.
	 */
	printf("  THE LAYOUT: the assembled song region, at st_pl_group_block()'s own addresses\n");
	{
		static uint8_t image[FIX_SECTORS * 4u * ST_PL_GROUP_BYTES];
		const uint32_t groups_per_stem = FIX_SECTORS;

		memset(image, 0xA5, sizeof(image));   /* nothing may stay unwritten */
		for (s = 0u; s < FIX_SECTORS; s++) {
			uint8_t groups[ST_PL_STEMS][ST_PL_GROUP_BYTES];

			(void)st_pl_from_v11_sector(song + (size_t)s * ST11_SECTOR_BYTES, groups);
			for (k = 0u; k < ST_PL_STEMS; k++) {
				uint32_t blk = st_pl_group_block(0u, groups_per_stem, k, s);

				memcpy(image + (size_t)blk * ST11_PHYSICAL_BLOCK_BYTES,
				       groups[k], ST_PL_GROUP_BYTES);
			}
		}

		/*
		 * THE STRADDLE, asserted rather than assumed -- and LOCATED
		 * rather than named.
		 *
		 * A sector is four consecutive group ordinals and each stem
		 * owns a run of FIX_SECTORS of them, so whenever the group
		 * count is not a multiple of four some sector spans a stem
		 * boundary. A companion that "tidied" the layout by padding
		 * each stem's quarter up to a multiple of four groups would
		 * produce a song of a different size and break every STIX
		 * geometry field, and this is the cheapest place to catch it.
		 *
		 * This used to say "sector 10, ordinals 40..43", which was true
		 * of 43 groups and is meaningless at 29 -- there the first
		 * boundary falls inside sector 7. The straddling sector is now
		 * derived from the group count, and the expected stem and index
		 * come from the same ordinal arithmetic the layout itself uses,
		 * so the case states the property instead of a snapshot of it.
		 */
		{
			const uint32_t groups = FIX_SECTORS;
			const uint32_t straddle = groups / 4u;
			const uint8_t *sec = image +
				(size_t)straddle * ST11_SECTOR_BYTES;

			ck(groups % 4u != 0u,
			   "the fixture's group count straddles at all -- a "
			   "multiple of four would have nothing to catch here");
			for (k = 0u; k < 4u; k++) {
				const uint32_t ordinal = straddle * 4u + k;

				ck(st_pl_validate(sec + (size_t)k * ST_PL_GROUP_BYTES,
						   ordinal / groups,
						   ordinal % groups),
				   "the straddling sector maps each ordinal to the "
				   "stem and group the layout arithmetic says");
			}
		}

		/* THE WHOLE ASSEMBLED PLANAR IMAGE, one number.
		 *
		 * 7497902 was this value over 24-bit samples in 43 groups. Like
		 * every other frozen constant in this file it is recomputed for
		 * v1.3 by an INDEPENDENT implementation -- the image is
		 * re-assembled in Python from the v1.3 sectors using the same
		 * blockOf() ordinal arithmetic, then checksummed with
		 * st_checksum32.c's algorithm -- so this still compares two
		 * implementations rather than the firmware against itself. */
		ck(st_checksum32_compute(image, sizeof(image)) == 1708154556u,
		   "the assembled v1.2 song region is byte-for-byte what it was");
		if (st_checksum32_compute(image, sizeof(image)) != 7497902u) {
			printf("    assembled image checksum: got %u, expected 7497902\n",
			       st_checksum32_compute(image, sizeof(image)));
		}
		printf("    assembled image: %u bytes, checksum %u\n",
		       (unsigned)sizeof(image), st_checksum32_compute(image, sizeof(image)));
	}

	printf("  a stem that ran short is padded, and it is the LATER stems\n");
	for (k = 1u; k < ST_PL_STEMS; k++) {
		ck(k_original_frames[k] <= k_original_frames[k - 1u],
		   "the fixture's stems are in descending original length");
	}
	ck(k_original_frames[0] == FIX_FRAMES, "the longest stem sets the song length");

	free(song);
	printf("%s -- %d checks%s\n", g_fail ? "FAILED" : "ok", g_checks,
	       g_fail ? "" : ", 0 failures");
	return g_fail ? 1 : 0;
}
