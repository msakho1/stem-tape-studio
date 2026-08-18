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
	uint8_t *block = read_fixture("handoff/v1.1/binaries/index-a-valid.bin", &len);
	st_stix_record_t r;

	st_stix_deserialize(block, &r);

	CHECK(r.magic == ST11_INDEX_MAGIC, "index-a-valid.bin: magic == 'STIX'");
	CHECK(r.index_version == 2u, "index-a-valid.bin: indexVersion == 2");
	CHECK(r.format_major == 1u && r.format_minor == 1u, "index-a-valid.bin: format 1.1");
	CHECK(r.slot_identity == ST11_SLOT_A, "index-a-valid.bin: slotIdentity == A");
	CHECK(r.song_slot == ST11_SLOT_B, "index-a-valid.bin: songSlot == B");
	CHECK((r.flags & ST11_IX_FLAG_SONG_PRESENT) != 0u, "index-a-valid.bin: SONG_PRESENT set");
	CHECK(r.generation_lo == 3u && r.generation_hi == 0u, "index-a-valid.bin: generation == 3");
	CHECK(r.song_start_block == 144u, "index-a-valid.bin: songStartBlock == 144");
	CHECK(r.song_block_count == 32u, "index-a-valid.bin: songBlockCount == 32");
	CHECK(r.frames == 680u, "index-a-valid.bin: frames == 680");
	CHECK(r.sector_count == 2u, "index-a-valid.bin: sectorCount == 2");
	CHECK(r.sample_rate == 48000u && r.channels == 2u && r.bit_depth == 24u,
	      "index-a-valid.bin: 48kHz/stereo/24-bit");
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
	CHECK(r.crc32 == 888519033u, "index-a-valid.bin: crc32 field == 888519033");

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
	uint8_t *block = read_fixture("handoff/v1.1/binaries/index-b-valid.bin", &len);
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
	CHECK(r.crc32 == 720762313u, "index-b-valid.bin: crc32 field == 720762313");

	free(block);
}

static void test_stix_validate_committed_records(void)
{
	size_t len_a, len_b;
	uint8_t *block_a = read_fixture("handoff/v1.1/binaries/index-a-valid.bin", &len_a);
	uint8_t *block_b = read_fixture("handoff/v1.1/binaries/index-b-valid.bin", &len_b);
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
	uint8_t *block = read_fixture("handoff/v1.1/binaries/index-uncommitted.bin", &len);
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

static void test_stix_storage_initialized_empty(void)
{
	size_t len;
	uint8_t *data = read_fixture("handoff/v1.1/binaries/storage-initialized-empty.bin", &len);

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
	uint8_t *block_a = read_fixture("handoff/v1.1/binaries/index-a-valid.bin", &len_a);
	uint8_t *block_b = read_fixture("handoff/v1.1/binaries/index-b-valid.bin", &len_b);
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
	uint8_t *data = read_fixture("handoff/v1.1/binaries/storage-initialized-empty.bin", &len);
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
	uint8_t *block_a = read_fixture("handoff/v1.1/binaries/index-a-valid.bin", &len_a);
	uint8_t *block_b = read_fixture("handoff/v1.1/binaries/index-b-valid.bin", &len_b);
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
 * handoff/v1.1/binaries/stcp-capability-response.bin.
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
	uint8_t *fixture = read_fixture("handoff/v1.1/binaries/stcp-capability-response.bin", &len);

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
	CHECK(parsed.proto_major == 1u && parsed.proto_minor == 1u, "parsed protocol == 1.1");
	CHECK(parsed.format_major == 1u && parsed.format_minor == 1u, "parsed format == 1.1");
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

int main(void)
{
	RUN(test_song_sectors_fixture);
	RUN(test_index_record_crc_fixture);
	RUN(test_stix_parse_index_a_valid);
	RUN(test_stix_parse_index_b_valid);
	RUN(test_stix_validate_committed_records);
	RUN(test_stix_validate_uncommitted);
	RUN(test_stix_storage_initialized_empty);
	RUN(test_stix_select_active_two_generations);
	RUN(test_stix_read_library_fresh_init);
	RUN(test_stix_read_library_active_generation_three);
	RUN(test_stcp_region_layout_matches_fixture);
	RUN(test_stcp_build_matches_fixture_byte_for_byte);

	printf("\n");
	printf("%d distinct test cases, %d assertion checks\n", g_test_cases, g_checks);
	if (g_failures) {
		printf("STEM V1.1 FIXTURE CONFORMANCE FAILED (%d of %d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("STEM V1.1 FIXTURE CONFORMANCE PASSED (%d test cases, %d checks)\n", g_test_cases, g_checks);
	return 0;
}
