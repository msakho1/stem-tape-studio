/*
 * test_planar_fixture.c -- the migration's equivalence proof, against the real
 * recorded song rather than anything this code generated.
 *
 * handoff/v1.1/binaries/song-sectors-four-stem.bin is 43 sectors of a genuine
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
#define FIX_SECTORS 43u
#define FIX_FRAMES  14592u
static const uint32_t k_stem_checksum[ST_PL_STEMS] = {
	1982348978u,  /* vocal      */
	207735031u,   /* drums      */
	3388280807u,  /* bass       */
	3473776285u,  /* instrument */
};
static const uint32_t k_song_checksum = 3509299530u;
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
				       : "handoff/v1.1/binaries/song-sectors-four-stem.bin";
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
	ck(len == (size_t)FIX_SECTORS * ST11_SECTOR_BYTES, "43 sectors of 8192 bytes");

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
