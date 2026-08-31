/*
 * test_bulk_xfer.c — host tests for st_bulk_xfer.h/.c: the pure wire-
 * contract parser/builder and per-session sequence state machine behind
 * the new 'U' bulk verified-sector upload command.
 *
 * Uses the SAME real frozen handoff fixture (handoff/v1.1/binaries/
 * song-sectors-four-stem.bin) and the SAME real region geometry
 * (handoff/v1.1/decoded/stcp-capability-response.json's synthetic
 * 272-block device: songA [16,144), songB [144,272)) test_stem_v11.c
 * already trusts -- never a fabricated fixture or invented geometry.
 *
 *     cc -std=c11 -Wall -Wextra -I../src \
 *        ../src/st_crc32.c ../src/st_bulk_xfer.c \
 *        test_bulk_xfer.c -o test_bulk_xfer && \
 *        (cd ../../.. && firmware/stemtape_player/tests/test_bulk_xfer)
 *
 * Must be run with the CURRENT WORKING DIRECTORY at the repository root,
 * matching every other host test in this repo.
 *
 * Covers, of the phase directive's required VERIFICATION list, every
 * case that is genuinely pure (parser/state, no I/O, no eMMC harness):
 *   - a real frozen 8192-byte companion sector through the exact request
 *     parser;
 *   - CRC detection over real (and real-but-corrupted) payload bytes;
 *   - out-of-order transaction rejection;
 *   - duplicate/idempotent retry;
 *   - destination bounds rejection for every A/B region edge.
 * The remaining required cases (truncated-payload resync, active-region
 * rejection via a real st_ab_session, real 16-block write/read-back/CRC
 * success, write/read/CRC failure responses, interruption fixtures, the
 * full 31,814-sector walk) need the real command handler and storage
 * harness -- added in the slices that build those (main.c wiring, then
 * commit/reload integration), not here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "st_bulk_xfer.h"
#include "st_crc32.h"
#include "st_v11_format.h"

/* Real region geometry from handoff/v1.1/decoded/stcp-capability-response.json
 * (the same synthetic 272-block test device every handoff/v1.1/binaries/
 * fixture was generated against) -- matches test_stem_v11.c's own constants
 * exactly, never independently invented. */
#define FIXTURE_SONG_A_START  16u
#define FIXTURE_SONG_A_BLOCKS 128u
#define FIXTURE_SONG_B_START  144u
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
 * Request header round-trip + a REAL frozen sector through the parser.
 * ======================================================================== */

static void test_header_round_trip_arbitrary_values(void)
{
	uint8_t wire[ST_BULK_REQ_HEADER_BYTES];

	wire[0] = ST_BULK_PROTO_VERSION;
	wire[1] = 0x11; wire[2] = 0x22; wire[3] = 0x33; wire[4] = 0x44;        /* seq */
	wire[5] = 0x90; wire[6] = 0x01; wire[7] = 0x00; wire[8] = 0x00;        /* dest_block */
	wire[9] = 0x00; wire[10] = 0x20; wire[11] = 0x00; wire[12] = 0x00;     /* payload_len */
	wire[13] = 0xEF; wire[14] = 0xBE; wire[15] = 0xAD; wire[16] = 0xDE;    /* payload_crc32 */

	st_bulk_req_header_t hdr;

	st_bulk_parse_header(wire, &hdr);
	CHECK(hdr.version == ST_BULK_PROTO_VERSION, "header round trip: version");
	CHECK(hdr.seq == 0x44332211u, "header round trip: seq little-endian decode");
	CHECK(hdr.dest_block == 0x00000190u, "header round trip: dest_block little-endian decode");
	CHECK(hdr.payload_len == 0x00002000u, "header round trip: payload_len little-endian decode (8192)");
	CHECK(hdr.payload_crc32 == 0xDEADBEEFu, "header round trip: payload_crc32 little-endian decode");
	CHECK(hdr.payload_len == ST_BULK_PAYLOAD_BYTES, "header round trip: 8192 == ST_BULK_PAYLOAD_BYTES");
}

static void test_real_frozen_sector_through_parser(void)
{
	size_t len;
	uint8_t *song = read_fixture("handoff/v1.1/binaries/song-sectors-four-stem.bin", &len);

	CHECK(len == 43u * ST_BULK_PAYLOAD_BYTES, "song-sectors-four-stem.bin is exactly 43 real STSC sectors");

	/* Sector 0's own real bytes -- the exact payload a real companion
	 * would send in a 'U' request for the first sector of a real song. */
	const uint8_t *real_sector0 = song;
	uint32_t real_crc = st_crc32_compute(real_sector0, ST_BULK_PAYLOAD_BYTES);

	uint8_t wire[ST_BULK_REQ_HEADER_BYTES];

	wire[ST_BULK_REQ_OFF_VERSION] = ST_BULK_PROTO_VERSION;
	wire[ST_BULK_REQ_OFF_SEQ] = 0; wire[ST_BULK_REQ_OFF_SEQ + 1] = 0;
	wire[ST_BULK_REQ_OFF_SEQ + 2] = 0; wire[ST_BULK_REQ_OFF_SEQ + 3] = 0;
	wire[ST_BULK_REQ_OFF_DEST_BLOCK] = (uint8_t)FIXTURE_SONG_A_START;
	wire[ST_BULK_REQ_OFF_DEST_BLOCK + 1] = 0; wire[ST_BULK_REQ_OFF_DEST_BLOCK + 2] = 0;
	wire[ST_BULK_REQ_OFF_DEST_BLOCK + 3] = 0;
	wire[ST_BULK_REQ_OFF_PAYLOAD_LEN] = (uint8_t)(ST_BULK_PAYLOAD_BYTES & 0xffu);
	wire[ST_BULK_REQ_OFF_PAYLOAD_LEN + 1] = (uint8_t)((ST_BULK_PAYLOAD_BYTES >> 8) & 0xffu);
	wire[ST_BULK_REQ_OFF_PAYLOAD_LEN + 2] = 0; wire[ST_BULK_REQ_OFF_PAYLOAD_LEN + 3] = 0;
	wire[ST_BULK_REQ_OFF_PAYLOAD_CRC32] = (uint8_t)(real_crc & 0xffu);
	wire[ST_BULK_REQ_OFF_PAYLOAD_CRC32 + 1] = (uint8_t)((real_crc >> 8) & 0xffu);
	wire[ST_BULK_REQ_OFF_PAYLOAD_CRC32 + 2] = (uint8_t)((real_crc >> 16) & 0xffu);
	wire[ST_BULK_REQ_OFF_PAYLOAD_CRC32 + 3] = (uint8_t)((real_crc >> 24) & 0xffu);

	st_bulk_req_header_t hdr;

	st_bulk_parse_header(wire, &hdr);
	CHECK(hdr.version == ST_BULK_PROTO_VERSION, "real sector 0: parsed version matches");
	CHECK(hdr.seq == 0u, "real sector 0: parsed seq == 0");
	CHECK(hdr.dest_block == FIXTURE_SONG_A_START, "real sector 0: parsed dest_block == real song A start");
	CHECK(hdr.payload_len == ST_BULK_PAYLOAD_BYTES, "real sector 0: parsed payload_len == 8192");
	CHECK(hdr.payload_crc32 == real_crc,
	      "real sector 0: parsed payload_crc32 matches CRC-32 actually computed over the real fixture bytes");

	/* The receiver-side check this proves the DATA half of: recomputing
	 * CRC-32 over the SAME real bytes must reproduce the declared value
	 * exactly -- the real accept/reject decision itself lives in the
	 * command handler (main.c), which is not host-testable without the
	 * real CDC/eMMC environment; this is the pure half of that check. */
	CHECK(st_crc32_compute(real_sector0, ST_BULK_PAYLOAD_BYTES) == hdr.payload_crc32,
	      "real sector 0: recomputed CRC-32 over the real bytes matches the parsed declaration");

	free(song);
}

static void test_crc_detects_real_corruption(void)
{
	size_t len;
	uint8_t *song = read_fixture("handoff/v1.1/binaries/song-sectors-four-stem.bin", &len);
	uint32_t original_crc = st_crc32_compute(song, ST_BULK_PAYLOAD_BYTES);

	uint8_t corrupted[ST_BULK_PAYLOAD_BYTES];

	memcpy(corrupted, song, ST_BULK_PAYLOAD_BYTES);
	corrupted[4096] ^= 0x01u; /* single-bit flip, real bytes otherwise unchanged */

	uint32_t corrupted_crc = st_crc32_compute(corrupted, ST_BULK_PAYLOAD_BYTES);

	CHECK(corrupted_crc != original_crc,
	      "CRC-32 detects a single-bit corruption of a real sector's payload bytes");

	free(song);
}

/* ========================================================================
 * Response build/decode.
 * ======================================================================== */

static uint32_t rd_u32le_test(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void test_response_build_ok(void)
{
	uint8_t out[ST_BULK_RESP_BYTES];

	st_bulk_build_response(ST_BULK_OK, 7u, FIXTURE_SONG_A_START, 0xCAFEBABEu, out);
	CHECK(out[ST_BULK_RESP_OFF_STATUS] == ST_BULK_OK, "OK response: status byte == 0");
	CHECK(rd_u32le_test(out + ST_BULK_RESP_OFF_SEQ) == 7u, "OK response: seq echoed");
	CHECK(rd_u32le_test(out + ST_BULK_RESP_OFF_DEST_BLOCK) == FIXTURE_SONG_A_START,
	      "OK response: dest_block echoed");
	CHECK(rd_u32le_test(out + ST_BULK_RESP_OFF_VERIFIED_CRC) == 0xCAFEBABEu,
	      "OK response: verified_crc32 carries the actual read-back CRC");
	CHECK(out[ST_BULK_RESP_OFF_RETRYABLE] == 0u, "OK response: retryable is 0 (success, not applicable)");
}

static void test_response_retryable_classification_every_status(void)
{
	/* Every status this contract defines gets a response built and its
	 * retryable byte cross-checked against st_bulk_status_is_retryable()
	 * directly -- proves the two never disagree, for every real value. */
	st_bulk_status_t all[] = {
		ST_BULK_OK, ST_BULK_ERR_UNSUPPORTED_VERSION, ST_BULK_ERR_BAD_LENGTH,
		ST_BULK_ERR_TIMEOUT_PAYLOAD, ST_BULK_ERR_CDC_OVERFLOW, ST_BULK_ERR_CRC_MISMATCH,
		ST_BULK_ERR_LAYOUT_NOT_READY, ST_BULK_ERR_NO_SESSION, ST_BULK_ERR_SESSION_CLOSED,
		ST_BULK_ERR_OUT_OF_SEQUENCE, ST_BULK_ERR_DEST_MISMATCH, ST_BULK_ERR_OUT_OF_BOUNDS,
		ST_BULK_ERR_ACTIVE_REGION, ST_BULK_ERR_OUTSIDE_FROZEN_PAIR, ST_BULK_ERR_EMMC_WRITE_FAIL,
		ST_BULK_ERR_EMMC_READBACK_FAIL, ST_BULK_ERR_READBACK_CRC_MISMATCH,
	};

	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		uint8_t out[ST_BULK_RESP_BYTES];

		st_bulk_build_response(all[i], 0u, 0u, 0u, out);
		bool expected = st_bulk_status_is_retryable(all[i]);
		bool got = out[ST_BULK_RESP_OFF_RETRYABLE] != 0u;

		CHECK(got == expected, "response retryable byte matches st_bulk_status_is_retryable() for status %d",
		      (int)all[i]);
	}

	/* The specific classification the phase directive itself calls out:
	 * transient/physical failures retryable, structural/protocol ones not. */
	CHECK(st_bulk_status_is_retryable(ST_BULK_ERR_CRC_MISMATCH), "CRC mismatch is retryable (resend the same bytes)");
	CHECK(st_bulk_status_is_retryable(ST_BULK_ERR_EMMC_WRITE_FAIL), "eMMC write failure is retryable");
	CHECK(st_bulk_status_is_retryable(ST_BULK_ERR_EMMC_READBACK_FAIL), "eMMC read-back failure is retryable");
	CHECK(st_bulk_status_is_retryable(ST_BULK_ERR_READBACK_CRC_MISMATCH), "read-back CRC mismatch is retryable");
	CHECK(!st_bulk_status_is_retryable(ST_BULK_ERR_OUT_OF_SEQUENCE),
	      "out-of-sequence is NOT retryable (host must fix its own state)");
	CHECK(!st_bulk_status_is_retryable(ST_BULK_ERR_ACTIVE_REGION), "active-region rejection is NOT retryable");
	CHECK(!st_bulk_status_is_retryable(ST_BULK_ERR_SESSION_CLOSED), "session-closed is NOT retryable");
}

/* ========================================================================
 * Sequence state machine: fresh session, new/retry/out-of-order/dest-
 * mismatch/bounds, using the REAL song A and song B region edges.
 * ======================================================================== */

static void test_seq_fresh_session_expects_zero_at_region_start(void)
{
	st_bulk_seq_t sq;

	st_bulk_seq_reset(&sq, FIXTURE_SONG_B_START, FIXTURE_SONG_B_BLOCKS);
	CHECK(st_bulk_seq_check(&sq, 0u, FIXTURE_SONG_B_START) == ST_BULK_SEQ_NEW,
	      "fresh session: seq 0 at region_start is a genuinely new sector");
	CHECK(st_bulk_seq_check(&sq, 1u, FIXTURE_SONG_B_START + ST_BULK_BLOCKS_PER_SECTOR) == ST_BULK_SEQ_OUT_OF_ORDER,
	      "fresh session: seq 1 before seq 0 was ever accepted is out of order");
}

static void test_seq_advance_then_retry_is_idempotent(void)
{
	st_bulk_seq_t sq;

	st_bulk_seq_reset(&sq, FIXTURE_SONG_A_START, FIXTURE_SONG_A_BLOCKS);
	CHECK(st_bulk_seq_check(&sq, 0u, FIXTURE_SONG_A_START) == ST_BULK_SEQ_NEW, "seq 0: new");
	st_bulk_seq_advance(&sq, 0u);
	CHECK(sq.next_seq == 1u, "after advancing seq 0, next_seq == 1");

	/* Lost-ACK retry: host resends seq 0 because it never saw our ack. */
	CHECK(st_bulk_seq_check(&sq, 0u, FIXTURE_SONG_A_START) == ST_BULK_SEQ_RETRY,
	      "resending the just-accepted seq 0 at the same destination is a legal retry");
	/* A retry must NEVER advance the tracker again. */
	st_bulk_seq_check(&sq, 0u, FIXTURE_SONG_A_START); /* pure -- calling it again changes nothing */
	CHECK(sq.next_seq == 1u, "next_seq unchanged after re-checking (but not advancing) a retry");

	uint32_t seq1_dest = FIXTURE_SONG_A_START + ST_BULK_BLOCKS_PER_SECTOR;

	CHECK(st_bulk_seq_check(&sq, 1u, seq1_dest) == ST_BULK_SEQ_NEW, "seq 1 at the right destination: new");
	st_bulk_seq_advance(&sq, 1u);
	CHECK(sq.next_seq == 2u, "after advancing seq 1, next_seq == 2");

	/* Now seq 0 is neither next_seq (2) nor next_seq-1 (1): a retry of a
	 * STALE (not the most recent) sector must fail closed, never be
	 * silently accepted twice removed from the frontier. */
	CHECK(st_bulk_seq_check(&sq, 0u, FIXTURE_SONG_A_START) == ST_BULK_SEQ_OUT_OF_ORDER,
	      "resending a STALE (not-most-recent) seq after the frontier moved on is out of order");
	/* And the current retry-eligible one is exactly seq 1. */
	CHECK(st_bulk_seq_check(&sq, 1u, seq1_dest) == ST_BULK_SEQ_RETRY, "seq 1 is now the sole legal retry");
}

static void test_seq_advance_only_moves_frontier_for_the_new_seq(void)
{
	st_bulk_seq_t sq;

	st_bulk_seq_reset(&sq, FIXTURE_SONG_A_START, FIXTURE_SONG_A_BLOCKS);
	/* Advancing with the WRONG seq (not next_seq) must be a defensive no-op. */
	st_bulk_seq_advance(&sq, 5u);
	CHECK(sq.next_seq == 0u && !sq.has_committed,
	      "st_bulk_seq_advance() with a seq != next_seq is a no-op (defensive)");
}

static void test_seq_dest_mismatch(void)
{
	st_bulk_seq_t sq;

	st_bulk_seq_reset(&sq, FIXTURE_SONG_B_START, FIXTURE_SONG_B_BLOCKS);
	CHECK(st_bulk_seq_check(&sq, 0u, FIXTURE_SONG_B_START + 1u) == ST_BULK_SEQ_DEST_MISMATCH,
	      "seq 0 with a destination one block off region_start is a dest mismatch");
	CHECK(st_bulk_seq_check(&sq, 0u, FIXTURE_SONG_A_START) == ST_BULK_SEQ_DEST_MISMATCH,
	      "seq 0 pointed at the OTHER region's start is a dest mismatch, not accepted as if it were valid");
}

static void test_seq_bounds_every_region_edge(void)
{
	/* Song A: 128 blocks / 16 blocks-per-sector = exactly 8 sectors,
	 * seq 0..7 valid, seq 8 (one sector past the region's own capacity)
	 * out of bounds -- the real edge from the real fixture geometry. */
	st_bulk_seq_t sq_a;

	st_bulk_seq_reset(&sq_a, FIXTURE_SONG_A_START, FIXTURE_SONG_A_BLOCKS);
	uint32_t last_valid_seq_a = (FIXTURE_SONG_A_BLOCKS / ST_BULK_BLOCKS_PER_SECTOR) - 1u; /* 7 */

	for (uint32_t s = 0; s < last_valid_seq_a; s++) {
		st_bulk_seq_advance(&sq_a, s);
	}
	uint32_t last_dest_a = FIXTURE_SONG_A_START + last_valid_seq_a * ST_BULK_BLOCKS_PER_SECTOR;

	CHECK(st_bulk_seq_check(&sq_a, last_valid_seq_a, last_dest_a) == ST_BULK_SEQ_NEW,
	      "song A region: the LAST valid sector (seq 7, fills the region exactly) is accepted");
	st_bulk_seq_advance(&sq_a, last_valid_seq_a);
	uint32_t one_past_dest_a = last_dest_a + ST_BULK_BLOCKS_PER_SECTOR;

	CHECK(st_bulk_seq_check(&sq_a, last_valid_seq_a + 1u, one_past_dest_a) == ST_BULK_SEQ_OUT_OF_BOUNDS,
	      "song A region: one sector past the region's own capacity is rejected as out of bounds");

	/* Song B: same shape, independently checked against ITS OWN real
	 * edge -- proves the bounds check is genuinely parameterized per
	 * session, not hardcoded to song A's numbers. */
	st_bulk_seq_t sq_b;

	st_bulk_seq_reset(&sq_b, FIXTURE_SONG_B_START, FIXTURE_SONG_B_BLOCKS);
	uint32_t last_valid_seq_b = (FIXTURE_SONG_B_BLOCKS / ST_BULK_BLOCKS_PER_SECTOR) - 1u; /* 7 */

	for (uint32_t s = 0; s < last_valid_seq_b; s++) {
		st_bulk_seq_advance(&sq_b, s);
	}
	uint32_t last_dest_b = FIXTURE_SONG_B_START + last_valid_seq_b * ST_BULK_BLOCKS_PER_SECTOR;

	CHECK(st_bulk_seq_check(&sq_b, last_valid_seq_b, last_dest_b) == ST_BULK_SEQ_NEW,
	      "song B region: the LAST valid sector is accepted at its own real edge");
	st_bulk_seq_advance(&sq_b, last_valid_seq_b);
	uint32_t one_past_dest_b = last_dest_b + ST_BULK_BLOCKS_PER_SECTOR;

	CHECK(st_bulk_seq_check(&sq_b, last_valid_seq_b + 1u, one_past_dest_b) == ST_BULK_SEQ_OUT_OF_BOUNDS,
	      "song B region: one sector past ITS OWN real capacity is rejected as out of bounds");
}

/* ========================================================================
 * Q/STCP capability extension.
 * ======================================================================== */

static void test_caps_extension_build(void)
{
	uint8_t out[ST_BULK_CAPS_BYTES];

	st_bulk_build_caps(out);
	CHECK(out[ST_BULK_CAPS_OFF_TAG + 0] == 'S' && out[ST_BULK_CAPS_OFF_TAG + 1] == 'T' &&
		      out[ST_BULK_CAPS_OFF_TAG + 2] == 'B' && out[ST_BULK_CAPS_OFF_TAG + 3] == 'C',
	      "capability extension: tag is literal ASCII 'STBC'");
	CHECK(rd_u32le_test(out + ST_BULK_CAPS_OFF_FLAGS) == ST_BULK_CAP_FLAG_SUPPORTED,
	      "capability extension: flags == ST_BULK_CAP_FLAG_SUPPORTED");
	CHECK(rd_u32le_test(out + ST_BULK_CAPS_OFF_MAX_SECTOR_BYTES) == ST_BULK_PAYLOAD_BYTES,
	      "capability extension: max_sector_bytes == 8192 (ST_BULK_PAYLOAD_BYTES)");
}

int main(void)
{
	RUN(test_header_round_trip_arbitrary_values);
	RUN(test_real_frozen_sector_through_parser);
	RUN(test_crc_detects_real_corruption);
	RUN(test_response_build_ok);
	RUN(test_response_retryable_classification_every_status);
	RUN(test_seq_fresh_session_expects_zero_at_region_start);
	RUN(test_seq_advance_then_retry_is_idempotent);
	RUN(test_seq_advance_only_moves_frontier_for_the_new_seq);
	RUN(test_seq_dest_mismatch);
	RUN(test_seq_bounds_every_region_edge);
	RUN(test_caps_extension_build);

	printf("\n");
	printf("%d distinct test cases, %d assertion checks\n", g_test_cases, g_checks);
	if (g_failures) {
		printf("BULK XFER WIRE/STATE TESTS FAILED (%d of %d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("BULK XFER WIRE/STATE TESTS PASSED (%d test cases, %d checks)\n", g_test_cases, g_checks);
	return 0;
}
