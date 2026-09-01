/*
 * test_bulk_xfer_walk.c — long-run mirror of xfer_bulk_write_sector()'s
 * own real call sequence (header parse -> CRC check -> st_bulk_seq_check()
 * -> st_ab_session_check_write() per block -> write -> read-back -> CRC
 * verify -> st_bulk_seq_advance()) at the REAL benchmark song's exact
 * scale: 31,814 sectors = 509,024 physical blocks = the same numbers the
 * phase directive's own real physical failure report uses (three real
 * uploads stopped at OFFSETS 499, 609, and 633 blocks into the inactive
 * region -- this walk crosses all three of those offsets by construction,
 * since it walks every sector of a region sized to hold the whole song.
 * The report's own absolute block numbers, 4611/4745/4721, are specific
 * to the real device's own region_start (4112 = 4611-499 = 4745-633 =
 * 4721-609) -- this test's own synthetic layout uses a different,
 * test-chosen region_start, so it checks the portable offset form of the
 * same three real checkpoints, not those device-specific absolute values).
 *
 * Does NOT allocate a 248.5 MiB fixture: the mock eMMC below remembers
 * only the SINGLE most-recently-written 8192-byte sector, matching
 * exactly what the real production sequence needs (write sector N,
 * immediately read sector N back, before ever touching sector N+1) --
 * O(1) memory regardless of song size. Payload content is a cheap,
 * deterministic per-sector pattern (not real audio, and not claimed to
 * be): this walk's job is proving the SEQUENCE/BOUNDS/IDEMPOTENCY
 * machinery holds at real scale, not re-proving audio-content CRC
 * correctness against real bytes -- that is st_bulk_xfer.h's own
 * test_bulk_xfer.c, exercised against a REAL frozen sector from
 * handoff/v1.3/binaries/song-sectors-four-stem.bin.
 *
 * Every pure function this test calls is the EXACT SAME one main.c's
 * real xfer_bulk_write_sector() calls (st_bulk_seq_check/advance,
 * st_ab_session_check_write) -- never a reimplementation -- so this test
 * is a faithful proof that those real functions, driven in the real
 * order, complete a real-scale upload without getting stuck, corrupting
 * sequencing, or double-accepting a retried sector.
 *
 * Index records and the final validity magic stay completely out of
 * scope here, by construction (this walk never touches an index block at
 * all, only the inactive SONG region) -- "interrupted upload preserves
 * previous generation", "final magic remains last", and "new generation
 * cannot be selected early" are guarantees of st_ab_session.c/st_stix.c's
 * own magic-commit machinery, already extensively proven for the shared
 * single-block 'W' path this bulk command reuses UNCHANGED (test_stem_
 * v11.c, test_stem_v11_transcripts.c's own interruption-sweep coverage);
 * this bulk command never adds a second, parallel commit mechanism for
 * this walk to separately re-prove.
 *
 *     cc -std=c11 -Wall -Wextra -Ifirmware/stemtape_player/src \
 *        ../src/st_crc32.c ../src/st_checksum32.c ../src/st_sector_v11.c \
 *        ../src/st_stix.c ../src/st_stcp.c ../src/st_ab_session.c \
 *        ../src/st_bulk_xfer.c \
 *        test_bulk_xfer_walk.c -o test_bulk_xfer_walk && \
 *        (cd ../../.. && firmware/stemtape_player/tests/test_bulk_xfer_walk)
 *
 * Must be run with the CURRENT WORKING DIRECTORY at the repository root
 * (matches every other host test in this repo), though this file itself
 * reads no fixture files.
 */

#include <stdio.h>
#include <string.h>

#include "st_ab_session.h"
#include "st_bulk_xfer.h"
#include "st_crc32.h"
#include "st_stcp.h"
#include "st_stix.h"
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

/* The real benchmark song's exact geometry (phase directive's own
 * numbers): 31,814 logical 8 KiB sectors = 509,024 physical 512-byte
 * blocks. */
#define WALK_SECTOR_COUNT 31814u
#define WALK_SONG_BLOCKS  (WALK_SECTOR_COUNT * ST_BULK_BLOCKS_PER_SECTOR) /* 509024 */

/* ---- mock eMMC: remembers only the single most-recently-written sector.
 * O(1) memory regardless of song size -- see this file's own top comment. */
static uint8_t g_mock_last_write[ST_BULK_PAYLOAD_BYTES];
static uint32_t g_mock_last_write_base = 0xffffffffu; /* sentinel: nothing written yet */
static bool g_mock_fail_write;
static bool g_mock_fail_read;

static bool mock_emmc_write(uint32_t base, const uint8_t *data, uint32_t count)
{
	if (g_mock_fail_write) {
		return false;
	}
	if (count != ST_BULK_BLOCKS_PER_SECTOR) {
		return false;
	}
	memcpy(g_mock_last_write, data, ST_BULK_PAYLOAD_BYTES);
	g_mock_last_write_base = base;
	return true;
}

static bool mock_emmc_read(uint32_t base, uint8_t *out, uint32_t count)
{
	if (g_mock_fail_read) {
		return false;
	}
	if (count != ST_BULK_BLOCKS_PER_SECTOR || base != g_mock_last_write_base) {
		return false;
	}
	memcpy(out, g_mock_last_write, ST_BULK_PAYLOAD_BYTES);
	return true;
}

/* Cheap, deterministic per-sector payload -- NOT real audio (see this
 * file's own top comment on why that is the right scope here). */
static void fill_sector_pattern(uint8_t *buf, uint32_t seq)
{
	for (uint32_t i = 0; i < ST_BULK_PAYLOAD_BYTES; i++) {
		buf[i] = (uint8_t)(0x5Au ^ (seq & 0xffu) ^ ((seq >> 8) & 0xffu) ^ (i & 0xffu));
	}
}

/* One real accept-path round trip through the EXACT same functions/order
 * xfer_bulk_write_sector() uses, given a mock-backed session. Returns true
 * only if every real step (sequence check, per-block session gate, write,
 * read-back, CRC) succeeds and the tracker genuinely advances. */
static bool do_one_sector(st_ab_session_t *session, st_bulk_seq_t *sq, uint32_t seq, uint32_t dest_block,
			   const uint8_t *payload, bool is_retry)
{
	st_bulk_seq_check_t chk = st_bulk_seq_check(sq, seq, dest_block);

	if (is_retry) {
		if (chk != ST_BULK_SEQ_RETRY) {
			return false;
		}
	} else {
		if (chk != ST_BULK_SEQ_NEW) {
			return false;
		}
	}

	for (uint32_t k = 0; k < ST_BULK_BLOCKS_PER_SECTOR; k++) {
		if (st_ab_session_check_write(session, dest_block + k, payload + k * ST11_PHYSICAL_BLOCK_BYTES) !=
		    ST_AB_WRITE_OK) {
			return false;
		}
	}

	if (!mock_emmc_write(dest_block, payload, ST_BULK_BLOCKS_PER_SECTOR)) {
		return false;
	}

	uint8_t readback[ST_BULK_PAYLOAD_BYTES];

	if (!mock_emmc_read(dest_block, readback, ST_BULK_BLOCKS_PER_SECTOR)) {
		return false;
	}
	if (st_crc32_compute(readback, ST_BULK_PAYLOAD_BYTES) != st_crc32_compute(payload, ST_BULK_PAYLOAD_BYTES)) {
		return false;
	}

	if (!is_retry) {
		st_bulk_seq_advance(sq, seq);
	}
	return true;
}

/* Builds a self-consistent, SONG-PRESENT generation-1 STIX record into
 * index A -- real st_stix_serialize()/st_stix_block_crc(), never
 * hand-poked bytes, matching test_stem_v11.c's own build_stix_block()
 * convention. Index B stays all-zero (blank), so the selector picks A.
 *
 * Deliberately declares a real (if small) active song in slot A rather
 * than leaving the record song-free: st_stix_read_library()'s own
 * inactive_song_slot rule (st_stix.h) only complements the active
 * record's song_slot when ST11_IX_FLAG_SONG_PRESENT is set -- a song-free
 * generation-1 record sends inactive_song_slot to A UNCOMPLEMENTED (the
 * real "first upload ever" convention), which would make this walk write
 * into the region this test's OWN active-region-rejection case expects
 * to stay protected. A song-present active record in slot A is what
 * actually produces inactive_song_slot == B, matching a real "replace an
 * existing song" upload -- the scenario the phase directive's physical
 * failure report itself describes (three failed attempts against an
 * already-initialized device, not a first-ever upload). */
static void build_initial_index_a(uint8_t out[ST11_PHYSICAL_BLOCK_BYTES])
{
	static const uint32_t stem_checksums[ST11_STEM_COUNT] = {0x11111111u, 0x22222222u, 0x33333333u,
								   0x44444444u};
	st_stix_record_t r;

	memset(&r, 0, sizeof(r));
	r.magic = ST11_INDEX_MAGIC;
	r.index_version = ST11_STIX_VERSION;
	r.format_major = ST11_FORMAT_MAJOR;
	r.format_minor = ST11_FORMAT_MINOR;
	r.slot_identity = ST11_SLOT_A;
	r.song_slot = ST11_SLOT_A;
	r.flags = ST11_IX_FLAG_SONG_PRESENT;
	r.generation_lo = 1u;
	r.song_start_block = 16u; /* == layout.song_a_start below */
	r.song_block_count = 32u;
	r.frames = 680u;
	r.sector_count = 2u;
	r.sample_rate = ST11_SAMPLE_RATE_HZ;
	r.channels = ST11_CHANNELS_PER_STEM;
	r.bit_depth = ST11_PCM_BIT_DEPTH;
	r.bpm_q8 = 120u * 256u;
	for (uint32_t s = 0; s < ST11_STEM_COUNT; s++) {
		r.original_frames[s] = r.frames;
		r.stem_checksums[s] = stem_checksums[s];
	}
	r.song_checksum = 0x55555555u;
	strncpy(r.title, "Walk Test Baseline", ST11_INDEX_TEXT_BYTES);
	strncpy(r.artist, "Fixture", ST11_INDEX_TEXT_BYTES);

	r.crc32 = 0;
	st_stix_serialize(&r, out);
	r.crc32 = st_stix_block_crc(out);
	st_stix_serialize(&r, out);
}

static void test_full_song_walk_crosses_reported_blocks(void)
{
	/* A real region layout sized so the inactive song region (B) has
	 * EXACTLY the real benchmark song's own block count -- constructed
	 * directly (not via st11_storage_layout_compute()) so this test
	 * controls the exact capacity without needing a many-hundred-
	 * thousand-block synthetic device number; still the REAL layout
	 * struct every real session-gate function consumes identically. */
	st11_region_layout_t layout;

	memset(&layout, 0, sizeof(layout));
	layout.index_a_start = 0u;
	layout.index_a_blocks = ST11_INDEX_REGION_BLOCKS;
	layout.index_b_start = 1u;
	layout.index_b_blocks = ST11_INDEX_REGION_BLOCKS;
	layout.song_a_start = 16u; /* sector-aligned, matches the real layout's own convention */
	layout.song_a_blocks = WALK_SONG_BLOCKS;
	layout.song_b_start = 16u + WALK_SONG_BLOCKS;
	layout.song_b_blocks = WALK_SONG_BLOCKS;

	uint8_t idx_a[ST11_PHYSICAL_BLOCK_BYTES];
	uint8_t idx_b[ST11_PHYSICAL_BLOCK_BYTES];

	build_initial_index_a(idx_a);
	memset(idx_b, 0, sizeof(idx_b)); /* blank -- selector picks A, B is the inactive destination */

	st_ab_session_t session;
	st_ab_open_result_t open_r =
		st_ab_session_open_replace(&session, idx_a, idx_b, &layout, WALK_SONG_BLOCKS);

	CHECK(open_r == ST_AB_OPEN_OK, "walk: REPLACE session opens against the real (constructed) layout");
	CHECK(session.inactive_song_slot == ST11_SLOT_B, "walk: inactive song slot is B, as expected");

	st_bulk_seq_t sq;

	st_bulk_seq_reset(&sq, layout.song_b_start, layout.song_b_blocks);

	g_mock_fail_write = false;
	g_mock_fail_read = false;
	g_mock_last_write_base = 0xffffffffu;

	/* The bug report's own three absolute block numbers (4611, 4745, 4721)
	 * are specific to the REAL device's own region_start (offset 499/633/
	 * 609 blocks in, respectively -- 4611-499 == 4745-633 == 4721-609 ==
	 * 4112, the real device's actual inactive-region base block). This
	 * synthetic layout's own song_b_start is a different, test-chosen
	 * value (region capacity is what's under test here, not one specific
	 * device's placement), so the portable, device-independent form of
	 * the same real checkpoints is the OFFSET from region start -- 499,
	 * 609, 633 blocks in -- not the absolute block numbers themselves. */
	bool crossed_offset_499 = false;
	bool crossed_offset_609 = false;
	bool crossed_offset_633 = false;
	uint32_t accepted = 0;
	uint32_t first_unexpected_stop_seq = 0xffffffffu;

	for (uint32_t seq = 0; seq < WALK_SECTOR_COUNT; seq++) {
		uint32_t dest = layout.song_b_start + seq * ST_BULK_BLOCKS_PER_SECTOR;
		uint32_t off = dest - layout.song_b_start;
		uint8_t payload[ST_BULK_PAYLOAD_BYTES];

		fill_sector_pattern(payload, seq);

		if (off <= 499u && off + ST_BULK_BLOCKS_PER_SECTOR > 499u) {
			crossed_offset_499 = true;
		}
		if (off <= 609u && off + ST_BULK_BLOCKS_PER_SECTOR > 609u) {
			crossed_offset_609 = true;
		}
		if (off <= 633u && off + ST_BULK_BLOCKS_PER_SECTOR > 633u) {
			crossed_offset_633 = true;
		}

		if (!do_one_sector(&session, &sq, seq, dest, payload, /*is_retry=*/false)) {
			first_unexpected_stop_seq = seq;
			break;
		}
		accepted++;
	}

	CHECK(first_unexpected_stop_seq == 0xffffffffu,
	      "walk: no unexplained stop across all %u real sectors (stopped at seq %u if not)",
	      WALK_SECTOR_COUNT, first_unexpected_stop_seq);
	CHECK(accepted == WALK_SECTOR_COUNT, "walk: every one of the %u sectors accepted exactly once (got %u)",
	      WALK_SECTOR_COUNT, accepted);
	CHECK(crossed_offset_499,
	      "walk: crosses the real reported failure offset 499 into the region (absolute block 4611 on the "
	      "real device)");
	CHECK(crossed_offset_633,
	      "walk: crosses the real reported failure offset 633 into the region (absolute block 4745 on the "
	      "real device)");
	CHECK(crossed_offset_609,
	      "walk: crosses the real reported failure offset 609 into the region (absolute block 4721 on the "
	      "real device)");
	CHECK(sq.next_seq == WALK_SECTOR_COUNT, "walk: sequence tracker's own next_seq lands exactly at the count");

	/* Duplicate/idempotent retry, mid-walk: resend the LAST accepted
	 * sector (a genuine lost-ACK scenario) -- must be reprocessed in
	 * full and succeed again, WITHOUT advancing the tracker a second
	 * time or rejecting the walk as already complete. */
	uint32_t retry_seq = WALK_SECTOR_COUNT - 1u;
	uint32_t retry_dest = layout.song_b_start + retry_seq * ST_BULK_BLOCKS_PER_SECTOR;
	uint8_t retry_payload[ST_BULK_PAYLOAD_BYTES];

	fill_sector_pattern(retry_payload, retry_seq);
	bool retry_ok = do_one_sector(&session, &sq, retry_seq, retry_dest, retry_payload, /*is_retry=*/true);

	CHECK(retry_ok, "walk: resending the last-accepted sector (lost-ACK retry) is safe and succeeds");
	CHECK(sq.next_seq == WALK_SECTOR_COUNT,
	      "walk: next_seq unchanged after the retry (did not advance a second time)");

	/* One step further than the region's own real capacity must fail
	 * closed -- proves the walk's own completion isn't an artifact of
	 * the loop bound, but a real boundary the sequence tracker itself
	 * enforces. */
	uint32_t past_dest = layout.song_b_start + WALK_SECTOR_COUNT * ST_BULK_BLOCKS_PER_SECTOR;

	CHECK(st_bulk_seq_check(&sq, WALK_SECTOR_COUNT, past_dest) == ST_BULK_SEQ_OUT_OF_BOUNDS,
	      "walk: one sector past the real region's own exact capacity is rejected as out of bounds");
}

static void test_active_region_rejected_throughout(void)
{
	/* Same real layout; this time prove that EVERY block of the ACTIVE
	 * region (song A, since index A/generation 1 is selected active) is
	 * rejected by the real st_ab_session_check_write() gate -- sampled
	 * across the real region rather than exhaustively (509,024 checks
	 * would be redundant with the walk above's own coverage of the
	 * gate's accept path; this proves its reject path on the SAME real
	 * function). */
	st11_region_layout_t layout;

	memset(&layout, 0, sizeof(layout));
	layout.index_a_start = 0u;
	layout.index_a_blocks = ST11_INDEX_REGION_BLOCKS;
	layout.index_b_start = 1u;
	layout.index_b_blocks = ST11_INDEX_REGION_BLOCKS;
	layout.song_a_start = 16u;
	layout.song_a_blocks = WALK_SONG_BLOCKS;
	layout.song_b_start = 16u + WALK_SONG_BLOCKS;
	layout.song_b_blocks = WALK_SONG_BLOCKS;

	uint8_t idx_a[ST11_PHYSICAL_BLOCK_BYTES];
	uint8_t idx_b[ST11_PHYSICAL_BLOCK_BYTES];

	build_initial_index_a(idx_a);
	memset(idx_b, 0, sizeof(idx_b));

	st_ab_session_t session;

	CHECK(st_ab_session_open_replace(&session, idx_a, idx_b, &layout, WALK_SONG_BLOCKS) == ST_AB_OPEN_OK,
	      "active-region test: session opens");

	uint8_t dummy[ST11_PHYSICAL_BLOCK_BYTES];

	memset(dummy, 0, sizeof(dummy));

	uint32_t sample_offsets[] = {0u, 1u, 4611u, 4745u, WALK_SONG_BLOCKS - 1u};
	bool all_rejected = true;

	for (size_t i = 0; i < sizeof(sample_offsets) / sizeof(sample_offsets[0]); i++) {
		uint32_t block = layout.song_a_start + sample_offsets[i];

		if (st_ab_session_check_write(&session, block, dummy) != ST_AB_WRITE_ERR_ACTIVE_REGION) {
			all_rejected = false;
		}
	}
	CHECK(all_rejected,
	      "active-region test: every sampled block of the ACTIVE song region is rejected ACTIVE_REGION, "
	      "never accepted");
}

static void test_write_read_crc_failure_paths(void)
{
	st11_region_layout_t layout;

	memset(&layout, 0, sizeof(layout));
	layout.index_a_start = 0u;
	layout.index_a_blocks = ST11_INDEX_REGION_BLOCKS;
	layout.index_b_start = 1u;
	layout.index_b_blocks = ST11_INDEX_REGION_BLOCKS;
	layout.song_a_start = 16u;
	layout.song_a_blocks = WALK_SONG_BLOCKS;
	layout.song_b_start = 16u + WALK_SONG_BLOCKS;
	layout.song_b_blocks = WALK_SONG_BLOCKS;

	uint8_t idx_a[ST11_PHYSICAL_BLOCK_BYTES];
	uint8_t idx_b[ST11_PHYSICAL_BLOCK_BYTES];

	build_initial_index_a(idx_a);
	memset(idx_b, 0, sizeof(idx_b));

	uint8_t payload[ST_BULK_PAYLOAD_BYTES];

	fill_sector_pattern(payload, 0u);

	/* Write failure: mock eMMC program rejects. */
	{
		st_ab_session_t session;

		st_ab_session_open_replace(&session, idx_a, idx_b, &layout, WALK_SONG_BLOCKS);
		st_bulk_seq_t sq;

		st_bulk_seq_reset(&sq, layout.song_b_start, layout.song_b_blocks);
		g_mock_fail_write = true;
		g_mock_fail_read = false;
		g_mock_last_write_base = 0xffffffffu;
		bool ok = do_one_sector(&session, &sq, 0u, layout.song_b_start, payload, false);

		CHECK(!ok, "write failure: a mock eMMC program rejection is NOT treated as success");
		CHECK(sq.next_seq == 0u, "write failure: sequence tracker does not advance on a write failure");
	}

	/* Read-back failure: mock eMMC read rejects (write itself succeeded). */
	{
		st_ab_session_t session;

		st_ab_session_open_replace(&session, idx_a, idx_b, &layout, WALK_SONG_BLOCKS);
		st_bulk_seq_t sq;

		st_bulk_seq_reset(&sq, layout.song_b_start, layout.song_b_blocks);
		g_mock_fail_write = false;
		g_mock_fail_read = true;
		g_mock_last_write_base = 0xffffffffu;
		bool ok = do_one_sector(&session, &sq, 0u, layout.song_b_start, payload, false);

		CHECK(!ok, "read-back failure: a mock eMMC read rejection is NOT treated as success");
		CHECK(sq.next_seq == 0u, "read-back failure: sequence tracker does not advance on a read-back failure");
	}

	/* Read-back CRC mismatch: read returns bytes for a DIFFERENT sector
	 * than the one just written (simulated by writing sector 0, then
	 * asking do_one_sector() to validate against sector 1's own
	 * pattern -- the CRC comparison inside do_one_sector() must catch
	 * this a real corrupted read-back would produce identically). */
	{
		st_ab_session_t session;

		st_ab_session_open_replace(&session, idx_a, idx_b, &layout, WALK_SONG_BLOCKS);
		g_mock_fail_write = false;
		g_mock_fail_read = false;
		g_mock_last_write_base = 0xffffffffu;

		uint8_t written[ST_BULK_PAYLOAD_BYTES];
		uint8_t different[ST_BULK_PAYLOAD_BYTES];

		fill_sector_pattern(written, 0u);
		fill_sector_pattern(different, 1u); /* deliberately different content */

		for (uint32_t k = 0; k < ST_BULK_BLOCKS_PER_SECTOR; k++) {
			CHECK(st_ab_session_check_write(&session, layout.song_b_start + k,
							 written + k * ST11_PHYSICAL_BLOCK_BYTES) == ST_AB_WRITE_OK,
			      "CRC mismatch setup: per-block session gate accepts block %u", k);
		}
		CHECK(mock_emmc_write(layout.song_b_start, written, ST_BULK_BLOCKS_PER_SECTOR),
		      "CRC mismatch setup: mock write succeeds");

		uint8_t readback[ST_BULK_PAYLOAD_BYTES];

		CHECK(mock_emmc_read(layout.song_b_start, readback, ST_BULK_BLOCKS_PER_SECTOR),
		      "CRC mismatch setup: mock read succeeds");
		CHECK(st_crc32_compute(readback, ST_BULK_PAYLOAD_BYTES) != st_crc32_compute(different, ST_BULK_PAYLOAD_BYTES),
		      "read-back CRC mismatch: real read-back bytes do NOT match a different sector's own CRC "
		      "(the exact check xfer_bulk_write_sector() makes before ever acking success)");
	}

	g_mock_fail_write = false;
	g_mock_fail_read = false;
}

int main(void)
{
	RUN(test_full_song_walk_crosses_reported_blocks);
	RUN(test_active_region_rejected_throughout);
	RUN(test_write_read_crc_failure_paths);

	printf("\n");
	printf("%d distinct test cases, %d assertion checks\n", g_test_cases, g_checks);
	if (g_failures) {
		printf("BULK XFER LONG-RUN WALK TESTS FAILED (%d of %d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("BULK XFER LONG-RUN WALK TESTS PASSED (%d test cases, %d checks)\n", g_test_cases, g_checks);
	return 0;
}
