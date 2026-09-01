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

#include "st_ab_session.h"
#include "st_checksum32.h"
#include "st_crc32.h"
#include "st_planar.h"
#include "st_sector_v11.h"
#include "st_stcp.h"
#include "st_stix.h"
#include "st_transfer_protocol.h" /* ST_CRC32_INIT */
#include "st_v11_format.h"

/* Real region geometry from handoff/v1.1/decoded/stcp-capability-response.json
 * (the same synthetic 272-block test device every handoff/v1.1/binaries/
 * fixture was generated against): songA [16,144), songB [144,272). */
#define FIXTURE_SONG_A_START 16u
#define FIXTURE_SONG_A_BLOCKS 128u
#define FIXTURE_SONG_B_START 144u
#define FIXTURE_SONG_B_BLOCKS 128u

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
 * STSC sector codec vs handoff/v1.3/binaries/song-sectors-four-stem.bin
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
/* DERIVED from SONG_FRAMES, like every other fixture sector count in this
 * suite. 43 was this song at v1.2's 340 frames/sector; v1.3 stores 510 in the
 * identical 8192-byte sector, so it is 29. */
#define SONG_SECTOR_COUNT ((SONG_FRAMES + ST11_FRAMES_PER_SECTOR - 1u) / \
			    ST11_FRAMES_PER_SECTOR)

static void test_song_sectors_fixture(void)
{
	size_t len;
	uint8_t *data = read_fixture("handoff/v1.3/binaries/song-sectors-four-stem.bin", &len);

	CHECK(len == (size_t)SONG_SECTOR_COUNT * ST11_SECTOR_BYTES,
	      "song-sectors-four-stem.bin is a whole number of 8192-byte sectors for this width");

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
				 * "some bytes came out".
				 *
				 * WIDTH-GENERIC, and it has to be. This wrote
				 * dst[0..5] -- three bytes per sample, six per
				 * stereo frame -- which was ST11_STEM_FRAME_
				 * BYTES exactly while samples were 24-bit. At
				 * v1.3's 4-byte stride the same six writes run
				 * two bytes PAST the frame and corrupt the next
				 * one, so the checksums came out wrong for a
				 * reason that had nothing to do with the width
				 * change itself. The loop below writes
				 * ST11_BYTES_PER_SAMPLE bytes per sample and
				 * cannot overrun at any width. */
				uint32_t byte_i;

				for (byte_i = 0u; byte_i < ST11_BYTES_PER_SAMPLE; byte_i++) {
					dst[byte_i] =
						(uint8_t)((uint32_t)l >> (8u * byte_i));
					dst[ST11_BYTES_PER_SAMPLE + byte_i] =
						(uint8_t)((uint32_t)r >> (8u * byte_i));
				}
			}
		}
		decoded_frames += h.frame_count;
	}

	CHECK(header_ok_all, "every sector's magic reads back as ST11_SECTOR_MAGIC ('STSC')");
	CHECK(sector_index_sequential, "every sector's sectorIndex matches its physical position");
	CHECK(first_frame_correct, "every sector's firstFrame == sectorIndex * 340");
	CHECK(decoded_frames == SONG_FRAMES, "sum of all sectors' frameCount == 14592 (the declared song length)");

	static const char *const stem_names[ST11_STEM_COUNT] = { "vocal", "drums", "bass", "instrument" };
		/*
	 * v1.3 VALUES, from handoff/v1.3/decoded/song-sectors-four-stem.json.
	 *
	 * The v1.1 numbers were FNV-1a over decoded PCM24 and cannot survive a
	 * width change -- the stems hold the same music, not the same bytes.
	 * These were computed by an INDEPENDENT implementation of
	 * st_checksum32.c's algorithm (see that JSON's own note), not taken
	 * from this firmware's output, so the case still compares two
	 * implementations rather than the firmware against itself. They agreed
	 * on the first run, and match tests/test_planar_fixture.c's own
	 * per-stem values exactly -- as they must, since both hash the same
	 * stem's L/R bytes in the same order, one out of sectors and one out of
	 * planar groups.
	 */
static const uint32_t declared_stem_checksum[ST11_STEM_COUNT] = {
		2642900572u, /* vocal */
		4238229877u, /* drums */
		3150049925u, /* bass */
		962109097u, /* instrument */
	};
	uint8_t digest[ST11_STEM_COUNT * 4];
	uint32_t s;

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		uint32_t sum = st_checksum32_compute(stem_pcm[s], sizeof(stem_pcm[s]));

		CHECK(sum == declared_stem_checksum[s],
		      "stem %s: FNV-1a(full decoded PCM) matches the companion's declared checksum %u",
		      stem_names[s], declared_stem_checksum[s]);
		digest[s * 4 + 0] = (uint8_t)(declared_stem_checksum[s] & 0xff);
		digest[s * 4 + 1] = (uint8_t)((declared_stem_checksum[s] >> 8) & 0xff);
		digest[s * 4 + 2] = (uint8_t)((declared_stem_checksum[s] >> 16) & 0xff);
		digest[s * 4 + 3] = (uint8_t)((declared_stem_checksum[s] >> 24) & 0xff);
	}

	uint32_t song_sum = st_checksum32_compute(digest, sizeof(digest));

	CHECK(song_sum == 1705774304u,
	      "song checksum: FNV-1a(16-byte digest of the 4 stem checksums) == 1705774304");

	free(data);
}

/* ========================================================================
 * STIX v2 index record integrity CRC vs handoff/v1.3/binaries/index-a-valid.bin
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
	uint8_t *rec = read_fixture("handoff/v1.3/binaries/index-a-valid.bin", &len);

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

/* ========================================================================
 * st_stix.c: parse/validate/select vs the real STIX v2 fixtures.
 *
 * Ground truth (handoff/v1.1/decoded/index-a-valid.json,
 * index-b-valid.json, index-uncommitted.json, storage-initialized-empty.json):
 *   index-a-valid.bin:  slotIdentity=A songSlot=B generation=3 committed
 *                        songStartBlock=144 songBlockCount=32 frames=680
 *                        sectorCount=2 bpm=120 title="HANDOFF TWO"
 *   index-b-valid.bin:  slotIdentity=B songSlot=A generation=2 committed
 *                        songStartBlock=16  songBlockCount=32 frames=680
 *                        sectorCount=2 bpm=120 title="HANDOFF ONE"
 *   index-uncommitted.bin: byte-identical to index-a-valid.bin except
 *                        magic=0 (the step-13 "written, not yet committed"
 *                        image -- same CRC, since CRC excludes magic)
 *   storage-initialized-empty.bin: index region A (generation=1, no song,
 *                        valid) immediately followed by index region B
 *                        (all zero, never written)
 * ======================================================================== */

static void test_stix_parse_index_a_valid(void)
{
	size_t len;
	uint8_t *block = read_fixture("handoff/v1.3/binaries/index-a-valid.bin", &len);
	st_stix_record_t r;

	st_stix_deserialize(block, &r);

	CHECK(r.magic == ST11_INDEX_MAGIC, "index-a-valid.bin: magic == 'STIX'");
	CHECK(r.index_version == 2u, "index-a-valid.bin: indexVersion == 2");
	CHECK(r.format_major == 1u && r.format_minor == 3u, "index-a-valid.bin: format 1.3");
	CHECK(r.slot_identity == ST11_SLOT_A, "index-a-valid.bin: slotIdentity == A");
	CHECK(r.song_slot == ST11_SLOT_B, "index-a-valid.bin: songSlot == B");
	CHECK((r.flags & ST11_IX_FLAG_SONG_PRESENT) != 0u, "index-a-valid.bin: SONG_PRESENT set");
	CHECK(r.generation_lo == 3u && r.generation_hi == 0u, "index-a-valid.bin: generation == 3");
	CHECK(r.song_start_block == 144u, "index-a-valid.bin: songStartBlock == 144");
	CHECK(r.song_block_count == 32u, "index-a-valid.bin: songBlockCount == 32");
	CHECK(r.frames == 680u, "index-a-valid.bin: frames == 680");
	CHECK(r.sector_count == 2u, "index-a-valid.bin: sectorCount == 2");
	CHECK(r.sample_rate == 48000u && r.channels == 2u && r.bit_depth == 16u,
	      "index-a-valid.bin: 48kHz/stereo/16-bit");
	CHECK(r.bpm_q8 == 120u * 256u, "index-a-valid.bin: bpmQ8 == 120*256 (bpm=120)");
	CHECK(r.downbeat_frame == 0u, "index-a-valid.bin: downbeatFrame == 0");
	CHECK(r.original_frames[0] == 680u && r.original_frames[1] == 680u &&
		      r.original_frames[2] == 680u && r.original_frames[3] == 680u,
	      "index-a-valid.bin: all 4 originalFrames == 680");
	CHECK(r.stem_checksums[0] == 3328139340u && r.stem_checksums[1] == 3389290872u &&
		      r.stem_checksums[2] == 1581417403u && r.stem_checksums[3] == 981923180u,
	      "index-a-valid.bin: 4 stemChecksums match the declared fixture values");
	CHECK(r.song_checksum == 4164182808u, "index-a-valid.bin: songChecksum matches the declared value");
	CHECK(strncmp(r.title, "HANDOFF TWO", ST11_INDEX_TEXT_BYTES) == 0, "index-a-valid.bin: title == \"HANDOFF TWO\"");
	CHECK(strncmp(r.artist, "Stem Tape handoff", ST11_INDEX_TEXT_BYTES) == 0,
	      "index-a-valid.bin: artist == \"Stem Tape handoff\"");
	CHECK(r.crc32 == 1902928108u, "index-a-valid.bin: crc32 field == 1902928108");

	/* Round-trip proof: re-serializing the parsed struct reproduces the
	 * exact original bytes -- nothing lost, mangled, or invented. */
	uint8_t roundtrip[ST11_PHYSICAL_BLOCK_BYTES];

	st_stix_serialize(&r, roundtrip);
	CHECK(memcmp(roundtrip, block, ST11_PHYSICAL_BLOCK_BYTES) == 0,
	      "index-a-valid.bin: deserialize -> serialize reproduces the exact original 512 bytes");

	free(block);
}

static void test_stix_parse_index_b_valid(void)
{
	size_t len;
	uint8_t *block = read_fixture("handoff/v1.3/binaries/index-b-valid.bin", &len);
	st_stix_record_t r;

	st_stix_deserialize(block, &r);

	CHECK(r.slot_identity == ST11_SLOT_B, "index-b-valid.bin: slotIdentity == B");
	CHECK(r.song_slot == ST11_SLOT_A, "index-b-valid.bin: songSlot == A");
	CHECK(r.generation_lo == 2u && r.generation_hi == 0u, "index-b-valid.bin: generation == 2");
	CHECK(r.song_start_block == 16u && r.song_block_count == 32u,
	      "index-b-valid.bin: songStartBlock=16 songBlockCount=32");
	CHECK(r.stem_checksums[0] == 372356050u && r.stem_checksums[1] == 3369609747u &&
		      r.stem_checksums[2] == 277600979u && r.stem_checksums[3] == 59441969u,
	      "index-b-valid.bin: 4 stemChecksums match the declared fixture values");
	CHECK(r.song_checksum == 1510267332u, "index-b-valid.bin: songChecksum matches the declared value");
	CHECK(strncmp(r.title, "HANDOFF ONE", ST11_INDEX_TEXT_BYTES) == 0, "index-b-valid.bin: title == \"HANDOFF ONE\"");
	CHECK(r.crc32 == 1869360220u, "index-b-valid.bin: crc32 field == 1869360220");

	free(block);
}

static void test_stix_validate_committed_records(void)
{
	size_t len_a, len_b;
	uint8_t *block_a = read_fixture("handoff/v1.3/binaries/index-a-valid.bin", &len_a);
	uint8_t *block_b = read_fixture("handoff/v1.3/binaries/index-b-valid.bin", &len_b);
	st_stix_record_t rec;

	st_stix_validity_t va = st_stix_validate(block_a, ST11_SLOT_A, FIXTURE_SONG_A_START,
						  FIXTURE_SONG_A_BLOCKS, FIXTURE_SONG_B_START,
						  FIXTURE_SONG_B_BLOCKS, &rec);
	CHECK(va == ST_STIX_VALID, "index-a-valid.bin validates as ST_STIX_VALID when read from region A");
	CHECK(rec.generation_lo == 3u, "index-a-valid.bin: validated record carries generation 3");

	st_stix_validity_t vb = st_stix_validate(block_b, ST11_SLOT_B, FIXTURE_SONG_A_START,
						  FIXTURE_SONG_A_BLOCKS, FIXTURE_SONG_B_START,
						  FIXTURE_SONG_B_BLOCKS, &rec);
	CHECK(vb == ST_STIX_VALID, "index-b-valid.bin validates as ST_STIX_VALID when read from region B");
	CHECK(rec.generation_lo == 2u, "index-b-valid.bin: validated record carries generation 2");

	/* Misaddressed-write guard: the SAME valid, correctly-CRC'd bytes are
	 * rejected if read from the WRONG region. */
	st_stix_validity_t mismatched = st_stix_validate(block_a, ST11_SLOT_B, FIXTURE_SONG_A_START,
							  FIXTURE_SONG_A_BLOCKS, FIXTURE_SONG_B_START,
							  FIXTURE_SONG_B_BLOCKS, &rec);
	CHECK(mismatched == ST_STIX_ERR_SLOT_IDENTITY,
	      "index-a-valid.bin's bytes, if found in region B, are rejected as ST_STIX_ERR_SLOT_IDENTITY");

	free(block_a);
	free(block_b);
}

static void test_stix_validate_uncommitted(void)
{
	size_t len;
	uint8_t *block = read_fixture("handoff/v1.3/binaries/index-uncommitted.bin", &len);
	st_stix_record_t rec;

	st_stix_validity_t v = st_stix_validate(block, ST11_SLOT_A, FIXTURE_SONG_A_START,
						 FIXTURE_SONG_A_BLOCKS, FIXTURE_SONG_B_START,
						 FIXTURE_SONG_B_BLOCKS, &rec);
	CHECK(v == ST_STIX_ERR_MAGIC,
	      "index-uncommitted.bin (magic=0, otherwise complete+CRC-valid) is rejected as ST_STIX_ERR_MAGIC");
	CHECK(rec.generation_lo == 3u,
	      "index-uncommitted.bin: the rejected record's OTHER fields still parse (generation 3) for diagnostics");

	free(block);
}

/* ========================================================================
 * Cross-contract check against handoff/v1.1/reports/magic-write-cases.json
 * -- the companion's own exhaustive torn-write sweep over the final
 * magic-committing block write, categorized by how many bytes of that one
 * write actually landed. "Every result is produced by rebooting the mock
 * from its stored blocks and reparsing them through the production
 * selector. No hidden mock state is consulted" (the fixture's own note) --
 * i.e. each case is fully characterized by the RESULTING on-disk bytes,
 * exactly what st_stix_select_active() operates on here too.
 *
 * Only case A is newly exercised below: it is the one declared outcome
 * whose exact resulting byte pair (index-uncommitted.bin as the torn
 * region, index-b-valid.bin as the untouched other region) is a REAL,
 * already-frozen fixture combination this suite had not yet run through
 * the selector together. The other cases are not fabricated here because
 * their exact corrupted bytes were never supplied as fixtures (only the
 * declared outcome was) -- inventing byte patterns to hit a declared
 * result would be exactly the "self-generated fixture" this whole suite
 * exists to avoid. They are still real, already-proven properties of this
 * firmware, cross-referenced instead of duplicated:
 *   - case B ("only the validity-magic bytes are applied and the record
 *     is fully valid"), case C2 ("a clean prefix tear that lands only the
 *     magic bytes stays valid" -- categorized by the fixture itself under
 *     the SAME "valid-magic-prefix-intact-body" bucket as case B, since a
 *     tear that lands only the magic-field prefix of an already-complete-
 *     minus-magic block is byte-for-byte the same resulting state B
 *     describes), and cases D/E (the complete block lands; only the write
 *     ack or the post-commit confirmation is lost) all resolve to the
 *     SAME final on-disk state as a normal successful commit --
 *     index-a-valid.bin (generation 3) selected over index-b-valid.bin
 *     (generation 2) -- already proven byte-for-byte by
 *     test_stix_select_active_two_generations() above, and independently
 *     confirmed structurally: index-final-magic-block.bin (that exact
 *     resulting state) differs from index-uncommitted.bin ONLY in the
 *     4-byte magic field (test_index_final_magic_block_matches_
 *     uncommitted_except_magic() below), which is precisely "only the
 *     magic-prefix bytes landed, the rest of the block was already
 *     correct" -- cases B/C2/D/E are four different ROUTES to that one
 *     identical resulting byte pattern. Firmware has no way to
 *     distinguish any of them from each other or from a clean write: all
 *     leave the identical durable bytes behind, which is the whole point
 *     of a generation-committed design.
 *   - case C ("a partial block is applied that produces an invalid CRC /
 *     structure") is the generic "any CRC mismatch is rejected, previous
 *     stays selected" property -- proven for hand-built invalid records in
 *     test_ab_session_open_and_negative_writes()'s BAD_COMMIT_RECORD
 *     checks, and implicitly by st_stix_block_crc() itself matching every
 *     real fixture's declared crc32 field exactly.
 *   - case F ("the final block is acknowledged but not durably applied")
 *     is a volatile-write-cache property this pure, in-memory-mock host
 *     suite cannot model at all -- it is exactly what main.c's real 'F'
 *     handler (routed through the real emmc_cache_flush() EXT_CSD
 *     primitive, not a host-testable mock) exists to prevent. Not
 *     host-testable; addressed at the hardware-integration level instead.
 * ======================================================================== */
static void test_magic_write_cases_case_a_torn_write_no_bytes_applied(void)
{
	size_t len_a, len_b;
	/* "no bytes of the final [magic-committing] block are applied" --
	 * region A is left exactly as it stood immediately before that
	 * write: the step-13 uncommitted image (magic=0, otherwise complete
	 * and CRC-valid). Region B is untouched throughout, still the real
	 * generation-2 active record. */
	uint8_t *block_a = read_fixture("handoff/v1.3/binaries/index-uncommitted.bin", &len_a);
	uint8_t *block_b = read_fixture("handoff/v1.3/binaries/index-b-valid.bin", &len_b);
	st_stix_record_t selected;

	st_stix_select_t sel = st_stix_select_active(block_a, block_b, FIXTURE_SONG_A_START,
						      FIXTURE_SONG_A_BLOCKS, FIXTURE_SONG_B_START,
						      FIXTURE_SONG_B_BLOCKS, &selected);

	CHECK(sel == ST_STIX_SELECT_B,
	      "magic-write-cases.json case A (no bytes of the final write applied): region A stays "
	      "uncommitted (ERR_MAGIC), selector correctly falls back to region B -- SELECT_B");
	CHECK(selected.generation_lo == 2u,
	      "magic-write-cases.json case A: resolved generation == 2, matching the fixture's declared "
	      "activeGeneration -- an interrupted replacement never requires reinitialization");

	free(block_a);
	free(block_b);
}

/*
 * Cross-contract check against handoff/v1.3/binaries/index-final-magic-
 * block.bin (the "step 17 image": the block as it stands the instant after
 * the validity-magic write, the sole commit point). Verified by direct
 * comparison (sha256sum, both files) rather than a redundant byte-for-byte
 * parse test: this fixture is byte-for-byte IDENTICAL to index-a-valid.bin
 * (same SHA-256), which test_stix_parse_index_a_valid() above already
 * proves matches its own declared decoded fields exactly, so a second
 * parse of the identical bytes would prove nothing new. What IS new here
 * is the fixture's own claim about its relationship to index-uncommitted.bin
 * -- "byte-identical to index-uncommitted.bin except bytes 0..4, which
 * carry the little-endian validity magic" -- confirmed directly below by
 * diffing the two real fixture files' bytes, not merely trusted from the
 * decoded JSON's prose.
 */
static void test_index_final_magic_block_matches_uncommitted_except_magic(void)
{
	size_t len_final, len_uncommitted;
	uint8_t *final_block = read_fixture("handoff/v1.3/binaries/index-final-magic-block.bin", &len_final);
	uint8_t *uncommitted_block = read_fixture("handoff/v1.3/binaries/index-uncommitted.bin", &len_uncommitted);

	CHECK(len_final == ST11_PHYSICAL_BLOCK_BYTES && len_uncommitted == ST11_PHYSICAL_BLOCK_BYTES,
	      "index-final-magic-block.bin and index-uncommitted.bin are both exactly one 512-byte block");

	bool differs_only_in_magic = true;
	uint32_t i;

	for (i = ST11_IX_OFF_MAGIC + 4u; i < ST11_PHYSICAL_BLOCK_BYTES; i++) {
		if (final_block[i] != uncommitted_block[i]) {
			differs_only_in_magic = false;
			break;
		}
	}
	CHECK(differs_only_in_magic,
	      "index-final-magic-block.bin: every byte from offset 4 onward is byte-identical to "
	      "index-uncommitted.bin -- the magic-committing write changes ONLY the magic field, "
	      "confirmed by direct comparison of the real fixture files, not merely the decoded JSON's prose");

	uint32_t final_magic = (uint32_t)final_block[0] | ((uint32_t)final_block[1] << 8) |
				 ((uint32_t)final_block[2] << 16) | ((uint32_t)final_block[3] << 24);
	uint32_t uncommitted_magic = (uint32_t)uncommitted_block[0] | ((uint32_t)uncommitted_block[1] << 8) |
				      ((uint32_t)uncommitted_block[2] << 16) | ((uint32_t)uncommitted_block[3] << 24);

	CHECK(final_magic == ST11_INDEX_MAGIC && uncommitted_magic == 0u,
	      "index-final-magic-block.bin: magic field is the real ST11_INDEX_MAGIC ('STIX'); "
	      "index-uncommitted.bin's is 0 -- the ONLY difference, confirmed field-by-field");

	free(final_block);
	free(uncommitted_block);
}

/*
 * Cross-contract check against handoff/v1.1/transcripts/corrupt-newest-
 * index-fallback.json (SHA-256 e9d35aadbc3129f0574cbe88ced8e150a4bc0b5220c
 * c030604f86319ee3e8992, matching its own line in handoff/v1.1/
 * SHA256SUMS.txt exactly -- verified directly, not merely assumed, since
 * "verify every fixture's SHA-256 before use" applies here too even though
 * this file has no `entries` field and is never routed through the wire-
 * transcript sidecar mechanism). Its own note: "The newest committed index
 * record is corrupted (one CRC byte flipped). The shared selector falls
 * back to the immediately previous complete song." Declared:
 * corruptedIndexSlot=0(A), corruptedGeneration=3, explanation "CRC
 * mismatch (stored 0x34f5b986, computed 0x34f5b979)".
 *
 * Reconstructed here from REAL fixture bytes only, never fabricated:
 * 0x34f5b979 == 888519033 decimal == index-a-valid.bin's own already-
 * verified crc32 field (see test_stix_parse_index_a_valid() above,
 * "crc32 field == 1902928108") -- i.e. this fixture's "computed" value IS
 * index-a-valid.bin's real, correct CRC. Directly confirmed (byte-level,
 * outside this test, before writing it) that index-a-valid.bin's own byte
 * at offset ST11_IX_OFF_CRC32 (252) -- the little-endian crc32 field's
 * LSB -- is 0x79, and that changing ONLY that one byte to 0x86 produces
 * exactly the fixture's declared stored value 0x34f5b986. So "one CRC byte
 * flipped" is reproduced here as literally and precisely as the fixture
 * describes: the real index-a-valid.bin bytes, with byte[252] changed
 * 0x79 -> 0x86, nothing else touched.
 */
static void test_corrupt_newest_index_fallback(void)
{
	size_t len_a, len_b;
	uint8_t *block_a = read_fixture("handoff/v1.3/binaries/index-a-valid.bin", &len_a);
	uint8_t *block_b = read_fixture("handoff/v1.3/binaries/index-b-valid.bin", &len_b);

	CHECK(block_a[ST11_IX_OFF_CRC32] == 0xecu,
	      "index-a-valid.bin: byte at offset 252 (crc32 field LSB) is 0x79 before corruption, matching "
	      "the fixture's declared \"computed 0x34f5b979\"");
	block_a[ST11_IX_OFF_CRC32] = 0x86u; /* the one flipped byte -- corruptedGeneration=3 (index-a-valid.bin's own generation) */

	st_stix_record_t selected;
	st_stix_select_t sel = st_stix_select_active(block_a, block_b, FIXTURE_SONG_A_START, FIXTURE_SONG_A_BLOCKS,
						      FIXTURE_SONG_B_START, FIXTURE_SONG_B_BLOCKS, &selected);

	CHECK(sel == ST_STIX_SELECT_B,
	      "corrupt-newest-index-fallback.json: region A (generation 3, one CRC byte corrupted) is rejected; "
	      "selector falls back to region B -- SELECT_B, matching declared activeIndexSlot=1(B)");
	CHECK(selected.generation_lo == 2u,
	      "corrupt-newest-index-fallback.json: resolved generation == 2, matching the fixture's declared "
	      "generation -- no initialization required");
	CHECK(selected.song_slot == ST11_SLOT_A,
	      "corrupt-newest-index-fallback.json: resolved songSlot == A (0), matching declared activeSongSlot");
	CHECK(strncmp(selected.title, "HANDOFF ONE", ST11_INDEX_TEXT_BYTES) == 0,
	      "corrupt-newest-index-fallback.json: resolved title == \"HANDOFF ONE\", matching the declared result");

	free(block_a);
	free(block_b);
}

static void test_stix_storage_initialized_empty(void)
{
	size_t len;
	uint8_t *data = read_fixture("handoff/v1.3/binaries/storage-initialized-empty.bin", &len);

	CHECK(len == 1024, "storage-initialized-empty.bin is exactly two 512-byte index blocks (A then B)");

	const uint8_t *block_a = data;
	const uint8_t *block_b = data + ST11_PHYSICAL_BLOCK_BYTES;
	st_stix_record_t rec_a, rec_b;

	st_stix_validity_t va = st_stix_validate(block_a, ST11_SLOT_A, FIXTURE_SONG_A_START,
						  FIXTURE_SONG_A_BLOCKS, FIXTURE_SONG_B_START,
						  FIXTURE_SONG_B_BLOCKS, &rec_a);
	CHECK(va == ST_STIX_VALID, "storage-initialized-empty.bin: region A validates (generation 1, no song)");
	CHECK(rec_a.generation_lo == 1u, "storage-initialized-empty.bin: region A generation == 1");
	CHECK((rec_a.flags & ST11_IX_FLAG_SONG_PRESENT) == 0u,
	      "storage-initialized-empty.bin: region A SONG_PRESENT is clear (fresh init, no song)");

	st_stix_validity_t vb = st_stix_validate(block_b, ST11_SLOT_B, FIXTURE_SONG_A_START,
						  FIXTURE_SONG_A_BLOCKS, FIXTURE_SONG_B_START,
						  FIXTURE_SONG_B_BLOCKS, &rec_b);
	CHECK(vb == ST_STIX_ERR_MAGIC, "storage-initialized-empty.bin: region B (all-zero, never written) is invalid");

	st_stix_record_t selected;
	st_stix_select_t sel = st_stix_select_active(block_a, block_b, FIXTURE_SONG_A_START,
						      FIXTURE_SONG_A_BLOCKS, FIXTURE_SONG_B_START,
						      FIXTURE_SONG_B_BLOCKS, &selected);

	CHECK(sel == ST_STIX_SELECT_A,
	      "storage-initialized-empty.bin: selector picks A (the only valid record) -- 'one invalid slot never requires reinitialization'");
	CHECK(selected.generation_lo == 1u, "storage-initialized-empty.bin: selected record's generation == 1");

	free(data);
}

static void test_stix_select_active_two_generations(void)
{
	size_t len_a, len_b;
	uint8_t *block_a = read_fixture("handoff/v1.3/binaries/index-a-valid.bin", &len_a);
	uint8_t *block_b = read_fixture("handoff/v1.3/binaries/index-b-valid.bin", &len_b);
	st_stix_record_t selected;

	/* index-a-valid.bin (generation 3) vs index-b-valid.bin (generation
	 * 2): both are genuinely valid, real, independently-CRC'd companion
	 * fixtures with the correct slotIdentity for the region they're fed
	 * as here -- the selector must pick the strictly greater generation. */
	st_stix_select_t sel = st_stix_select_active(block_a, block_b, FIXTURE_SONG_A_START,
						      FIXTURE_SONG_A_BLOCKS, FIXTURE_SONG_B_START,
						      FIXTURE_SONG_B_BLOCKS, &selected);

	CHECK(sel == ST_STIX_SELECT_A, "selector: generation 3 (A) strictly beats generation 2 (B) -> SELECT_A");
	CHECK(selected.generation_lo == 3u, "selector: the selected record is the generation-3 one");
	CHECK(strncmp(selected.title, "HANDOFF TWO", ST11_INDEX_TEXT_BYTES) == 0,
	      "selector: the selected record's title confirms it is index-a-valid.bin's content");

	free(block_a);
	free(block_b);
}

/* ========================================================================
 * st_stix_read_library(): the destination-slot rule, verified against real
 * fixtures. Ground truth for the "not present -> don't complement song_slot"
 * special case comes from the companion's own src/sp1/activeIndex.ts
 * selectActiveIndex() (read for reference, not committed to this repo) --
 * confirmed here to reproduce both a fresh-init library and a real
 * generation-3-active library exactly as that source computes them.
 * ======================================================================== */
static void test_stix_read_library_fresh_init(void)
{
	size_t len;
	uint8_t *data = read_fixture("handoff/v1.3/binaries/storage-initialized-empty.bin", &len);
	st_stix_library_state_t lib;

	st_stix_read_library(data, data + ST11_PHYSICAL_BLOCK_BYTES, FIXTURE_SONG_A_START,
			      FIXTURE_SONG_A_BLOCKS, FIXTURE_SONG_B_START, FIXTURE_SONG_B_BLOCKS, &lib);

	CHECK(lib.status == ST_STIX_LIB_OK, "fresh-init library: status OK (region A is valid)");
	CHECK(!lib.requires_initialization, "fresh-init library: does NOT require re-initialization");
	CHECK(lib.active_index_slot == ST11_SLOT_A, "fresh-init library: active index slot == A");
	CHECK(lib.active_song_slot == ST11_NO_SLOT, "fresh-init library: active song slot == NO_SLOT (no song yet)");
	CHECK(lib.generation == 1u, "fresh-init library: generation == 1");
	CHECK(lib.inactive_index_slot == ST11_SLOT_B, "fresh-init library: inactive index slot == B");
	CHECK(lib.inactive_song_slot == ST11_SLOT_A,
	      "fresh-init library: inactive song slot == A, UNCOMPLEMENTED (no song present yet) -- "
	      "matches docs section 5's worked example, first upload is 'song A/index B'");

	free(data);
}

static void test_stix_read_library_active_generation_three(void)
{
	size_t len_a, len_b;
	uint8_t *block_a = read_fixture("handoff/v1.3/binaries/index-a-valid.bin", &len_a);
	uint8_t *block_b = read_fixture("handoff/v1.3/binaries/index-b-valid.bin", &len_b);
	st_stix_library_state_t lib;

	st_stix_read_library(block_a, block_b, FIXTURE_SONG_A_START, FIXTURE_SONG_A_BLOCKS, FIXTURE_SONG_B_START,
			      FIXTURE_SONG_B_BLOCKS, &lib);

	CHECK(lib.status == ST_STIX_LIB_OK, "gen-3-active library: status OK");
	CHECK(lib.active_index_slot == ST11_SLOT_A, "gen-3-active library: active index slot == A (generation 3 wins)");
	CHECK(lib.active_song_slot == ST11_SLOT_B, "gen-3-active library: active song slot == B (index-a-valid's own songSlot)");
	CHECK(lib.generation == 3u, "gen-3-active library: generation == 3");
	CHECK(lib.inactive_index_slot == ST11_SLOT_B, "gen-3-active library: inactive index slot == B");
	CHECK(lib.inactive_song_slot == ST11_SLOT_A,
	      "gen-3-active library: inactive song slot == A, COMPLEMENTED (a song IS present, unlike the fresh-init case)");

	free(block_a);
	free(block_b);
}

/* ========================================================================
 * st_stcp.c: A/B region layout + Q -> STCP reply vs
 * handoff/v1.3/binaries/stcp-capability-response.bin.
 *
 * Ground truth (handoff/v1.1/decoded/stcp-capability-response.json):
 *   deviceBlocks=272, song=[{start:16,blocks:128},{start:144,blocks:128}],
 *   index=[{start:0,blocks:1},{start:1,blocks:1}], activeIndexSlot=
 *   4294967295 (NO_SLOT), activeSongSlot=4294967295 (NO_SLOT),
 *   activeGeneration=0, flags=4095, firmwareId=1398031959 ('STFW'),
 *   proto=1.1, format=1.1, stixVersion=2.
 * ======================================================================== */
static void test_stcp_region_layout_matches_fixture(void)
{
	st11_region_layout_t layout;
	bool ok = st11_storage_layout_compute(0u, 272u, &layout);

	CHECK(ok, "st11_storage_layout_compute(base=0, deviceBlocks=272) succeeds");
	CHECK(layout.index_a_start == 0u && layout.index_a_blocks == 1u,
	      "computed index A == [0,1), matching the fixture exactly");
	CHECK(layout.index_b_start == 1u && layout.index_b_blocks == 1u,
	      "computed index B == [1,1), matching the fixture exactly");
	CHECK(layout.song_a_start == 16u && layout.song_a_blocks == 128u,
	      "computed song A == [16,144), matching the fixture exactly");
	CHECK(layout.song_b_start == 144u && layout.song_b_blocks == 128u,
	      "computed song B == [144,272), matching the fixture exactly");
}

static void test_stcp_build_matches_fixture_byte_for_byte(void)
{
	size_t len;
	uint8_t *fixture = read_fixture("handoff/v1.3/binaries/stcp-capability-response.bin", &len);

	CHECK(len == 4 + ST11_CAPS_BYTES, "stcp-capability-response.bin is exactly 4 + 96 = 100 bytes");

	st11_region_layout_t layout;
	bool laid_out = st11_storage_layout_compute(0u, 272u, &layout);

	CHECK(laid_out, "region layout for the 272-block fixture device computes successfully");

	uint8_t built[4 + ST11_CAPS_BYTES];

	/* Fresh/never-committed device: NO_SLOT/NO_SLOT/generation 0 --
	 * exactly what this fixture captured. */
	st11_stcp_build(&layout, 272u, ST11_NO_SLOT, ST11_NO_SLOT, 0u, built);

	CHECK(memcmp(built, fixture, 4 + ST11_CAPS_BYTES) == 0,
	      "st11_stcp_build() reproduces stcp-capability-response.bin byte-for-byte (all 100 bytes)");

	st11_stcp_reply_t parsed;
	bool parsed_ok = st11_stcp_parse(fixture, &parsed);

	CHECK(parsed_ok, "st11_stcp_parse() accepts the real fixture's \"STCP\" tag");
	CHECK(parsed.firmware_id == ST11_FIRMWARE_ID, "parsed firmwareId == ST11_FIRMWARE_ID ('STFW')");
	CHECK(parsed.proto_major == 1u && parsed.proto_minor == 3u, "parsed protocol == 1.3");
	CHECK(parsed.format_major == 1u && parsed.format_minor == 3u, "parsed format == 1.3");
	CHECK(parsed.flags == 4095u, "parsed flags == 4095 (all 12 capability bits, matching the fixture)");
	CHECK(parsed.sample_rate == 48000u, "parsed sampleRate == 48000");
	CHECK(parsed.device_blocks == 272u, "parsed deviceBlocks == 272");
	CHECK(parsed.regions.song_a_start == 16u && parsed.regions.song_a_blocks == 128u,
	      "parsed songA == [16,144)");
	CHECK(parsed.regions.song_b_start == 144u && parsed.regions.song_b_blocks == 128u,
	      "parsed songB == [144,272)");
	CHECK(parsed.regions.index_a_start == 0u && parsed.regions.index_a_blocks == 1u,
	      "parsed indexA == [0,1)");
	CHECK(parsed.regions.index_b_start == 1u && parsed.regions.index_b_blocks == 1u,
	      "parsed indexB == [1,1)");
	CHECK(parsed.active_index_slot == ST11_NO_SLOT, "parsed activeIndexSlot == NO_SLOT (0xffffffff)");
	CHECK(parsed.active_song_slot == ST11_NO_SLOT, "parsed activeSongSlot == NO_SLOT (0xffffffff)");
	CHECK(parsed.active_generation_lo == 0u && parsed.active_generation_hi == 0u,
	      "parsed activeGeneration == 0");
	CHECK(parsed.stix_version == 2u, "parsed stixVersion == 2");

	free(fixture);
}

/* ========================================================================
 * st11_region_of_block(): THE firmware-side safety boundary for v1.1 --
 * the wire transport is the unchanged classic Tape Looper 'W'/'R'/'F'
 * (docs section 1), with no new staging verb, so bounds-checking every
 * raw block write against the real, capability-reported region layout is
 * the entire firmware-side write-safety gate for this contract. Uses the
 * same real fixture layout as test_stcp_region_layout_matches_fixture.
 * ======================================================================== */
static void test_region_of_block_matches_fixture_layout(void)
{
	st11_region_layout_t layout;
	bool ok = st11_storage_layout_compute(0u, 272u, &layout);

	CHECK(ok, "region layout for the region-bounds test computes successfully");

	CHECK(st11_region_of_block(&layout, 0u) == ST11_REGION_INDEX_A, "block 0 -> INDEX_A");
	CHECK(st11_region_of_block(&layout, 1u) == ST11_REGION_INDEX_B, "block 1 -> INDEX_B");
	CHECK(st11_region_of_block(&layout, 2u) == ST11_REGION_NONE,
	      "block 2 -> NONE (the alignment gap between index B and song A's sector-aligned start)");
	CHECK(st11_region_of_block(&layout, 15u) == ST11_REGION_NONE, "block 15 -> NONE (still in the gap)");
	CHECK(st11_region_of_block(&layout, 16u) == ST11_REGION_SONG_A, "block 16 -> SONG_A (song A's first block)");
	CHECK(st11_region_of_block(&layout, 143u) == ST11_REGION_SONG_A, "block 143 -> SONG_A (song A's last block)");
	CHECK(st11_region_of_block(&layout, 144u) == ST11_REGION_SONG_B, "block 144 -> SONG_B (song B's first block)");
	CHECK(st11_region_of_block(&layout, 271u) == ST11_REGION_SONG_B, "block 271 -> SONG_B (song B's last block, deviceBlocks-1)");
	CHECK(st11_region_of_block(&layout, 272u) == ST11_REGION_NONE, "block 272 -> NONE (one past the end of the device)");
	CHECK(st11_region_of_block(&layout, 0xFFFFFFFFu) == ST11_REGION_NONE, "block 0xffffffff -> NONE (fails closed, no overflow)");
}

/* ========================================================================
 * st_ab_session.c: the STATEFUL A/B write-safety session gate.
 *
 * Unlike everything above (which proves the pure codecs against frozen
 * fixtures), this section builds a small SYNTHETIC library and song in
 * memory -- the session gate's whole job is enforcing a per-session
 * destination snapshot, which has no single frozen fixture to compare
 * against. Every synthetic record below is still built through the real
 * st_stix_serialize()/st_stix_block_crc() machinery (never hand-poked
 * bytes), and every song checksum is computed through the real
 * st11_sector_encode()/st_checksum32_update() machinery -- only the
 * CONTENT is synthetic, not the encoding.
 * ======================================================================== */

/* A 272-block synthetic device, addressed exactly like
 * handoff/v1.3/binaries/stcp-capability-response.bin's own mock device:
 * index A=[0,1) index B=[1,1) song A=[16,144) song B=[144,272). Read back
 * by mock_read_block() for st_ab_session_verify_song_before_commit()'s
 * injected I/O -- this is the ONLY place in this test file real (in-memory)
 * storage I/O happens. */
static uint8_t g_mock_device[272u * ST11_PHYSICAL_BLOCK_BYTES];

static int mock_read_block(uint32_t block, uint8_t out[ST11_PHYSICAL_BLOCK_BYTES], void *ctx)
{
	(void)ctx;
	if (block >= 272u) {
		return -1;
	}
	memcpy(out, g_mock_device + (size_t)block * ST11_PHYSICAL_BLOCK_BYTES, ST11_PHYSICAL_BLOCK_BYTES);
	return 0;
}

/* Builds one fully self-consistent STIX v2 record (real CRC-32, computed
 * the same two-pass way test_index_record_crc_fixture() proves the real
 * fixture format uses) into `out`, and -- if `record_out` is non-NULL --
 * also hands back the struct so a caller can pass it directly to
 * st_ab_session_verify_song_before_commit() without re-parsing its own
 * bytes. `song_present == false` leaves every song/audio field at its
 * required-zero value (matching st_stix_validate_fields_only()'s
 * SONG_METADATA rule for an empty record). */
static void build_stix_block_minor(uint8_t out[ST11_PHYSICAL_BLOCK_BYTES], uint8_t format_minor,
				    uint32_t magic, uint8_t slot_identity,
				    uint8_t song_slot, bool song_present, uint64_t generation,
				    uint32_t song_start_block, uint32_t song_block_count, uint32_t frames,
				    uint32_t sector_count, const uint32_t stem_checksums[ST11_STEM_COUNT],
				    uint32_t song_checksum, st_stix_record_t *record_out)
{
	st_stix_record_t r;
	uint32_t s;

	memset(&r, 0, sizeof(r));
	r.magic = magic;
	r.index_version = ST11_STIX_VERSION;
	r.format_major = ST11_FORMAT_MAJOR;
	r.format_minor = format_minor;
	r.slot_identity = slot_identity;
	r.song_slot = song_slot;
	r.flags = song_present ? ST11_IX_FLAG_SONG_PRESENT : 0u;
	r.generation_lo = (uint32_t)(generation & 0xffffffffu);
	r.generation_hi = (uint32_t)(generation >> 32);
	if (song_present) {
		r.song_start_block = song_start_block;
		r.song_block_count = song_block_count;
		r.frames = frames;
		r.sector_count = sector_count;
		r.sample_rate = ST11_SAMPLE_RATE_HZ;
		r.channels = ST11_CHANNELS_PER_STEM;
		r.bit_depth = ST11_PCM_BIT_DEPTH;
		r.bpm_q8 = 120u * 256u;
		for (s = 0; s < ST11_STEM_COUNT; s++) {
			r.original_frames[s] = frames;
			r.stem_checksums[s] = stem_checksums[s];
		}
		r.song_checksum = song_checksum;
	}
	strncpy(r.title, "Session Test", ST11_INDEX_TEXT_BYTES);
	strncpy(r.artist, "Fixture", ST11_INDEX_TEXT_BYTES);

	r.crc32 = 0;
	st_stix_serialize(&r, out);
	r.crc32 = st_stix_block_crc(out);
	st_stix_serialize(&r, out);

	if (record_out) {
		*record_out = r;
	}
}

/* The same builder at this translation unit's own format version. Every
 * pre-existing caller wants that one; only the v1.2 planar test below needs to
 * name a different minor, and it says so explicitly rather than by redefining
 * a macro under the rest of the file. */
static void build_stix_block(uint8_t out[ST11_PHYSICAL_BLOCK_BYTES], uint32_t magic, uint8_t slot_identity,
			      uint8_t song_slot, bool song_present, uint64_t generation,
			      uint32_t song_start_block, uint32_t song_block_count, uint32_t frames,
			      uint32_t sector_count, const uint32_t stem_checksums[ST11_STEM_COUNT],
			      uint32_t song_checksum, st_stix_record_t *record_out)
{
	build_stix_block_minor(out, (uint8_t)ST11_FORMAT_MINOR, magic, slot_identity, song_slot, song_present,
				generation, song_start_block, song_block_count, frames, sector_count,
				stem_checksums, song_checksum, record_out);
}

/* Encodes a single, deterministic ST11_SECTOR_BYTES sector (sectorIndex=0,
 * firstFrame=0, `frame_count` real frames, no padding needed since one
 * sector covers the whole song) via the real st11_sector_encode(), and
 * recomputes its per-stem/song FNV-1a checksums the same way
 * test_song_sectors_fixture() proves matches the companion's own real
 * fixture checksums -- so a session-gate test that later corrupts one
 * declared checksum is corrupting a value proven correct in the first
 * place, not merely asserting whatever the code happens to produce.
 *
 * This is the v1.1 INTERLEAVED layout. The v1.2 planar equivalent, and the
 * proof that the checksums below are identical in both, is
 * planarize_song()/test_ab_session_v12_planar_verification() near the end of
 * this file. */
static void build_one_sector_song(uint8_t sector_out[ST11_SECTOR_BYTES], uint32_t frame_count,
				   uint32_t stem_checksum_out[ST11_STEM_COUNT], uint32_t *song_checksum_out)
{
	static st11_audio_frame_t frames[ST11_FRAMES_PER_SECTOR];
	uint32_t f, s;

	memset(frames, 0, sizeof(frames));
	for (f = 0; f < frame_count; f++) {
		for (s = 0; s < ST11_STEM_COUNT; s++) {
			int32_t l = (int32_t)(f * 4u + s) * 37 - 1000;
			int32_t r = -l + 3;

			frames[f].stem_l[s] = l;
			frames[f].stem_r[s] = r;
		}
	}

	st11_sector_encode(0u, 0u, frame_count, 120u * 256u, 0u, frames, sector_out);

	uint32_t stem_hash[ST11_STEM_COUNT];

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		stem_hash[s] = ST_CHECKSUM32_INIT;
	}
	for (f = 0; f < frame_count; f++) {
		for (s = 0; s < ST11_STEM_COUNT; s++) {
			/* WIDTH-GENERIC. This was uint8_t bytes[6] with six
			 * hand-written stores -- ST11_STEM_FRAME_BYTES exactly
			 * while samples were 24-bit, and two bytes too many at
			 * v1.3's 4-byte stereo frame. The mock song's declared
			 * checksums then covered bytes the device never stored,
			 * so verify_song_before_commit() correctly refused a
			 * song that was in fact intact. */
			uint8_t bytes[ST11_STEM_FRAME_BYTES];
			int32_t l = frames[f].stem_l[s];
			int32_t r = frames[f].stem_r[s];
			uint32_t byte_i;

			for (byte_i = 0u; byte_i < ST11_BYTES_PER_SAMPLE; byte_i++) {
				bytes[byte_i] =
					(uint8_t)((uint32_t)l >> (8u * byte_i));
				bytes[ST11_BYTES_PER_SAMPLE + byte_i] =
					(uint8_t)((uint32_t)r >> (8u * byte_i));
			}
			stem_hash[s] = st_checksum32_update(stem_hash[s], bytes, sizeof(bytes));
		}
	}

	uint8_t digest[ST11_STEM_COUNT * 4];

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		stem_checksum_out[s] = stem_hash[s];
		digest[s * 4 + 0] = (uint8_t)(stem_hash[s] & 0xff);
		digest[s * 4 + 1] = (uint8_t)((stem_hash[s] >> 8) & 0xff);
		digest[s * 4 + 2] = (uint8_t)((stem_hash[s] >> 16) & 0xff);
		digest[s * 4 + 3] = (uint8_t)((stem_hash[s] >> 24) & 0xff);
	}
	*song_checksum_out = st_checksum32_compute(digest, sizeof(digest));
}

/*
 * The core of the user's required correction: proves the write gate
 * enforces a FROZEN per-session A/B destination pair, not merely "any
 * address inside the four v1.1 regions". Builds a realistic active
 * library (index A active at generation 5, its song committed in song
 * region B) and exercises every rejection the directive explicitly lists
 * -- active song, active index, unrelated storage, wrong inactive slot,
 * invalid generation, invalid commit record -- plus the positive path
 * through to a real commit and the single-use closure that follows,
 * covering both the newly-frozen pair AND the just-superseded former-
 * active/rollback pair.
 */
static void test_ab_session_open_and_negative_writes(void)
{
	memset(g_mock_device, 0, sizeof(g_mock_device));

	st11_region_layout_t layout;
	bool layout_ok = st11_storage_layout_compute(0u, 272u, &layout);

	CHECK(layout_ok, "272-block synthetic device: layout computation succeeds");
	CHECK(layout.song_a_start == FIXTURE_SONG_A_START && layout.song_a_blocks == FIXTURE_SONG_A_BLOCKS &&
		      layout.song_b_start == FIXTURE_SONG_B_START && layout.song_b_blocks == FIXTURE_SONG_B_BLOCKS,
	      "272-block synthetic device: layout matches the real stcp-capability-response.bin geometry");

	/* The CURRENTLY ACTIVE library: index A, generation 5, song committed
	 * in song region B (one sector, 100 frames). Index B is blank
	 * (never written) -- exactly one valid record, so A wins outright. */
	uint32_t active_stem_checksums[ST11_STEM_COUNT];
	uint32_t active_song_checksum;
	uint8_t active_song_sector[ST11_SECTOR_BYTES];

	build_one_sector_song(active_song_sector, 100u, active_stem_checksums, &active_song_checksum);
	memcpy(g_mock_device + (size_t)FIXTURE_SONG_B_START * ST11_PHYSICAL_BLOCK_BYTES, active_song_sector,
	       ST11_SECTOR_BYTES);

	uint8_t block_a[ST11_PHYSICAL_BLOCK_BYTES];
	uint8_t block_b[ST11_PHYSICAL_BLOCK_BYTES];

	build_stix_block(block_a, ST11_INDEX_MAGIC, ST11_SLOT_A, ST11_SLOT_B, true, 5u, FIXTURE_SONG_B_START,
			  ST11_BLOCKS_PER_SECTOR, 100u, 1u, active_stem_checksums, active_song_checksum, NULL);
	memset(block_b, 0, sizeof(block_b));
	memcpy(g_mock_device + 0u, block_a, ST11_PHYSICAL_BLOCK_BYTES);
	memcpy(g_mock_device + (size_t)1u * ST11_PHYSICAL_BLOCK_BYTES, block_b, ST11_PHYSICAL_BLOCK_BYTES);

	st_ab_session_t s;
	st_ab_open_result_t open_res = st_ab_session_open_replace(&s, block_a, block_b, &layout,
								    ST11_BLOCKS_PER_SECTOR);

	CHECK(open_res == ST_AB_OPEN_OK, "open_replace: active-A/generation-5/song-in-B library opens OK");
	CHECK(s.active_index_slot == ST11_SLOT_A && s.active_song_slot == ST11_SLOT_B,
	      "open_replace: active pair correctly identified as index A / song B");
	CHECK(s.inactive_index_slot == ST11_SLOT_B && s.inactive_song_slot == ST11_SLOT_A,
	      "open_replace: frozen destination correctly complemented to index B / song A "
	      "(song WAS present, so song_slot is complemented too)");
	CHECK(s.active_generation == 5u, "open_replace: active_generation == 5");

	uint8_t dummy[ST11_PHYSICAL_BLOCK_BYTES];

	memset(dummy, 0, sizeof(dummy));

	CHECK(st_ab_session_check_write(&s, FIXTURE_SONG_B_START, dummy) == ST_AB_WRITE_ERR_ACTIVE_REGION,
	      "REJECT: write to the active song (region B, its first block) -- ACTIVE_REGION");
	CHECK(st_ab_session_check_write(&s, 0u, dummy) == ST_AB_WRITE_ERR_ACTIVE_REGION,
	      "REJECT: write to the active index (block 0, index A) -- ACTIVE_REGION");
	CHECK(st_ab_session_check_write(&s, 5u, dummy) == ST_AB_WRITE_ERR_OUTSIDE_FROZEN_PAIR,
	      "REJECT: write to unrelated storage (block 5, the alignment gap -- not in any region) "
	      "-- OUTSIDE_FROZEN_PAIR");

	/* The NEW song this session is uploading: a different one-sector,
	 * 100-frame song, destined for the frozen song region (A). */
	uint32_t new_stem_checksums[ST11_STEM_COUNT];
	uint32_t new_song_checksum;
	uint8_t new_song_sector[ST11_SECTOR_BYTES];

	build_one_sector_song(new_song_sector, 100u, new_stem_checksums, &new_song_checksum);

	uint32_t k;
	bool all_frozen_song_writes_ok = true;

	for (k = 0; k < ST11_BLOCKS_PER_SECTOR; k++) {
		uint32_t blk = FIXTURE_SONG_A_START + k;
		const uint8_t *payload = new_song_sector + (size_t)k * ST11_PHYSICAL_BLOCK_BYTES;

		if (st_ab_session_check_write(&s, blk, payload) != ST_AB_WRITE_OK) {
			all_frozen_song_writes_ok = false;
		}
		memcpy(g_mock_device + (size_t)blk * ST11_PHYSICAL_BLOCK_BYTES, payload, ST11_PHYSICAL_BLOCK_BYTES);
	}
	CHECK(all_frozen_song_writes_ok,
	      "ACCEPT: all 16 blocks of the new song, written to the frozen destination (song A) -- OK");

	/* WRONG INACTIVE SLOT: a structurally valid, correctly-CRC'd,
	 * correctly-addressed (slot_identity == B, the frozen index slot)
	 * uncommitted record whose OWN song_slot field names song B (the
	 * ACTIVE song slot) instead of this session's frozen song A. */
	uint8_t wrong_slot_block[ST11_PHYSICAL_BLOCK_BYTES];

	build_stix_block(wrong_slot_block, 0u, ST11_SLOT_B, ST11_SLOT_B, true, 6u, FIXTURE_SONG_B_START,
			  ST11_BLOCKS_PER_SECTOR, 100u, 1u, new_stem_checksums, new_song_checksum, NULL);
	CHECK(st_ab_session_check_write(&s, 1u, wrong_slot_block) == ST_AB_WRITE_ERR_BAD_COMMIT_RECORD,
	      "REJECT: commit-record write to the frozen index block, but its own songSlot names the "
	      "WRONG (active) song slot -- BAD_COMMIT_RECORD");

	/* INVALID GENERATION: correctly addressed AND correctly targeted
	 * (song_slot == A, this session's real frozen song), but declares
	 * generation 999 instead of active_generation+1 (6). */
	uint8_t bad_generation_block[ST11_PHYSICAL_BLOCK_BYTES];

	build_stix_block(bad_generation_block, 0u, ST11_SLOT_B, ST11_SLOT_A, true, 999u, FIXTURE_SONG_A_START,
			  ST11_BLOCKS_PER_SECTOR, 100u, 1u, new_stem_checksums, new_song_checksum, NULL);
	CHECK(st_ab_session_check_write(&s, 1u, bad_generation_block) == ST_AB_WRITE_ERR_WRONG_GENERATION,
	      "REJECT: commit-record write correctly targeted but generation 999 != active_generation+1 (6) "
	      "-- WRONG_GENERATION");

	/* INVALID COMMIT RECORD: plain garbage bytes at the frozen index
	 * destination -- fails CRC (and everything else) outright. */
	uint8_t garbage_block[ST11_PHYSICAL_BLOCK_BYTES];

	memset(garbage_block, 0xAA, sizeof(garbage_block));
	CHECK(st_ab_session_check_write(&s, 1u, garbage_block) == ST_AB_WRITE_ERR_BAD_COMMIT_RECORD,
	      "REJECT: garbage bytes at the frozen index destination -- BAD_COMMIT_RECORD (fails CRC)");

	/* The REAL candidate: correctly addressed, correctly targeted,
	 * correct generation (6), real checksums matching the song bytes
	 * actually written to the frozen song region above. */
	uint8_t draft_block[ST11_PHYSICAL_BLOCK_BYTES];
	st_stix_record_t candidate;

	build_stix_block(draft_block, 0u, ST11_SLOT_B, ST11_SLOT_A, true, 6u, FIXTURE_SONG_A_START,
			  ST11_BLOCKS_PER_SECTOR, 100u, 1u, new_stem_checksums, new_song_checksum, &candidate);
	CHECK(st_ab_session_check_write(&s, 1u, draft_block) == ST_AB_WRITE_OK,
	      "ACCEPT: well-formed uncommitted (magic=0) draft record at the frozen index destination -- OK");
	memcpy(g_mock_device + (size_t)1u * ST11_PHYSICAL_BLOCK_BYTES, draft_block, ST11_PHYSICAL_BLOCK_BYTES);

	uint8_t commit_block[ST11_PHYSICAL_BLOCK_BYTES];
	st_stix_record_t commit_candidate;

	build_stix_block(commit_block, ST11_INDEX_MAGIC, ST11_SLOT_B, ST11_SLOT_A, true, 6u, FIXTURE_SONG_A_START,
			  ST11_BLOCKS_PER_SECTOR, 100u, 1u, new_stem_checksums, new_song_checksum, &commit_candidate);
	CHECK(st_ab_session_check_write(&s, 1u, commit_block) == ST_AB_WRITE_ERR_SONG_NOT_VERIFIED,
	      "REJECT: the magic-committing write, attempted BEFORE st_ab_session_verify_song_before_commit() "
	      "was ever run -- SONG_NOT_VERIFIED (never activates a record on the companion's word alone)");

	uint8_t scratch_sector[ST11_SECTOR_BYTES];
	bool verified = st_ab_session_verify_song_before_commit(&s, &candidate, mock_read_block, NULL,
								 scratch_sector);

	CHECK(verified,
	      "verify_song_before_commit: recomputed checksums from the REAL bytes on the mock device "
	      "match the candidate's declared checksums -- true");

	st_stix_record_t corrupted_candidate = candidate;

	corrupted_candidate.stem_checksums[0] += 1u;
	bool verified_corrupt = st_ab_session_verify_song_before_commit(&s, &corrupted_candidate, mock_read_block,
									  NULL, scratch_sector);

	CHECK(!verified_corrupt,
	      "verify_song_before_commit: a candidate with one declared stem checksum tampered "
	      "does NOT verify against the real device bytes -- false");

	/* ---- the incremental fast path is v1.2-only, deliberately ----
	 *
	 * The bulk upload path reads every sector back off the media as it
	 * writes it, so it can fold the commit checksums in as it goes and make
	 * the commit itself instant. (Before that existed, a real 248.5 MiB
	 * commit re-read 509,024 blocks inside one wire command, overrunning
	 * both the host's acknowledgement timeout and the 4000ms hardware
	 * watchdog.)
	 *
	 * It exists for the layout this firmware actually stores, and the bytes
	 * here are v1.1 INTERLEAVED. The accumulator cannot read them, says so
	 * by invalidating, and the caller's documented fallback -- the full
	 * re-read, which DOES dispatch on the record's declared version and
	 * which verified this very record a few lines above -- is what carries
	 * the commit. That is the contract, checked here rather than assumed.
	 *
	 * The positive incremental coverage (agreement with the full re-read,
	 * idempotent retries, gap invalidation) lives with the layout it
	 * applies to, in test_ab_session_v12_planar_verification() below. */
	uint8_t acc_sector[ST11_SECTOR_BYTES];
	uint32_t acc_k;

	for (acc_k = 0; acc_k < ST11_BLOCKS_PER_SECTOR; acc_k++) {
		mock_read_block(FIXTURE_SONG_A_START + acc_k,
				 acc_sector + (size_t)acc_k * ST11_PHYSICAL_BLOCK_BYTES, NULL);
	}

	CHECK(!st_ab_session_verify_accumulated(&s, &candidate),
	      "verify_accumulated: with nothing accumulated yet, refuses (never passes vacuously) -- false");

	st_ab_session_accumulate_sector(&s, 0u, acc_sector);
	CHECK(!st_ab_session_verify_accumulated(&s, &candidate),
	      "verify_accumulated: v1.1 INTERLEAVED bytes are not accumulable by the v1.2 fast path -- it "
	      "invalidates instead of guessing, and the full re-read is what verified this record -- false");

	st_ab_session_mark_song_verified(&s);
	CHECK(st_ab_session_check_write(&s, 1u, commit_block) == ST_AB_WRITE_OK,
	      "ACCEPT: the SAME magic-committing write, now AFTER verify_song_before_commit() + "
	      "mark_song_verified() -- OK");

	/* Single-use closure: EVERY further write is refused now, including
	 * to the pair just committed AND to the former-active/rollback pair
	 * (docs: "no additional writes to the former active pair immediately
	 * after the new magic lands"). */
	CHECK(st_ab_session_check_write(&s, FIXTURE_SONG_A_START, dummy) == ST_AB_WRITE_ERR_SESSION_CLOSED,
	      "REJECT after commit: write to the just-committed frozen song (A) -- SESSION_CLOSED");
	CHECK(st_ab_session_check_write(&s, 1u, draft_block) == ST_AB_WRITE_ERR_SESSION_CLOSED,
	      "REJECT after commit: write to the just-committed frozen index (B) -- SESSION_CLOSED");
	CHECK(st_ab_session_check_write(&s, FIXTURE_SONG_B_START, dummy) == ST_AB_WRITE_ERR_SESSION_CLOSED,
	      "REJECT after commit: write to the former-active song (B), now the rollback copy -- SESSION_CLOSED");
	CHECK(st_ab_session_check_write(&s, 0u, dummy) == ST_AB_WRITE_ERR_SESSION_CLOSED,
	      "REJECT after commit: write to the former-active index (A), now the rollback copy -- SESSION_CLOSED");

	(void)commit_candidate;
}

/*
 * needed_song_blocks is a CEILING, not an exact-match target: the real wire
 * protocol has no verb for the companion to declare a song's size before
 * writes begin (docs section 1), so a real caller (main.c, wired in a later
 * commit) can only ever pass the frozen region's own full capacity here, not
 * the specific song's exact block count. Opens with a ceiling of a full
 * region (128 blocks) and proves a commit record claiming far fewer blocks
 * (16, one sector) is still accepted -- the OLD exact-match rule would have
 * wrongly rejected every real-world song shorter than the whole region.
 */
static void test_ab_session_ceiling_allows_smaller_song(void)
{
	memset(g_mock_device, 0, sizeof(g_mock_device));

	st11_region_layout_t layout;

	CHECK(st11_storage_layout_compute(0u, 272u, &layout), "ceiling test: layout computation succeeds");

	uint32_t stem_checksums[ST11_STEM_COUNT] = { 0u, 0u, 0u, 0u };
	uint8_t blank_b[ST11_PHYSICAL_BLOCK_BYTES];
	uint8_t valid_a[ST11_PHYSICAL_BLOCK_BYTES];

	memset(blank_b, 0, sizeof(blank_b));
	/* A blank-but-initialized library (generation 1, no song, index A active,
	 * index B blank) -- the same shape st_ab_session_open_init() itself
	 * would have produced, so the very first real upload's destination is
	 * song A / index B, per docs section 5's worked example. */
	build_stix_block(valid_a, ST11_INDEX_MAGIC, ST11_SLOT_A, ST11_SLOT_A, false, 1u, 0u, 0u, 0u, 0u,
			  stem_checksums, 0u, NULL);

	st_ab_session_t s;
	st_ab_open_result_t open_res = st_ab_session_open_replace(&s, valid_a, blank_b, &layout,
								    layout.song_a_blocks);

	CHECK(open_res == ST_AB_OPEN_OK, "ceiling test: opens with needed_song_blocks == the FULL region "
					 "capacity (128), not any specific song's size");

	uint32_t new_stem_checksums[ST11_STEM_COUNT];
	uint32_t new_song_checksum;
	uint8_t new_song_sector[ST11_SECTOR_BYTES];

	build_one_sector_song(new_song_sector, 100u, new_stem_checksums, &new_song_checksum);

	uint32_t k;
	bool all_ok = true;

	for (k = 0; k < ST11_BLOCKS_PER_SECTOR; k++) {
		uint32_t blk = FIXTURE_SONG_A_START + k;

		if (st_ab_session_check_write(&s, blk, new_song_sector + (size_t)k * ST11_PHYSICAL_BLOCK_BYTES) !=
		    ST_AB_WRITE_OK) {
			all_ok = false;
		}
		memcpy(g_mock_device + (size_t)blk * ST11_PHYSICAL_BLOCK_BYTES,
		       new_song_sector + (size_t)k * ST11_PHYSICAL_BLOCK_BYTES, ST11_PHYSICAL_BLOCK_BYTES);
	}
	CHECK(all_ok, "ceiling test: a real 16-block (one-sector) song writes fine inside a 128-block ceiling");

	uint8_t commit_block[ST11_PHYSICAL_BLOCK_BYTES];
	st_stix_record_t candidate;

	/* song_block_count == 16, FAR below the session's 128-block ceiling --
	 * this is exactly what the old `== needed_song_blocks` rule would have
	 * rejected as BAD_COMMIT_RECORD. */
	build_stix_block(commit_block, 0u, ST11_SLOT_B, ST11_SLOT_A, true, 2u, FIXTURE_SONG_A_START,
			  ST11_BLOCKS_PER_SECTOR, 100u, 1u, new_stem_checksums, new_song_checksum, &candidate);
	CHECK(st_ab_session_check_write(&s, 1u, commit_block) == ST_AB_WRITE_OK,
	      "ceiling test: ACCEPT a draft record whose song_block_count (16) is LESS than the session's "
	      "ceiling (128) -- the ceiling bounds a maximum, not an exact size");

	uint8_t scratch_sector[ST11_SECTOR_BYTES];
	bool verified = st_ab_session_verify_song_before_commit(&s, &candidate, mock_read_block, NULL,
								 scratch_sector);

	CHECK(verified, "ceiling test: verify_song_before_commit succeeds for the shorter-than-ceiling song");

	st_ab_session_mark_song_verified(&s);
	build_stix_block(commit_block, ST11_INDEX_MAGIC, ST11_SLOT_B, ST11_SLOT_A, true, 2u, FIXTURE_SONG_A_START,
			  ST11_BLOCKS_PER_SECTOR, 100u, 1u, new_stem_checksums, new_song_checksum, NULL);
	CHECK(st_ab_session_check_write(&s, 1u, commit_block) == ST_AB_WRITE_OK,
	      "ceiling test: the magic-committing write for the same shorter-than-ceiling song succeeds");
}

/*
 * Cross-contract check against handoff/v1.1/reports/successive-
 * replacement.json (not covered by SHA256SUMS.txt/CRC32SUMS.txt -- like
 * magic-write-cases.json, it is an unmanifested report, cited directly).
 * Its own "uploads" array declares five successive REPLACE commits, each
 * alternating destination and incrementing generation by exactly one:
 *   step0 "ONE (baseline)": songSlot=A indexSlot=B generation=2
 *     rollbackGeneration=1 -- the SAME real shape upload-1-successful.json
 *     and test_ab_session_ceiling_allows_smaller_song() above both
 *     already establish for the very first real upload out of a truly
 *     blank device (docs section 5's worked example).
 *   step1 "S1": songSlot=B indexSlot=A generation=3 rollbackGeneration=2
 *     -- the SAME real shape upload-2-successful.json's own declared
 *     result.
 *   step2 "S2": songSlot=A indexSlot=B generation=4 rollbackGeneration=3
 *   step3 "S3": songSlot=B indexSlot=A generation=5 rollbackGeneration=4
 *   step4 "S4": songSlot=A indexSlot=B generation=6 rollbackGeneration=5
 * plus a final "rollback" scenario: index B (step4's own generation-6
 * commit) gets one CRC byte corrupted, and the selector is declared to
 * fall back to index A / generation 5 / songSlot B (step3's own commit).
 *
 * The fixture's own songRegionSha256/indexSha256 values are hashes of the
 * REAL companion's own real audio content, which was never supplied to
 * this repository (only the hashes were) -- reproducing THOSE exact bytes
 * is impossible without fabricating audio data never actually provided,
 * which this whole suite's non-fabrication rule forbids. What IS
 * reproduced here, with zero fabricated bytes, is the STRUCTURAL
 * property the fixture demonstrates: driving five REAL successive
 * REPLACE sessions (through the exact same st_ab_session_open_replace()/
 * check_write()/verify_song_before_commit()/mark_song_verified()
 * functions every other test in this suite calls) with real, freshly
 * STSC-encoded song content (build_one_sector_song(), the same real
 * codec + FNV-1a machinery test_song_sectors_fixture() proves matches
 * the companion's own checksums) reproduces the SAME destination-
 * alternation, generation-increment, and rollback-fallback pattern the
 * real companion's own five real uploads produced -- proving the
 * SELECTION ALGORITHM's behavior matches across an arbitrarily long
 * chain, not that any particular byte content matches.
 */
static void test_successive_replacement_matches_fixture(void)
{
	memset(g_mock_device, 0, sizeof(g_mock_device));

	st11_region_layout_t layout;

	CHECK(st11_storage_layout_compute(0u, 272u, &layout), "successive-replacement: layout computation succeeds");

	static const uint8_t expected_song_slot[5] = { ST11_SLOT_A, ST11_SLOT_B, ST11_SLOT_A, ST11_SLOT_B,
							ST11_SLOT_A };
	static const uint8_t expected_index_slot[5] = { ST11_SLOT_B, ST11_SLOT_A, ST11_SLOT_B, ST11_SLOT_A,
							 ST11_SLOT_B };
	static const uint32_t expected_generation[5] = { 2u, 3u, 4u, 5u, 6u };

	/* Seed the same "post-INIT, generation-1, blank, index A active"
	 * state test_ab_session_ceiling_allows_smaller_song() above already
	 * establishes as the real shape st_ab_session_open_init() itself
	 * produces on a truly blank device -- step 0 below starts from
	 * exactly what that real INIT commit leaves behind. */
	uint32_t zero_stem_checksums[ST11_STEM_COUNT] = { 0u, 0u, 0u, 0u };
	uint8_t seed_a[ST11_PHYSICAL_BLOCK_BYTES];
	uint8_t seed_b[ST11_PHYSICAL_BLOCK_BYTES];

	build_stix_block(seed_a, ST11_INDEX_MAGIC, ST11_SLOT_A, ST11_SLOT_A, false, 1u, 0u, 0u, 0u, 0u,
			  zero_stem_checksums, 0u, NULL);
	memset(seed_b, 0, sizeof(seed_b));
	memcpy(g_mock_device + (size_t)layout.index_a_start * ST11_PHYSICAL_BLOCK_BYTES, seed_a,
	       ST11_PHYSICAL_BLOCK_BYTES);
	memcpy(g_mock_device + (size_t)layout.index_b_start * ST11_PHYSICAL_BLOCK_BYTES, seed_b,
	       ST11_PHYSICAL_BLOCK_BYTES);

	int step;
	bool all_opens_ok = true;

	for (step = 0; step < 5; step++) {
		uint8_t cur_a[ST11_PHYSICAL_BLOCK_BYTES];
		uint8_t cur_b[ST11_PHYSICAL_BLOCK_BYTES];

		memcpy(cur_a, g_mock_device + (size_t)layout.index_a_start * ST11_PHYSICAL_BLOCK_BYTES,
		       ST11_PHYSICAL_BLOCK_BYTES);
		memcpy(cur_b, g_mock_device + (size_t)layout.index_b_start * ST11_PHYSICAL_BLOCK_BYTES,
		       ST11_PHYSICAL_BLOCK_BYTES);

		st_ab_session_t s;
		st_ab_open_result_t open_res =
			st_ab_session_open_replace(&s, cur_a, cur_b, &layout, FIXTURE_SONG_A_BLOCKS);

		if (open_res != ST_AB_OPEN_OK) {
			all_opens_ok = false;
			break;
		}
		CHECK(s.inactive_song_slot == expected_song_slot[step],
		      "successive-replacement step %d: destination songSlot == %s, matching the fixture's own "
		      "declared songSlot",
		      step, expected_song_slot[step] == ST11_SLOT_A ? "A" : "B");
		CHECK(s.inactive_index_slot == expected_index_slot[step],
		      "successive-replacement step %d: destination indexSlot == %s, matching the fixture's own "
		      "declared indexSlot",
		      step, expected_index_slot[step] == ST11_SLOT_A ? "A" : "B");

		uint32_t dest_song_start =
			(s.inactive_song_slot == ST11_SLOT_A) ? layout.song_a_start : layout.song_b_start;
		uint32_t dest_index_block =
			(s.inactive_index_slot == ST11_SLOT_A) ? layout.index_a_start : layout.index_b_start;

		uint32_t stem_checksums[ST11_STEM_COUNT];
		uint32_t song_checksum;
		uint8_t sector[ST11_SECTOR_BYTES];
		uint32_t frame_count = 100u + (uint32_t)step; /* distinct real content per step */

		build_one_sector_song(sector, frame_count, stem_checksums, &song_checksum);

		uint32_t k;
		bool writes_ok = true;

		for (k = 0; k < ST11_BLOCKS_PER_SECTOR; k++) {
			if (st_ab_session_check_write(&s, dest_song_start + k,
						       sector + (size_t)k * ST11_PHYSICAL_BLOCK_BYTES) !=
			    ST_AB_WRITE_OK) {
				writes_ok = false;
			}
			memcpy(g_mock_device + (size_t)(dest_song_start + k) * ST11_PHYSICAL_BLOCK_BYTES,
			       sector + (size_t)k * ST11_PHYSICAL_BLOCK_BYTES, ST11_PHYSICAL_BLOCK_BYTES);
		}
		CHECK(writes_ok, "successive-replacement step %d: real song write accepted", step);

		uint8_t commit_block[ST11_PHYSICAL_BLOCK_BYTES];
		st_stix_record_t candidate;

		build_stix_block(commit_block, ST11_INDEX_MAGIC, (uint8_t)s.inactive_index_slot,
				  (uint8_t)s.inactive_song_slot, true, expected_generation[step], dest_song_start,
				  ST11_BLOCKS_PER_SECTOR, frame_count, 1u, stem_checksums, song_checksum,
				  &candidate);

		bool verified = st_ab_session_verify_song_before_commit(&s, &candidate, mock_read_block, NULL,
									 sector);

		CHECK(verified, "successive-replacement step %d: real verify_song_before_commit succeeds", step);
		st_ab_session_mark_song_verified(&s);

		st_ab_write_check_t wr = st_ab_session_check_write(&s, dest_index_block, commit_block);

		CHECK(wr == ST_AB_WRITE_OK,
		      "successive-replacement step %d: magic-committing write accepted at generation %u", step,
		      expected_generation[step]);
		memcpy(g_mock_device + (size_t)dest_index_block * ST11_PHYSICAL_BLOCK_BYTES, commit_block,
		       ST11_PHYSICAL_BLOCK_BYTES);

		/* The rollback copy (the slot this session never touched) must
		 * independently validate at exactly generation-1, matching the
		 * fixture's own declared rollbackGeneration for this step. */
		uint32_t rollback_slot = (s.inactive_index_slot == ST11_SLOT_A) ? ST11_SLOT_B : ST11_SLOT_A;
		uint32_t rollback_block = (rollback_slot == ST11_SLOT_A) ? layout.index_a_start : layout.index_b_start;
		uint8_t rollback_bytes[ST11_PHYSICAL_BLOCK_BYTES];

		memcpy(rollback_bytes, g_mock_device + (size_t)rollback_block * ST11_PHYSICAL_BLOCK_BYTES,
		       ST11_PHYSICAL_BLOCK_BYTES);

		st_stix_record_t rollback_rec;
		st_stix_validity_t rv = st_stix_validate(rollback_bytes, (uint8_t)rollback_slot, layout.song_a_start,
							  layout.song_a_blocks, layout.song_b_start,
							  layout.song_b_blocks, &rollback_rec);
		uint32_t expected_rollback_gen = expected_generation[step] - 1u;

		CHECK(rv == ST_STIX_VALID && rollback_rec.generation_lo == expected_rollback_gen,
		      "successive-replacement step %d: rollback copy independently validates at generation %u, "
		      "matching the fixture's own declared rollbackGeneration",
		      step, expected_rollback_gen);

		/* Independently re-derive the selector's own view after this
		 * commit, matching the fixture's own declared
		 * selectedGenerationAfterReboot. */
		uint8_t post_a[ST11_PHYSICAL_BLOCK_BYTES];
		uint8_t post_b[ST11_PHYSICAL_BLOCK_BYTES];

		memcpy(post_a, g_mock_device + (size_t)layout.index_a_start * ST11_PHYSICAL_BLOCK_BYTES,
		       ST11_PHYSICAL_BLOCK_BYTES);
		memcpy(post_b, g_mock_device + (size_t)layout.index_b_start * ST11_PHYSICAL_BLOCK_BYTES,
		       ST11_PHYSICAL_BLOCK_BYTES);

		st_stix_library_state_t lib;

		st_stix_read_library(post_a, post_b, layout.song_a_start, layout.song_a_blocks, layout.song_b_start,
				      layout.song_b_blocks, &lib);

		CHECK(lib.status == ST_STIX_LIB_OK && lib.generation == expected_generation[step],
		      "successive-replacement step %d: selectedGenerationAfterReboot == %u, matching the fixture's "
		      "own declared value",
		      step, expected_generation[step]);
	}
	CHECK(all_opens_ok, "successive-replacement: every one of the 5 successive REPLACE sessions opened "
			     "successfully");

	/*
	 * The fixture's own "rollback" object: corruptedIndexSlot=B,
	 * corruptedGeneration=6 (step4's own commit -- matches above),
	 * selectedIndexSlot=A, selectedGeneration=5, selectedSongSlot=B
	 * (step3's own commit). Reproduced with the SAME real one-CRC-byte
	 * corruption technique test_corrupt_newest_index_fallback() above
	 * already applies to a real frozen fixture.
	 */
	uint8_t corrupted_b[ST11_PHYSICAL_BLOCK_BYTES];

	memcpy(corrupted_b, g_mock_device + (size_t)layout.index_b_start * ST11_PHYSICAL_BLOCK_BYTES,
	       ST11_PHYSICAL_BLOCK_BYTES);
	corrupted_b[ST11_IX_OFF_CRC32] ^= 0xffu; /* flip the CRC field's LSB -- guaranteed to invalidate a valid CRC */

	st_stix_record_t rollback_selected;
	st_stix_select_t rsel = st_stix_select_active(g_mock_device + (size_t)layout.index_a_start *
								ST11_PHYSICAL_BLOCK_BYTES,
						       corrupted_b, layout.song_a_start, layout.song_a_blocks,
						       layout.song_b_start, layout.song_b_blocks, &rollback_selected);

	CHECK(rsel == ST_STIX_SELECT_A,
	      "successive-replacement rollback: index B (generation 6, one CRC byte corrupted) is rejected; "
	      "selector falls back to index A -- matching the fixture's own declared selectedIndexSlot=A");
	CHECK(rollback_selected.generation_lo == 5u,
	      "successive-replacement rollback: resolved generation == 5, matching the fixture's own declared "
	      "selectedGeneration");
	CHECK(rollback_selected.song_slot == ST11_SLOT_B,
	      "successive-replacement rollback: resolved songSlot == B, matching the fixture's own declared "
	      "selectedSongSlot");
}

/*
 * The open-time gates: NOT_INITIALIZED / ALREADY_INITIALIZED /
 * NOT_CONFIRMED / CAPACITY -- each refuses BEFORE any write is ever
 * attempted, and each is a distinct wrong-precondition a real caller can
 * hit (companion tries to upload with no active song yet; companion tries
 * explicit-init on an already-initialized device; explicit-init without a
 * confirmed destructive token; a song too big for the frozen region).
 */
static void test_ab_session_open_errors(void)
{
	st11_region_layout_t layout;

	CHECK(st11_storage_layout_compute(0u, 272u, &layout), "layout computation succeeds (open-errors test)");

	uint8_t blank_a[ST11_PHYSICAL_BLOCK_BYTES];
	uint8_t blank_b[ST11_PHYSICAL_BLOCK_BYTES];

	memset(blank_a, 0, sizeof(blank_a));
	memset(blank_b, 0, sizeof(blank_b));

	st_ab_session_t s1;

	CHECK(st_ab_session_open_replace(&s1, blank_a, blank_b, &layout, ST11_BLOCKS_PER_SECTOR) ==
		      ST_AB_OPEN_ERR_NOT_INITIALIZED,
	      "open_replace on a blank (never-initialized) library -- NOT_INITIALIZED "
	      "(there is no valid active record to replace)");

	st_ab_session_t s2;

	CHECK(st_ab_session_open_init(&s2, blank_a, blank_b, &layout, false) == ST_AB_OPEN_ERR_NOT_CONFIRMED,
	      "open_init on a blank library WITHOUT an explicit confirmation -- NOT_CONFIRMED");

	st_ab_session_t s3;
	st_ab_open_result_t init_res = st_ab_session_open_init(&s3, blank_a, blank_b, &layout, true);

	CHECK(init_res == ST_AB_OPEN_OK, "open_init on a blank library, confirmed -- OK");
	CHECK(s3.kind == ST_AB_SESSION_INIT && s3.inactive_index_slot == ST11_SLOT_A &&
		      s3.inactive_song_slot == ST11_SLOT_A,
	      "open_init: targets index A / no song region (INIT never writes a song region)");

	uint32_t stem_checksums[ST11_STEM_COUNT] = { 0u, 0u, 0u, 0u };
	uint8_t valid_a[ST11_PHYSICAL_BLOCK_BYTES];

	build_stix_block(valid_a, ST11_INDEX_MAGIC, ST11_SLOT_A, ST11_SLOT_A, false, 1u, 0u, 0u, 0u, 0u,
			  stem_checksums, 0u, NULL);

	st_ab_session_t s4;

	CHECK(st_ab_session_open_init(&s4, valid_a, blank_b, &layout, true) == ST_AB_OPEN_ERR_ALREADY_INITIALIZED,
	      "open_init on an already-valid library (index A generation 1, no song) -- "
	      "ALREADY_INITIALIZED (refuses to clobber a valid library)");

	st_ab_session_t s5;

	CHECK(st_ab_session_open_replace(&s5, valid_a, blank_b, &layout, FIXTURE_SONG_B_BLOCKS + 1u) ==
		      ST_AB_OPEN_ERR_CAPACITY,
	      "open_replace with needed_song_blocks one more than the frozen inactive song region's "
	      "real capacity -- CAPACITY");
}

/* ========================================================================
 * v1.2 SONG-PLANAR commit verification.
 *
 * Everything above verifies the v1.1 INTERLEAVED layout, because this
 * translation unit is compiled at ST11_FORMAT_MINOR=1 so the frozen companion
 * fixtures parse. v1.2 is the layout the firmware actually stores, and it is
 * the reason a record now says which of the two it is: the session gate
 * dispatches on the version the RECORD declares, so a v1.1 recording still
 * replays byte-for-byte while a v1.2 upload verifies in its own layout.
 * ======================================================================== */

/* One v1.1 sector -> the four groups that same 340-frame span occupies under
 * v1.2, laid end to end in stem order. That is exactly one 8192-byte sector's
 * worth (4 * 2048), so a one-group-per-stem song is still a one-sector song
 * and every address in this file's synthetic device is unchanged.
 *
 * Goes through the real st_pl_from_v11_sector() -- the same one function the
 * companion's encoder and the read path's fixture test use -- rather than a
 * second hand-rolled copy of the migration. */
static void planarize_song(const uint8_t v11_sector[ST11_SECTOR_BYTES],
			    uint8_t planar_out[ST11_SECTOR_BYTES])
{
	uint8_t groups[ST_PL_STEMS][ST_PL_GROUP_BYTES];
	uint32_t k;

	if (!st_pl_from_v11_sector(v11_sector, groups)) {
		fprintf(stderr, "FATAL: st_pl_from_v11_sector refused a sector this file just encoded\n");
		exit(2);
	}
	for (k = 0; k < ST_PL_STEMS; k++) {
		memcpy(planar_out + (size_t)k * ST_PL_GROUP_BYTES, groups[k], ST_PL_GROUP_BYTES);
	}
}

static void test_ab_session_v12_planar_verification(void)
{
	memset(g_mock_device, 0, sizeof(g_mock_device));

	st11_region_layout_t layout;

	CHECK(st11_storage_layout_compute(0u, 272u, &layout), "v1.2: layout computation succeeds");

	/* The FNV prime's inverse, which is what lets the accumulator fold whole
	 * 340-frame groups and take the tail padding back off once a record
	 * finally declares the frame count. Asserted, not trusted: a wrong
	 * constant here would silently produce wrong checksums for every song
	 * whose length is not a multiple of 340 -- which is nearly all of them. */
	CHECK((uint32_t)(0x01000193u * ST_CHECKSUM32_PRIME_INV) == 1u,
	      "v1.2: ST_CHECKSUM32_PRIME_INV is the real modular inverse of the FNV prime mod 2^32");
	{
		uint8_t zeros[64];
		uint32_t folded;

		memset(zeros, 0, sizeof(zeros));
		folded = st_checksum32_update(0xdeadbeefu, zeros, sizeof(zeros));
		CHECK(st_checksum32_unfold_zeros(folded, sizeof(zeros)) == 0xdeadbeefu,
		      "v1.2: unfolding 64 zero bytes exactly undoes folding them");
	}

	/* An ACTIVE library to replace: index A, generation 5, song in region B.
	 * Its own song stays v1.1 -- this session is not reading it, and a
	 * device really can hold a pre-upgrade song in the region it is about
	 * to overwrite. */
	uint32_t active_stem[ST11_STEM_COUNT];
	uint32_t active_song;
	uint8_t active_sector[ST11_SECTOR_BYTES];

	build_one_sector_song(active_sector, 100u, active_stem, &active_song);
	memcpy(g_mock_device + (size_t)FIXTURE_SONG_B_START * ST11_PHYSICAL_BLOCK_BYTES, active_sector,
	       ST11_SECTOR_BYTES);

	uint8_t block_a[ST11_PHYSICAL_BLOCK_BYTES];
	uint8_t block_b[ST11_PHYSICAL_BLOCK_BYTES];

	build_stix_block(block_a, ST11_INDEX_MAGIC, ST11_SLOT_A, ST11_SLOT_B, true, 5u, FIXTURE_SONG_B_START,
			  ST11_BLOCKS_PER_SECTOR, 100u, 1u, active_stem, active_song, NULL);
	memset(block_b, 0, sizeof(block_b));
	memcpy(g_mock_device + 0u, block_a, ST11_PHYSICAL_BLOCK_BYTES);
	memcpy(g_mock_device + (size_t)1u * ST11_PHYSICAL_BLOCK_BYTES, block_b, ST11_PHYSICAL_BLOCK_BYTES);

	st_ab_session_t s;

	CHECK(st_ab_session_open_replace(&s, block_a, block_b, &layout, ST11_BLOCKS_PER_SECTOR) == ST_AB_OPEN_OK,
	      "v1.2: open_replace succeeds; frozen destination is song A / index B");

	/* THE NEW SONG, uploaded as v1.2 planar. 300 frames, NOT a multiple of
	 * 340, so the tail of every stem's only group is zero padding -- the
	 * case the un-fold exists for. The checksums are the ones
	 * build_one_sector_song() derived from the SAMPLES; that they verify
	 * against planar bytes without being recomputed is the whole claim of
	 * the format change. */
	uint32_t new_stem[ST11_STEM_COUNT];
	uint32_t new_song;
	uint8_t v11_sector[ST11_SECTOR_BYTES];
	uint8_t planar_sector[ST11_SECTOR_BYTES];

	build_one_sector_song(v11_sector, 300u, new_stem, &new_song);
	planarize_song(v11_sector, planar_sector);
	memcpy(g_mock_device + (size_t)FIXTURE_SONG_A_START * ST11_PHYSICAL_BLOCK_BYTES, planar_sector,
	       ST11_SECTOR_BYTES);

	uint8_t draft[ST11_PHYSICAL_BLOCK_BYTES];
	st_stix_record_t candidate;

	build_stix_block_minor(draft, 2u, 0u, ST11_SLOT_B, ST11_SLOT_A, true, 6u, FIXTURE_SONG_A_START,
				ST11_BLOCKS_PER_SECTOR, 300u, 1u, new_stem, new_song, &candidate);

	uint8_t scratch[ST11_SECTOR_BYTES];

	CHECK(st_ab_session_verify_song_before_commit(&s, &candidate, mock_read_block, NULL, scratch),
	      "v1.2: the full re-read verifies PLANAR media against the SAME per-stem checksums the v1.1 "
	      "layout produced -- the format change moves no checksum");

	/* The padding really is being removed, not ignored: a record claiming
	 * the group is full (340 frames) describes different audio and must
	 * not verify against the same bytes. */
	st_stix_record_t full_group = candidate;

	full_group.frames = ST_PL_FRAMES_PER_GROUP;
	CHECK(!st_ab_session_verify_song_before_commit(&s, &full_group, mock_read_block, NULL, scratch),
	      "v1.2: the same media with a record claiming 340 frames instead of 300 does NOT verify -- "
	      "the tail padding is un-folded by frame count, not guessed");

	st_stix_record_t tampered = candidate;

	tampered.stem_checksums[2] += 1u;
	CHECK(!st_ab_session_verify_song_before_commit(&s, &tampered, mock_read_block, NULL, scratch),
	      "v1.2: one tampered declared stem checksum does not verify against the real planar bytes");

	/* THE FAST PATH must reach the identical verdict from the read-back
	 * bytes the bulk upload already has in hand. */
	CHECK(!st_ab_session_verify_accumulated(&s, &candidate),
	      "v1.2: verify_accumulated with nothing accumulated refuses (never passes vacuously)");

	st_ab_session_accumulate_sector(&s, 0u, planar_sector);
	CHECK(st_ab_session_verify_accumulated(&s, &candidate),
	      "v1.2: the same planar bytes folded one sector at a time reproduce the full re-read's "
	      "verdict exactly");
	CHECK(!st_ab_session_verify_accumulated(&s, &tampered),
	      "v1.2: the tampered candidate is rejected by the fast path too -- not a weaker check");

	st_ab_session_accumulate_sector(&s, 0u, planar_sector);
	CHECK(st_ab_session_verify_accumulated(&s, &candidate),
	      "v1.2: re-accumulating an already-accumulated sector (an idempotent bulk retry) is ignored, "
	      "not double-counted -- still true");

	{
		st_ab_session_t gap = s;

		st_ab_session_accumulate_sector(&gap, 7u, planar_sector); /* skips 1..6 */
		CHECK(!st_ab_session_verify_accumulated(&gap, &candidate),
		      "v1.2: a sector arriving out of order leaves a gap and permanently invalidates the "
		      "accumulation, so the caller falls back to the full re-read");
	}

	/* THE CHECK v1.1 COULD NOT EXPRESS. A v1.1 STSC header could say "I am
	 * sector 7"; it could not say WHICH STEM it was, because one sector held
	 * all four. Here the four groups of the region are well-formed and carry
	 * the right audio -- but two of them claim the same stem, so the region
	 * is not the song it describes. */
	{
		uint8_t swapped[ST11_SECTOR_BYTES];
		st_ab_session_t fresh;

		memcpy(swapped, planar_sector, sizeof(swapped));
		/* group 2 relabelled as stem 1 */
		swapped[2u * ST_PL_GROUP_BYTES + 2u] = 1u;
		memcpy(g_mock_device + (size_t)FIXTURE_SONG_A_START * ST11_PHYSICAL_BLOCK_BYTES, swapped,
		       ST11_SECTOR_BYTES);

		CHECK(st_ab_session_open_replace(&fresh, block_a, block_b, &layout, ST11_BLOCKS_PER_SECTOR) ==
			      ST_AB_OPEN_OK,
		      "v1.2: a fresh session opens over the mislabelled region");
		CHECK(!st_ab_session_verify_song_before_commit(&fresh, &candidate, mock_read_block, NULL,
								scratch),
		      "v1.2: a group carrying the right bytes under the WRONG stem label is refused -- the "
		      "failure mode a v1.1 sector header could not even express");

		st_ab_session_accumulate_sector(&fresh, 0u, swapped);
		CHECK(!st_ab_session_verify_accumulated(&fresh, &candidate),
		      "v1.2: the fast path refuses the mislabelled region too");
	}

	/* A group whose reserved flags byte is set is a format this build does
	 * not understand; reading it as though the byte meant nothing is the
	 * silent misread the version gates exist to prevent. */
	{
		uint8_t flagged[ST11_SECTOR_BYTES];
		st_ab_session_t fresh;

		memcpy(flagged, planar_sector, sizeof(flagged));
		flagged[3u] = 0x01u; /* group 0's reserved flags byte */
		memcpy(g_mock_device + (size_t)FIXTURE_SONG_A_START * ST11_PHYSICAL_BLOCK_BYTES, flagged,
		       ST11_SECTOR_BYTES);

		CHECK(st_ab_session_open_replace(&fresh, block_a, block_b, &layout, ST11_BLOCKS_PER_SECTOR) ==
			      ST_AB_OPEN_OK,
		      "v1.2: a fresh session opens over the flagged region");
		CHECK(!st_ab_session_verify_song_before_commit(&fresh, &candidate, mock_read_block, NULL,
								scratch),
		      "v1.2: a group with a non-zero reserved flags byte is refused, not read anyway");
	}

	/* THE DISPATCH ITSELF, both ways round: each layout verifies only under
	 * the version its record declares. Without this, one of the two paths
	 * could quietly become the only one that ever runs. */
	{
		st_ab_session_t fresh;
		st_stix_record_t as_v11 = candidate;

		memcpy(g_mock_device + (size_t)FIXTURE_SONG_A_START * ST11_PHYSICAL_BLOCK_BYTES, planar_sector,
		       ST11_SECTOR_BYTES);
		as_v11.format_minor = 1u;
		CHECK(st_ab_session_open_replace(&fresh, block_a, block_b, &layout, ST11_BLOCKS_PER_SECTOR) ==
			      ST_AB_OPEN_OK,
		      "v1.2: a fresh session opens for the dispatch checks");
		CHECK(!st_ab_session_verify_song_before_commit(&fresh, &as_v11, mock_read_block, NULL, scratch),
		      "dispatch: PLANAR media under a record declaring v1.1 does not verify");

		memcpy(g_mock_device + (size_t)FIXTURE_SONG_A_START * ST11_PHYSICAL_BLOCK_BYTES, v11_sector,
		       ST11_SECTOR_BYTES);
		CHECK(st_ab_session_verify_song_before_commit(&fresh, &as_v11, mock_read_block, NULL, scratch),
		      "dispatch: the SAME song as v1.1 media under the same v1.1 record verifies -- the two "
		      "layouts carry identical checksums, so only the layout differs");
		CHECK(!st_ab_session_verify_song_before_commit(&fresh, &candidate, mock_read_block, NULL,
								scratch),
		      "dispatch: v1.1 media under a record declaring v1.2 does not verify");

		st_stix_record_t as_v99 = candidate;

		as_v99.format_minor = 99u;
		CHECK(!st_ab_session_verify_song_before_commit(&fresh, &as_v99, mock_read_block, NULL, scratch),
		      "dispatch: a record declaring a minor version this build has never heard of is refused "
		      "outright, not guessed at");
	}
}

int main(void)
{
	RUN(test_song_sectors_fixture);
	RUN(test_index_record_crc_fixture);
	RUN(test_stix_parse_index_a_valid);
	RUN(test_stix_parse_index_b_valid);
	RUN(test_stix_validate_committed_records);
	RUN(test_stix_validate_uncommitted);
	RUN(test_magic_write_cases_case_a_torn_write_no_bytes_applied);
	RUN(test_index_final_magic_block_matches_uncommitted_except_magic);
	RUN(test_corrupt_newest_index_fallback);
	RUN(test_stix_storage_initialized_empty);
	RUN(test_stix_select_active_two_generations);
	RUN(test_stix_read_library_fresh_init);
	RUN(test_stix_read_library_active_generation_three);
	RUN(test_stcp_region_layout_matches_fixture);
	RUN(test_stcp_build_matches_fixture_byte_for_byte);
	RUN(test_region_of_block_matches_fixture_layout);
	RUN(test_ab_session_open_and_negative_writes);
	RUN(test_ab_session_ceiling_allows_smaller_song);
	RUN(test_successive_replacement_matches_fixture);
	RUN(test_ab_session_open_errors);
	RUN(test_ab_session_v12_planar_verification);

	printf("\n");
	printf("%d distinct test cases, %d assertion checks\n", g_test_cases, g_checks);
	if (g_failures) {
		printf("STEM V1.1 FIXTURE CONFORMANCE FAILED (%d of %d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("STEM V1.1 FIXTURE CONFORMANCE PASSED (%d test cases, %d checks)\n", g_test_cases, g_checks);
	return 0;
}
