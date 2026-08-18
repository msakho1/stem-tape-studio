/*
 * test_stem_v11.c — Stem Tape v1.1 migration: byte-for-byte, sample-for-
 * sample conformance against the frozen Lovable handoff/v1.1/ fixtures.
 * Per the migration directive: prove the firmware C encoder/decoder
 * matches the COMMITTED companion binary fixtures -- never substitute
 * self-generated "expected" bytes.
 *
 *     cc -std=c11 -Wall -Wextra -I../src \
 *        ../src/st_crc32.c ../src/st_checksum32.c ../src/st_sector_v11.c \
 *        test_stem_v11.c -o test_stem_v11 && \
 *        (cd ../../.. && firmware/stemtape_player/tests/test_stem_v11)
 *
 * Must be run with the CURRENT WORKING DIRECTORY at the repository root
 * (matches this project's CI convention: every workflow step runs with
 * working-directory ${{ github.workspace }}/project) so the relative
 * handoff/v1.1/binaries/ paths below resolve.
 *
 * Same self-checking pattern as test_stemtape_player.c: [OK]/[FAIL] per
 * assertion, nonzero exit on any failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "st_checksum32.h"
#include "st_crc32.h"
#include "st_sector_v11.h"
#include "st_transfer_protocol.h" /* ST_CRC32_INIT */
#include "st_v11_format.h"

static int g_checks;
static int g_failures;
static int g_test_cases;

#define CHECK(cond, ...) do { \
		g_checks++; \
		if (cond) { \
			printf("[OK  ] " __VA_ARGS__); \
			printf("\n"); \
		} else { \
			g_failures++; \
			printf("[FAIL] " __VA_ARGS__); \
			printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)

#define RUN(fn) do { g_test_cases++; fn(); } while (0)

/* Reads an entire fixture file into a malloc'd buffer. Aborts the whole
 * test binary (not just one CHECK) on failure -- a missing/short fixture
 * means the handoff bundle itself is broken, not a normal test failure. */
static uint8_t *read_fixture(const char *path, size_t *len_out)
{
	FILE *f = fopen(path, "rb");

	if (!f) {
		fprintf(stderr, "FATAL: could not open fixture %s (run from the repo root?)\n", path);
		exit(2);
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fprintf(stderr, "FATAL: fseek failed on %s\n", path);
		exit(2);
	}
	long sz = ftell(f);

	if (sz < 0) {
		fprintf(stderr, "FATAL: ftell failed on %s\n", path);
		exit(2);
	}
	rewind(f);
	uint8_t *buf = malloc((size_t)sz);

	if (!buf) {
		fprintf(stderr, "FATAL: out of memory reading %s\n", path);
		exit(2);
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		fprintf(stderr, "FATAL: short read on %s\n", path);
		exit(2);
	}
	fclose(f);
	*len_out = (size_t)sz;
	return buf;
}

/* ========================================================================
 * STSC sector codec vs handoff/v1.1/binaries/song-sectors-four-stem.bin
 *
 * Ground truth (handoff/v1.1/decoded/song-sectors-four-stem.json, verified
 * SHA-256/CRC32 against handoff/v1.1/SHA256SUMS.txt + CRC32SUMS.txt before
 * being frozen into this repo):
 *   frames=14592, sectorCount=43, sampleRate=48000, channels=2, pcmDepth=24
 *   stems (name, originalFrames, checksum):
 *     vocal      14592  1982348978
 *     drums      14150   207735031
 *     bass       14000  3388280807
 *     instrument 12000  3473776285
 *   songChecksum 3509299530
 *
 * The per-stem/song "checksum" fields are FNV-1a (st_checksum32.h), NOT
 * CRC-32 -- confirmed empirically while building this test (a CRC-32
 * attempt over the same byte ranges did not match any of the four
 * declared values; FNV-1a over each stem's FULL shared-length decoded
 * PCM24 stream matched all four exactly, and FNV-1a over the 16-byte
 * little-endian digest of those four checksums matched songChecksum
 * exactly) -- see src/sp1/song.ts's checksum32()/assertCanonicalSong()
 * in the original handoff for the companion-side source of this rule.
 * ======================================================================== */

#define SONG_FRAMES 14592u
#define SONG_SECTOR_COUNT 43u

static void test_song_sectors_fixture(void)
{
	size_t len;
	uint8_t *data = read_fixture("handoff/v1.1/binaries/song-sectors-four-stem.bin", &len);

	CHECK(len == (size_t)SONG_SECTOR_COUNT * ST11_SECTOR_BYTES,
	      "song-sectors-four-stem.bin is exactly 43 * 8192 = 352256 bytes");

	/* Per-stem, full shared-length (frames=14592) decoded PCM24, matching
	 * CanonicalStem.pcm24's own length convention (padded, not truncated
	 * to originalFrames). */
	static uint8_t stem_pcm[ST11_STEM_COUNT][SONG_FRAMES * ST11_STEM_FRAME_BYTES];
	uint32_t decoded_frames = 0;
	uint32_t sector_index;
	bool header_ok_all = true;
	bool sector_index_sequential = true;
	bool first_frame_correct = true;

	for (sector_index = 0; sector_index < SONG_SECTOR_COUNT; sector_index++) {
		const uint8_t *sector = data + (size_t)sector_index * ST11_SECTOR_BYTES;
		st11_sector_header_t h;

		if (!st11_sector_read_header(sector, &h)) {
			header_ok_all = false;
			continue;
		}
		if (h.sector_index != sector_index) {
			sector_index_sequential = false;
		}
		if (h.first_frame != sector_index * ST11_FRAMES_PER_SECTOR) {
			first_frame_correct = false;
		}

		/* Exact byte-level proof for sector 0's header fields, hand-
		 * decoded from the raw fixture bytes independently of this
		 * codec (od/python) before this test was written. */
		if (sector_index == 0) {
			CHECK(h.sector_index == 0u, "sector 0: sectorIndex == 0");
			CHECK(h.first_frame == 0u, "sector 0: firstFrame == 0");
			CHECK(h.frame_count == ST11_FRAMES_PER_SECTOR,
			      "sector 0: frameCount == 340 (a full, non-final sector)");
			CHECK(h.bpm_q8 == 24576u, "sector 0: bpmQ8 == 24576 (96.0 BPM in Q8)");
			CHECK(h.downbeat_frame == 12000u, "sector 0: downbeatFrame == 12000");
		}
		/* Sector 42 (the last, index 42) is the short/final sector:
		 * 14592 - 42*340 = 312 frames. */
		if (sector_index == SONG_SECTOR_COUNT - 1) {
			CHECK(h.frame_count == SONG_FRAMES - (SONG_SECTOR_COUNT - 1) * ST11_FRAMES_PER_SECTOR,
			      "sector 42 (final): frameCount == 312 (14592 - 42*340)");
		}

		uint32_t f;

		for (f = 0; f < h.frame_count; f++) {
			st11_audio_frame_t frame;
			uint32_t abs_frame = h.first_frame + f;
			uint32_t s;

			st11_sector_decode_frame(sector, f, &frame);
			for (s = 0; s < ST11_STEM_COUNT; s++) {
				uint8_t *dst = &stem_pcm[s][(size_t)abs_frame * ST11_STEM_FRAME_BYTES];
				int32_t l = frame.stem_l[s];
				int32_t r = frame.stem_r[s];

				/* Re-encode the decoded sample back to raw LE
				 * bytes for the checksum -- this proves the
				 * decode is lossless/reversible, not just
				 * "some bytes came out". */
				dst[0] = (uint8_t)(l & 0xff);
				dst[1] = (uint8_t)((l >> 8) & 0xff);
				dst[2] = (uint8_t)((l >> 16) & 0xff);
				dst[3] = (uint8_t)(r & 0xff);
				dst[4] = (uint8_t)((r >> 8) & 0xff);
				dst[5] = (uint8_t)((r >> 16) & 0xff);
			}
		}
		decoded_frames += h.frame_count;
	}

	CHECK(header_ok_all, "every sector's magic reads back as ST11_SECTOR_MAGIC ('STSC')");
	CHECK(sector_index_sequential, "every sector's sectorIndex matches its physical position");
	CHECK(first_frame_correct, "every sector's firstFrame == sectorIndex * 340");
	CHECK(decoded_frames == SONG_FRAMES, "sum of all sectors' frameCount == 14592 (the declared song length)");

	static const char *const stem_names[ST11_STEM_COUNT] = { "vocal", "drums", "bass", "instrument" };
	static const uint32_t declared_stem_checksum[ST11_STEM_COUNT] = {
		1982348978u, 207735031u, 3388280807u, 3473776285u,
	};
	uint8_t digest[ST11_STEM_COUNT * 4];
	uint32_t s;

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		uint32_t sum = st_checksum32_compute(stem_pcm[s], sizeof(stem_pcm[s]));

		CHECK(sum == declared_stem_checksum[s],
		      "stem %s: FNV-1a(full decoded PCM24) matches the companion's declared checksum %u",
		      stem_names[s], declared_stem_checksum[s]);
		digest[s * 4 + 0] = (uint8_t)(declared_stem_checksum[s] & 0xff);
		digest[s * 4 + 1] = (uint8_t)((declared_stem_checksum[s] >> 8) & 0xff);
		digest[s * 4 + 2] = (uint8_t)((declared_stem_checksum[s] >> 16) & 0xff);
		digest[s * 4 + 3] = (uint8_t)((declared_stem_checksum[s] >> 24) & 0xff);
	}

	uint32_t song_sum = st_checksum32_compute(digest, sizeof(digest));

	CHECK(song_sum == 3509299530u,
	      "song checksum: FNV-1a(16-byte digest of the 4 stem checksums) == 3509299530");

	free(data);
}

/* ========================================================================
 * STIX v2 index record integrity CRC vs handoff/v1.1/binaries/index-a-valid.bin
 *
 * Proves the OTHER checksum algorithm the v1.1 contract uses: CRC-32 IEEE
 * 802.3 (st_crc32.c, already used elsewhere in this codebase) over the
 * record's own bytes [0,252) with the validity-magic bytes [0,4)
 * normalized to zero -- this is the record's self-integrity check, a
 * DIFFERENT thing from the FNV-1a content checksums proven above. Full
 * STIX field parsing/validation is a later migration commit; this proves
 * only the CRC rule in isolation, against a real fixture, before any
 * parser is built on top of it.
 * ======================================================================== */
static void test_index_record_crc_fixture(void)
{
	size_t len;
	uint8_t *rec = read_fixture("handoff/v1.1/binaries/index-a-valid.bin", &len);

	CHECK(len == 512, "index-a-valid.bin is exactly one 512-byte physical block");

	uint32_t magic = (uint32_t)rec[0] | ((uint32_t)rec[1] << 8) | ((uint32_t)rec[2] << 16) |
			  ((uint32_t)rec[3] << 24);

	CHECK(magic == ST11_INDEX_MAGIC, "index-a-valid.bin: magic == ST11_INDEX_MAGIC ('STIX')");

	uint32_t declared_crc = (uint32_t)rec[252] | ((uint32_t)rec[253] << 8) |
				  ((uint32_t)rec[254] << 16) | ((uint32_t)rec[255] << 24);

	/* CRC over [0,252) with [0,4) (the magic) zeroed -- computed in two
	 * streaming pieces so no scratch buffer is needed: 4 zero bytes,
	 * then the real bytes [4,252). */
	static const uint8_t zero4[4] = { 0, 0, 0, 0 };
	uint32_t running = st_crc32_update(ST_CRC32_INIT, zero4, sizeof(zero4));

	running = st_crc32_update(running, rec + 4, ST11_IX_CRC_RANGE_TO - 4);
	uint32_t computed_crc = running ^ 0xFFFFFFFFu;

	CHECK(computed_crc == declared_crc,
	      "index-a-valid.bin: CRC-32(record[0,252) with magic zeroed) matches its own crc32 field");

	bool tail_zero = true;
	size_t i;

	for (i = ST11_INDEX_RECORD_BYTES; i < 512; i++) {
		if (rec[i] != 0) {
			tail_zero = false;
			break;
		}
	}
	CHECK(tail_zero, "index-a-valid.bin: bytes [256,512) past the 256-byte record are zero");

	free(rec);
}

int main(void)
{
	RUN(test_song_sectors_fixture);
	RUN(test_index_record_crc_fixture);

	printf("\n");
	printf("%d distinct test cases, %d assertion checks\n", g_test_cases, g_checks);
	if (g_failures) {
		printf("STEM V1.1 FIXTURE CONFORMANCE FAILED (%d of %d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("STEM V1.1 FIXTURE CONFORMANCE PASSED (%d test cases, %d checks)\n", g_test_cases, g_checks);
	return 0;
}
