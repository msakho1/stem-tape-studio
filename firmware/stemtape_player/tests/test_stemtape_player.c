/*
 * test_stemtape_player.c — Stem Tape standalone player: host-runnable tests
 * for every PURE module (st_crc32, st_transfer, st_storage_layout,
 * st_gesture, st_scrub, st_led_pattern, st_midi_queue).
 *
 *     cc -std=c11 -Wall -Wextra -I../src \
 *        ../src/st_crc32.c ../src/st_transfer.c ../src/st_gesture.c \
 *        ../src/st_scrub.c ../src/st_led_pattern.c ../src/st_midi_queue.c \
 *        test_stemtape_player.c -lm -o test_stemtape_player && \
 *        ./test_stemtape_player
 *
 * Same self-checking pattern as firmware/stemtape/tests/test_led.c: [OK]/
 * [FAIL] per assertion, nonzero exit on any failure.
 *
 * GATE 1's st_midi_queue tests (test_midi_*) drive the SAME two functions
 * (st_midi_decode_ump32(), st_midi_queue_push()/pop()) that main.c's real
 * USB-MIDI rx_packet_cb calls -- see st_midi_queue.h's header comment.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "st_crc32.h"
#include "st_fx_catalog.h"
#include "st_gesture.h"
#include "st_led_pattern.h"
#include "st_midi_queue.h"
#include "st_scrub.h"
#include "st_sector_codec.h"
#include "st_stem_validate.h"
#include "st_storage_layout.h"
#include "st_transfer.h"
#include "st_transfer_protocol.h"

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

/* One "distinct test case" per RUN(), reported separately from the raw
 * assertion (CHECK) count in the final summary -- a test case may contain
 * many CHECKs (e.g. one per field of a round-trip) or drive a bounded
 * fixture loop internally and report one aggregated CHECK (see
 * test_gesture_idle_zero_actions), but it is always counted once here. */
#define RUN(fn) do { g_test_cases++; fn(); } while (0)

/* ========================================================================
 * st_crc32
 * ======================================================================== */
static void test_crc32(void)
{
	const uint8_t data[] = "123456789";
	uint32_t crc = st_crc32_compute(data, 9);

	/* Standard CRC-32/ISO-HDLC check value for the ASCII string
	 * "123456789" is the textbook self-test vector for this exact
	 * polynomial. */
	CHECK(crc == 0xCBF43926u, "CRC-32 check-vector \"123456789\" -> 0xCBF43926");

	uint8_t zeros[8192] = { 0 };
	uint32_t crc_a = st_crc32_compute(zeros, sizeof(zeros));

	zeros[100] = 0xFFu;
	uint32_t crc_b = st_crc32_compute(zeros, sizeof(zeros));

	CHECK(crc_a != crc_b, "a single flipped byte changes the CRC");
}

/* ========================================================================
 * st_storage_layout: explicit serializer + overflow-proof sizing
 * ======================================================================== */
static void test_storage_layout(void)
{
	CHECK(ST_SECTOR_BYTES == 8192u, "sector size is the documented stock 8192 bytes");
	CHECK(ST_STEM_COUNT == 4u, "exactly four stems");
	CHECK(ST_CHANNELS_PER_STEM == 2u, "stereo, never downgraded to mono");
	CHECK(ST_STORAGE_LAYOUT_VERSION == 2u, "layout is v2, the overflow-corrected version");
	CHECK(ST_SECTOR_FRAME_CAPACITY == 340u,
	      "sector frame capacity is the real documented SP-1 value, not a raw byte division");

	uint32_t sectors = st_storage_song_sectors(ST_SAMPLE_RATE_HZ * 10u); /* 10 s */

	CHECK(sectors == (uint32_t)(((uint64_t)10 * ST_SAMPLE_RATE_HZ + ST_SECTOR_FRAME_CAPACITY - 1u) /
				     ST_SECTOR_FRAME_CAPACITY),
	      "song sector count matches the real per-sector frame capacity, not ceil(bytes/8192)");

	/* Capacity-detected slot count: NOT a hardcoded UI number. */
	uint32_t cap_small = st_storage_compute_slot_capacity(ST_SONG_DATA_SECTOR0 + 1u, 180u);
	uint32_t cap_big = st_storage_compute_slot_capacity(
		ST_SONG_DATA_SECTOR0 + (uint64_t)1000000000ull, 180u);

	CHECK(cap_small == 0u, "a device with essentially no usable capacity reports zero slots");
	CHECK(cap_big == ST_MAX_SLOTS, "a huge device clamps to ST_MAX_SLOTS, not an unbounded count");

	/* THE bug this version fixes: the v1 struct (sizeof ~11,288 bytes) was
	 * crammed into a single 8192-byte sector reservation. Prove the worst
	 * case (every slot populated) fits its reserved sectors, at runtime,
	 * not just via the header's compile-time _Static_assert. */
	uint32_t worst_case = st_library_header_serialized_size(ST_MAX_SLOTS);

	CHECK(worst_case > 0u, "serialized size computes for the full slot count");
	CHECK(worst_case <= (uint32_t)ST_LIBRARY_HEADER_SECTORS_PER_COPY * ST_SECTOR_BYTES,
	      "worst-case serialized header fits its reserved sectors per copy (the v1 overflow, fixed)");

	uint32_t block;

	CHECK(st_storage_sector_to_block(0u, &block) && block == ST_STORAGE_BASE_BLOCK,
	      "logical sector 0 maps to the proven safe base block");
	CHECK(st_storage_sector_to_block(1u, &block) && block == ST_STORAGE_BASE_BLOCK + 16u,
	      "logical sector 1 maps 16 physical 512-byte blocks past sector 0");

	/* Serialize -> deserialize round trip, exercising every field type
	 * (u32/u16/u8/char[]) and a real slot record, not a zeroed struct. */
	static uint8_t buf[ST_LIBRARY_HEADER_SECTORS_PER_COPY * ST_SECTOR_BYTES];
	st_library_header_t h;
	st_library_header_t h2;

	memset(&h, 0, sizeof(h));
	h.generation = 7u;
	h.slot_count = 2u;
	h.current_slot = 1u;
	h.slot[0].song_id_hash = 0xAABBCCDDu;
	h.slot[0].frame_count = 123456u;
	h.slot[0].stem_content_frames[0] = 100000u;
	h.slot[0].stem_content_frames[1] = 123456u;
	h.slot[0].stem_crc32[2] = 0xDEADBEEFu;
	h.slot[0].bpm_q8 = (uint16_t)(128u << 8); /* 128.0 BPM in Q8.8 */
	h.slot[0].downbeat_frame = 4800u;
	h.slot[0].active_stem = ST_STEM_BASS;
	strcpy(h.slot[0].title, "Test Song");
	strcpy(h.slot[0].artist, "Test Artist");
	h.slot[1].frame_count = 0u; /* empty slot */

	uint32_t written = st_library_header_serialize(&h, buf, sizeof(buf));

	CHECK(written == st_library_header_serialized_size(2u),
	      "serialize returns exactly the computed size for 2 slots (compact, not ST_MAX_SLOTS)");
	CHECK(written <= ST_LIBRARY_HEADER_SECTORS_PER_COPY * ST_SECTOR_BYTES,
	      "a real 2-slot header comfortably fits the reserved sectors");

	CHECK(st_library_header_deserialize(buf, written, &h2),
	      "deserialize accepts what serialize just produced");
	CHECK(h2.generation == 7u && h2.slot_count == 2u && h2.current_slot == 1u,
	      "deserialized fixed fields match");
	CHECK(h2.slot[0].song_id_hash == 0xAABBCCDDu && h2.slot[0].frame_count == 123456u,
	      "deserialized slot 0 scalar fields match");
	CHECK(h2.slot[0].stem_content_frames[1] == 123456u && h2.slot[0].stem_crc32[2] == 0xDEADBEEFu,
	      "deserialized per-stem arrays match (independent length + checksum per stem)");
	CHECK(h2.slot[0].bpm_q8 == (uint16_t)(128u << 8) && h2.slot[0].downbeat_frame == 4800u,
	      "deserialized BPM/downbeat timing metadata matches");
	CHECK(strcmp(h2.slot[0].title, "Test Song") == 0 && strcmp(h2.slot[0].artist, "Test Artist") == 0,
	      "deserialized title/artist strings match");

	/* Corruption is rejected, not silently accepted. */
	uint8_t corrupt[64];

	memcpy(corrupt, buf, sizeof(corrupt));
	corrupt[0] ^= 0xFFu; /* wreck the magic */
	CHECK(!st_library_header_deserialize(corrupt, written, &h2),
	      "a corrupted magic is rejected");

	memcpy(corrupt, buf, written < sizeof(corrupt) ? written : sizeof(corrupt));
	if (written >= 40u) {
		corrupt[39] ^= 0xFFu; /* flip a byte inside the first slot record */
	}
	CHECK(!st_library_header_deserialize(corrupt, written, &h2),
	      "a corrupted slot record fails the header CRC and is rejected");

	CHECK(st_library_header_serialize(&h, buf, ST_LIBRARY_HEADER_FIXED_BYTES) == 0u,
	      "serialize fails closed (returns 0) rather than truncating into an undersized buffer");
}

/* ========================================================================
 * st_sector_codec: the real, documented SP-1 sector encode/decode
 * ======================================================================== */
static void test_sector_codec_hand_built_fixture(void)
{
	static st_audio_frame_t frames[ST_SECTOR_FRAME_CAPACITY];
	static uint8_t sector[ST_SECTOR_BYTES];
	st_sector_reserved_t reserved;

	memset(frames, 0, sizeof(frames));
	memset(&reserved, 0, sizeof(reserved));

	/* Frame 0 (logical sub-block 0, local frame 0), stem 0:
	 * L = 0x112233, R = 0x445566.
	 * Documented byte order: b0=Lmid, b1=LMSB, b2=RMSB, b3=Llsb, b4=Rlsb, b5=Rmid. */
	frames[0].stem_l[0] = 0x112233;
	frames[0].stem_r[0] = 0x445566;

	/* Frame 85 (logical sub-block 1, local frame 0), stem 0: a second,
	 * distinct marker used to prove the {0,2,1,3} PHYSICAL sub-block
	 * order, not merely the byte packing within one sub-block. Both
	 * values are kept < 0x800000 (positive in 24-bit two's complement) so
	 * the fixture exercises the byte order, not the signed-clamp path. */
	frames[85].stem_l[0] = 0x778899;
	frames[85].stem_r[0] = 0x2ABBCC;

	reserved.sync[0] = 0x0102u;
	reserved.tempo[0] = 0x0304u;
	reserved.led[0][0] = 0x11u;
	reserved.led[0][1] = 0x22u;
	reserved.led[0][2] = 0x33u;
	reserved.led[0][3] = 0x44u;

	st_sector_encode(frames, &reserved, sector);

	/* Logical sub-block 0 -> physical slot 0 -> sector byte offset 0. */
	uint8_t expect_frame0[6] = { 0x22, 0x11, 0x44, 0x33, 0x66, 0x55 };

	CHECK(memcmp(sector, expect_frame0, 6) == 0,
	      "hand-computed byte order for frame 0 stem 0 matches the documented spec exactly");

	/* Logical sub-block 0's reserved 8 bytes sit at local offset 2040 (85 *
	 * 24), which is also sector offset 2040 since sub-block 0 is physical
	 * slot 0. */
	uint8_t expect_reserved0[8] = { 0x02, 0x01, 0x04, 0x03, 0x11, 0x22, 0x33, 0x44 };

	CHECK(memcmp(sector + 2040, expect_reserved0, 8) == 0,
	      "reserved sync/tempo/LED bytes land at the documented sub-block-local offset 2040");

	/* Logical sub-block 1 -> PHYSICAL slot 2 (ST_SUBBLOCK_PHYSICAL_ORDER =
	 * {0,2,1,3}) -> sector byte offset 2 * 2048 = 4096, NOT the sequential
	 * 2048 a naive implementation would use. This is the check that would
	 * fail if the non-sequential physical order were ignored. */
	uint8_t expect_frame85[6] = { 0x22, 0x11, 0x44, 0x33, 0x66, 0x55 };

	/* frame 85, stem 0: L=0x778899 -> mid=0x88,MSB=0x77,lsb=0x99;
	 * R=0x2ABBCC -> MSB=0x2A,mid=0xBB,lsb=0xCC. */
	expect_frame85[0] = 0x88; /* L mid */
	expect_frame85[1] = 0x77; /* L MSB */
	expect_frame85[2] = 0x2A; /* R MSB */
	expect_frame85[3] = 0x99; /* L lsb */
	expect_frame85[4] = 0xCC; /* R lsb */
	expect_frame85[5] = 0xBB; /* R mid */

	CHECK(memcmp(sector + 4096, expect_frame85, 6) == 0,
	      "logical sub-block 1 lands at PHYSICAL offset 4096 (order {0,2,1,3}), not sequential 2048");
}

static void test_sector_codec_round_trip(void)
{
	static st_audio_frame_t frames_in[ST_SECTOR_FRAME_CAPACITY];
	static st_audio_frame_t frames_out[ST_SECTOR_FRAME_CAPACITY];
	static uint8_t sector[ST_SECTOR_BYTES];
	st_sector_reserved_t reserved_in, reserved_out;
	uint32_t f, s;
	bool all_match = true;

	memset(&reserved_in, 0, sizeof(reserved_in));
	for (f = 0; f < ST_SECTOR_FRAME_CAPACITY; f++) {
		for (s = 0; s < ST_STEM_COUNT; s++) {
			/* A deterministic, full-range pattern touching sign bits. */
			frames_in[f].stem_l[s] = (int32_t)(((f * 37u + s * 101u) % 0x1000000u)) - 0x800000;
			frames_in[f].stem_r[s] = (int32_t)(((f * 53u + s * 211u) % 0x1000000u)) - 0x800000;
		}
	}
	reserved_in.sync[0] = 0xBEEFu;
	reserved_in.tempo[2] = 0x1234u;
	reserved_in.led[3][1] = 0x99u;

	st_sector_encode(frames_in, &reserved_in, sector);
	st_sector_decode(sector, frames_out, &reserved_out);

	for (f = 0; f < ST_SECTOR_FRAME_CAPACITY && all_match; f++) {
		for (s = 0; s < ST_STEM_COUNT; s++) {
			if (frames_in[f].stem_l[s] != frames_out[f].stem_l[s] ||
			    frames_in[f].stem_r[s] != frames_out[f].stem_r[s]) {
				all_match = false;
				break;
			}
		}
	}
	CHECK(all_match, "encode -> decode round trip is exact across all 340 frames * 4 stems, full sign range");
	CHECK(reserved_out.sync[0] == 0xBEEFu && reserved_out.tempo[2] == 0x1234u &&
	      reserved_out.led[3][1] == 0x99u,
	      "reserved sync/tempo/LED metadata round-trips per sub-block");
}

/* ========================================================================
 * st_transfer: transactional begin/stage/verify/commit/abort
 * ======================================================================== */
#define MOCK_SECTORS 8u
typedef struct {
	uint8_t data[MOCK_SECTORS][ST_SECTOR_BYTES];
	bool    written[MOCK_SECTORS];
	bool    fail_write;
	bool    fail_read;
} mock_storage_t;

static int mock_write(uint32_t sector, const uint8_t d[ST_SECTOR_BYTES], void *ctx)
{
	mock_storage_t *m = (mock_storage_t *)ctx;
	uint32_t local = sector - ST_STAGING_SECTOR0;

	if (m->fail_write || local >= MOCK_SECTORS) {
		return -1;
	}
	memcpy(m->data[local], d, ST_SECTOR_BYTES);
	m->written[local] = true;
	return 0;
}

static int mock_read(uint32_t sector, uint8_t out[ST_SECTOR_BYTES], void *ctx)
{
	mock_storage_t *m = (mock_storage_t *)ctx;
	uint32_t local = sector - ST_STAGING_SECTOR0;

	if (m->fail_read || local >= MOCK_SECTORS || !m->written[local]) {
		return -1;
	}
	memcpy(out, m->data[local], ST_SECTOR_BYTES);
	return 0;
}

/* Builds two real, decodable sectors (ST_SECTOR_FRAME_CAPACITY frames each)
 * from a deterministic pattern, computes the matching whole-payload CRC and
 * every per-stem CRC (mirroring exactly what st_xfer_verify() itself
 * computes from the decoded samples), and fills in a ready-to-use
 * st_xfer_song_meta_t. This replaces the old raw-garbage-byte fixture (which
 * predates the codec and could never actually decode). */
static void build_two_sector_fixture(uint8_t sec0[ST_SECTOR_BYTES], uint8_t sec1[ST_SECTOR_BYTES],
				      st_xfer_song_meta_t *meta)
{
	static st_audio_frame_t frames0[ST_SECTOR_FRAME_CAPACITY];
	static st_audio_frame_t frames1[ST_SECTOR_FRAME_CAPACITY];
	st_sector_reserved_t reserved;
	uint32_t f, s;
	uint32_t payload_crc = ST_CRC32_INIT;
	uint32_t stem_crc[ST_STEM_COUNT];

	memset(&reserved, 0, sizeof(reserved));
	for (s = 0; s < ST_STEM_COUNT; s++) {
		stem_crc[s] = ST_CRC32_INIT;
	}
	for (f = 0; f < ST_SECTOR_FRAME_CAPACITY; f++) {
		for (s = 0; s < ST_STEM_COUNT; s++) {
			frames0[f].stem_l[s] = (int32_t)((f * 3u + s) % 8000u);
			frames0[f].stem_r[s] = -frames0[f].stem_l[s];
			frames1[f].stem_l[s] = (int32_t)((f * 5u + s) % 8000u);
			frames1[f].stem_r[s] = -frames1[f].stem_l[s];
		}
	}
	st_sector_encode(frames0, &reserved, sec0);
	st_sector_encode(frames1, &reserved, sec1);

	payload_crc = st_crc32_update(payload_crc, sec0, ST_SECTOR_BYTES);
	payload_crc = st_crc32_update(payload_crc, sec1, ST_SECTOR_BYTES);
	payload_crc ^= 0xFFFFFFFFu;

	/* Per-stem CRC over every content frame across BOTH sectors (all
	 * ST_SECTOR_FRAME_CAPACITY * 2 frames are "content" in this fixture). */
	for (f = 0; f < ST_SECTOR_FRAME_CAPACITY; f++) {
		for (s = 0; s < ST_STEM_COUNT; s++) {
			uint8_t b[8];
			int32_t l = frames0[f].stem_l[s], r = frames0[f].stem_r[s];

			b[0] = (uint8_t)(l & 0xFF); b[1] = (uint8_t)((l >> 8) & 0xFF);
			b[2] = (uint8_t)((l >> 16) & 0xFF); b[3] = (uint8_t)((l >> 24) & 0xFF);
			b[4] = (uint8_t)(r & 0xFF); b[5] = (uint8_t)((r >> 8) & 0xFF);
			b[6] = (uint8_t)((r >> 16) & 0xFF); b[7] = (uint8_t)((r >> 24) & 0xFF);
			stem_crc[s] = st_crc32_update(stem_crc[s], b, 8u);
		}
	}
	for (f = 0; f < ST_SECTOR_FRAME_CAPACITY; f++) {
		for (s = 0; s < ST_STEM_COUNT; s++) {
			uint8_t b[8];
			int32_t l = frames1[f].stem_l[s], r = frames1[f].stem_r[s];

			b[0] = (uint8_t)(l & 0xFF); b[1] = (uint8_t)((l >> 8) & 0xFF);
			b[2] = (uint8_t)((l >> 16) & 0xFF); b[3] = (uint8_t)((l >> 24) & 0xFF);
			b[4] = (uint8_t)(r & 0xFF); b[5] = (uint8_t)((r >> 8) & 0xFF);
			b[6] = (uint8_t)((r >> 16) & 0xFF); b[7] = (uint8_t)((r >> 24) & 0xFF);
			stem_crc[s] = st_crc32_update(stem_crc[s], b, 8u);
		}
	}

	memset(meta, 0, sizeof(*meta));
	meta->frame_count = ST_SECTOR_FRAME_CAPACITY * 2u;
	meta->expected_crc32 = payload_crc;
	meta->stem_present_mask = 0x0Fu;
	meta->bpm_q8 = (uint16_t)(120u << 8);
	meta->downbeat_frame = 0u;
	strcpy(meta->title, "Fixture Song");
	strcpy(meta->artist, "Fixture Artist");
	for (s = 0; s < ST_STEM_COUNT; s++) {
		meta->stem_content_frames[s] = meta->frame_count; /* full-length, no trailing silence */
		meta->stem_crc32[s] = stem_crc[s] ^ 0xFFFFFFFFu;
	}
}

static void test_transfer_happy_path(void)
{
	st_xfer_txn_t t;
	mock_storage_t m;
	uint32_t resume;
	uint8_t sec0[ST_SECTOR_BYTES], sec1[ST_SECTOR_BYTES];
	st_xfer_song_meta_t meta;

	memset(&m, 0, sizeof(m));
	st_xfer_txn_reset(&t);
	build_two_sector_fixture(sec0, sec1, &meta);

	CHECK(st_xfer_begin(&t, 0, &meta, 16u, &resume) == ST_XFER_OK,
	      "begin: accepted for a valid slot");
	CHECK(resume == 0u, "fresh begin resumes from sector 0");

	CHECK(st_xfer_stage_sector(&t, 0, sec0, st_crc32_compute(sec0, ST_SECTOR_BYTES),
				    mock_write, &m) == ST_XFER_OK,
	      "stage sector 0: accepted");
	CHECK(st_xfer_stage_sector(&t, 1, sec1, st_crc32_compute(sec1, ST_SECTOR_BYTES),
				    mock_write, &m) == ST_XFER_OK,
	      "stage sector 1: accepted");

	CHECK(st_xfer_verify(&t, mock_read, &m) == ST_XFER_OK,
	      "verify: whole-payload CRC AND all four independently-decoded per-stem CRCs match");
	CHECK(st_xfer_commit_precheck(&t) == ST_XFER_OK, "commit precheck passes after a real verify");
	CHECK(!t.open, "commit clears the transaction");

	st_slot_meta_t slot_meta;

	st_xfer_txn_reset(&t);
	(void)st_xfer_begin(&t, 2, &meta, 16u, &resume);
	(void)st_xfer_stage_sector(&t, 0, sec0, st_crc32_compute(sec0, ST_SECTOR_BYTES), mock_write, &m);
	(void)st_xfer_stage_sector(&t, 1, sec1, st_crc32_compute(sec1, ST_SECTOR_BYTES), mock_write, &m);
	(void)st_xfer_verify(&t, mock_read, &m);
	CHECK(st_xfer_build_slot_meta(&t, 999u, &slot_meta), "slot meta builds after a real verify");
	CHECK(slot_meta.frame_count == meta.frame_count && slot_meta.start_sector == 999u,
	      "committed slot meta carries the right frame count and start sector");
	CHECK(slot_meta.stem_crc32[2] == meta.stem_crc32[2] &&
	      slot_meta.stem_content_frames[2] == meta.stem_content_frames[2],
	      "committed slot meta carries the independent per-stem length + checksum");
	CHECK(strcmp(slot_meta.title, "Fixture Song") == 0, "committed slot meta carries title/artist");
	CHECK(slot_meta.active_stem == ST_STEM_VOCAL && slot_meta.scrub_speed_index == 1u,
	      "a fresh upload gets firmware-default performance state, never stale carry-over");
}

static void test_transfer_wrong_stem_crc_rejected(void)
{
	st_xfer_txn_t t;
	mock_storage_t m;
	uint32_t resume;
	uint8_t sec0[ST_SECTOR_BYTES], sec1[ST_SECTOR_BYTES];
	st_xfer_song_meta_t meta;

	memset(&m, 0, sizeof(m));
	st_xfer_txn_reset(&t);
	build_two_sector_fixture(sec0, sec1, &meta);
	meta.stem_crc32[1] ^= 0xFFFFFFFFu; /* companion declared the WRONG crc for stem 1 */

	(void)st_xfer_begin(&t, 0, &meta, 16u, &resume);
	(void)st_xfer_stage_sector(&t, 0, sec0, st_crc32_compute(sec0, ST_SECTOR_BYTES), mock_write, &m);
	(void)st_xfer_stage_sector(&t, 1, sec1, st_crc32_compute(sec1, ST_SECTOR_BYTES), mock_write, &m);

	CHECK(st_xfer_verify(&t, mock_read, &m) == ST_XFER_ERR_PAYLOAD_CRC,
	      "a wrong declared per-stem checksum fails verify even though the whole-payload CRC matches -- "
	      "each stem's checksum is checked independently, not inferred from the others");
	CHECK(!t.verified, "the transaction is not marked verified");
}

static void test_transfer_corrupt_sector_rejected(void)
{
	st_xfer_txn_t t;
	mock_storage_t m;
	uint32_t resume;
	uint8_t sec0[ST_SECTOR_BYTES], sec1[ST_SECTOR_BYTES];
	st_xfer_song_meta_t meta;

	memset(&m, 0, sizeof(m));
	st_xfer_txn_reset(&t);
	build_two_sector_fixture(sec0, sec1, &meta);
	(void)st_xfer_begin(&t, 0, &meta, 16u, &resume);

	st_xfer_result_t r = st_xfer_stage_sector(&t, 0, sec0, 0x12345678u /* wrong crc */, mock_write, &m);

	CHECK(r == ST_XFER_ERR_SECTOR_CRC, "a sector with a mismatched per-sector CRC is rejected");
	CHECK(t.staged_through == 0u, "the rejected sector does not advance staged_through");
	CHECK(!m.written[0], "a CRC-rejected sector is never even attempted as a write");
}

static void test_transfer_interrupted_upload_never_commits(void)
{
	st_xfer_txn_t t;
	mock_storage_t m;
	uint32_t resume;
	uint8_t sec0[ST_SECTOR_BYTES], sec1[ST_SECTOR_BYTES];
	st_xfer_song_meta_t meta;

	memset(&m, 0, sizeof(m));
	st_xfer_txn_reset(&t);
	build_two_sector_fixture(sec0, sec1, &meta);
	(void)st_xfer_begin(&t, 5, &meta, 16u, &resume);
	(void)st_xfer_stage_sector(&t, 0, sec0, st_crc32_compute(sec0, ST_SECTOR_BYTES), mock_write, &m);
	/* Connection "drops" before sector 1 and before any verify/commit. */

	CHECK(st_xfer_commit_precheck(&t) == ST_XFER_ERR_INTERRUPTED_COMMIT,
	      "commit is refused when verify was never run -- an interrupted upload can never land");

	/* Reconnect: RESUME with the identical tuple continues from sector 1. */
	uint32_t resume2;

	CHECK(st_xfer_begin(&t, 5, &meta, 16u, &resume2) == ST_XFER_OK,
	      "re-sending the identical (slot, frame_count, crc) tuple resumes, not restarts");
	CHECK(resume2 == 1u, "resume offset is exactly the sector after the last one confirmed staged");

	/* A DIFFERENT tuple for the same slot discards the stale progress. */
	uint32_t resume3;
	st_xfer_song_meta_t meta_changed = meta;

	meta_changed.frame_count += 1u;
	(void)st_xfer_begin(&t, 5, &meta_changed, 16u, &resume3);
	CHECK(resume3 == 0u, "a changed tuple for the same slot starts fresh, discarding stale staging");
}

static void test_transfer_abort_and_token(void)
{
	st_xfer_txn_t t;
	uint32_t resume;
	st_xfer_song_meta_t meta;

	memset(&meta, 0, sizeof(meta));
	meta.frame_count = 100u;
	meta.expected_crc32 = 0x1u;
	meta.stem_present_mask = 0x1u;

	st_xfer_txn_reset(&t);
	(void)st_xfer_begin(&t, 1, &meta, 16u, &resume);
	CHECK(t.open, "transaction open after begin");
	st_xfer_abort(&t);
	CHECK(!t.open, "abort closes the transaction");

	CHECK(st_xfer_check_token(ST_DESTRUCTIVE_CONFIRM_TOKEN, ST_DESTRUCTIVE_CONFIRM_LEN),
	      "the correct destructive token is accepted");
	CHECK(!st_xfer_check_token((const uint8_t *)"wrongtok", 8u),
	      "an incorrect token of the right length is rejected");
	CHECK(!st_xfer_check_token(ST_DESTRUCTIVE_CONFIRM_TOKEN, 3u),
	      "a short token is rejected outright, not partially matched");
}

static void test_transfer_bad_slot_and_oversize(void)
{
	st_xfer_txn_t t;
	uint32_t resume;
	st_xfer_song_meta_t meta;

	memset(&meta, 0, sizeof(meta));
	meta.frame_count = 100u;
	meta.expected_crc32 = 0x1u;
	meta.stem_present_mask = 0x1u;

	st_xfer_txn_reset(&t);
	CHECK(st_xfer_begin(&t, 20u, &meta, 16u, &resume) == ST_XFER_ERR_BAD_SLOT,
	      "a slot index >= total_slots is rejected");
	/* A song comfortably longer than ST_MAX_SONG_SECONDS (which the
	 * staging region is exactly sized for) unambiguously overflows it. */
	meta.frame_count = ST_SAMPLE_RATE_HZ * (ST_MAX_SONG_SECONDS + 60u);
	CHECK(st_xfer_begin(&t, 0u, &meta, 16u, &resume) == ST_XFER_ERR_TOO_LARGE,
	      "a song too large for the staging region is rejected up front");

	/* A stem declared longer than the shared frame_count is invalid
	 * timing metadata, rejected up front rather than silently clamped. */
	memset(&meta, 0, sizeof(meta));
	meta.frame_count = 500u;
	meta.stem_content_frames[0] = 501u;
	CHECK(st_xfer_begin(&t, 0u, &meta, 16u, &resume) == ST_XFER_ERR_BAD_TIMING,
	      "a per-stem content length exceeding the shared frame_count is rejected");

	memset(&meta, 0, sizeof(meta));
	meta.frame_count = 500u;
	meta.downbeat_frame = 500u; /* must be < frame_count */
	CHECK(st_xfer_begin(&t, 0u, &meta, 16u, &resume) == ST_XFER_ERR_BAD_TIMING,
	      "a downbeat_frame at or past the end of the song is rejected");
}

/* ========================================================================
 * st_gesture
 * ======================================================================== */
static void settle(st_gesture_state_t *s)
{
	st_gesture_reset(s, 0);
	s->settled = true; /* skip the startup-settle window for gesture-shape tests */
}

static void test_gesture_idle_zero_actions(void)
{
	st_gesture_state_t s;
	st_cmd_batch_t out;
	bool all_zero = true;
	uint32_t first_bad_t = 0u;

	st_gesture_reset(&s, 1000u);
	/* Nothing touched for 60 real seconds: only tick calls, no edges.
	 * This is ONE distinct test case ("an untouched controller generates
	 * exactly zero actions indefinitely") exercised over 12,000 ticks as
	 * its fixture, not 12,000 separate test cases -- reported as a single
	 * aggregated CHECK so the assertion count reflects distinct claims,
	 * not loop iterations (per the "report test cases separately from
	 * assertion executions" requirement). */
	uint32_t t;

	for (t = 1000u; t <= 61000u; t += 5u) {
		st_gesture_process_tick(&s, t, &out);
		if (out.count != 0u) {
			all_zero = false;
			first_bad_t = t;
			break;
		}
	}
	CHECK(all_zero, "an untouched controller emits zero actions across 12,000 idle ticks (60s @ 5ms)%s",
	      all_zero ? "" : " -- FIRST VIOLATION logged below");
	if (!all_zero) {
		printf("       first nonzero idle tick at t=%u\n", first_bad_t);
	}
}

static void test_gesture_boot_baseline_not_input(void)
{
	st_gesture_state_t s;
	st_cmd_batch_t out;

	st_gesture_reset(&s, 0u);
	/* A control already held at boot (before the settle window closes)
	 * must not be treated as user input. */
	st_gesture_process_edge(&s, ST_CTRL_PLAY, true, 10u, &out);
	CHECK(out.count == 0u, "a control already down during the startup-settle window emits nothing");
}

static void test_gesture_play_pause_tap(void)
{
	st_gesture_state_t s;
	st_cmd_batch_t out;

	settle(&s);
	s.playing = false;
	st_gesture_process_edge(&s, ST_CTRL_PLAY, true, 1000u, &out);
	CHECK(out.count == 0u, "PLAY press alone emits nothing yet (tap/hold undecided)");
	st_gesture_process_edge(&s, ST_CTRL_PLAY, false, 1100u, &out); /* 100 ms: a tap */
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_PLAY_PAUSE_TOGGLE,
	      "a quick PLAY tap toggles play/pause");
	CHECK(s.playing, "playing flips true");
}

static void test_gesture_global_loop_momentary_and_latch(void)
{
	st_gesture_state_t s;
	st_cmd_batch_t out;

	settle(&s);
	s.playing = true;
	st_gesture_process_edge(&s, ST_CTRL_PLAY, true, 1000u, &out);
	st_gesture_process_tick(&s, 1000u + ST_GESTURE_PLAY_TAP_HOLD_MS - 1u, &out);
	CHECK(out.count == 0u, "one ms before the hold threshold: no momentary loop yet");
	st_gesture_process_tick(&s, 1000u + ST_GESTURE_PLAY_TAP_HOLD_MS, &out);
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_LOOP_MOMENTARY_START,
	      "hold threshold crossed while playing: momentary loop starts");
	CHECK(s.loop_momentary_active, "loop_momentary_active set");

	/* Release without latching: momentary loop ends. */
	st_gesture_process_edge(&s, ST_CTRL_PLAY, false, 2000u, &out);
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_LOOP_MOMENTARY_END,
	      "releasing PLAY without latching ends the momentary loop");
	CHECK(!s.loop_momentary_active, "loop_momentary_active cleared");

	/* Now latch: hold PLAY, tap FUNCTION while held, release PLAY. */
	st_gesture_process_edge(&s, ST_CTRL_PLAY, true, 3000u, &out);
	st_gesture_process_tick(&s, 3000u + ST_GESTURE_PLAY_TAP_HOLD_MS, &out);
	st_gesture_process_edge(&s, ST_CTRL_FUNCTION, true, 3500u, &out);
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_LOOP_LATCH,
	      "FUNCTION tapped while PLAY held latches the loop");
	CHECK(s.loop_latched, "loop_latched set");
	st_gesture_process_edge(&s, ST_CTRL_FUNCTION, false, 3520u, &out);
	CHECK(out.count == 0u, "FUNCTION release after the latch emits nothing more");
	st_gesture_process_edge(&s, ST_CTRL_PLAY, false, 4000u, &out);
	CHECK(out.count == 0u, "PLAY MAY be released without ending or relocating the latched loop");
	CHECK(s.loop_latched, "loop stays latched after PLAY release");

	/* Exit: tap PLAY. */
	st_gesture_process_edge(&s, ST_CTRL_PLAY, true, 5000u, &out);
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_LOOP_EXIT, "tapping PLAY exits a latched loop");
	CHECK(!s.loop_latched, "loop_latched cleared on exit");
	st_gesture_process_edge(&s, ST_CTRL_PLAY, false, 5050u, &out);
	CHECK(out.count == 0u, "the exit press's release does not ALSO toggle play/pause");
}

static void test_gesture_fx_scope_toggle_and_ownership(void)
{
	st_gesture_state_t s;
	st_cmd_batch_t out;

	settle(&s);

	/* Bare simultaneous volumes (no FUNCTION) open STEM scope. */
	st_gesture_process_edge(&s, ST_CTRL_VOL_MINUS, true, 1000u, &out);
	CHECK(out.count == 0u, "first volume of the chord emits nothing yet (pending)");
	st_gesture_process_edge(&s, ST_CTRL_VOL_PLUS, true, 1050u, &out); /* within 120 ms window */
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_FX_SCOPE_OPEN_STEM,
	      "bare simultaneous Volume-/Volume+ within the chord window opens STEM FX scope");
	CHECK(s.fx_scope == ST_FX_SCOPE_STEM, "scope is STEM");

	/* The chord emits no master-volume/loop-division/stem-select side effects. */
	st_gesture_process_edge(&s, ST_CTRL_VOL_MINUS, false, 1100u, &out);
	CHECK(out.count == 0u, "releasing the chord's Volume- emits nothing extra");
	st_gesture_process_edge(&s, ST_CTRL_VOL_PLUS, false, 1120u, &out);
	CHECK(out.count == 0u, "releasing the chord's Volume+ emits nothing extra");

	/* In STEM scope, FUNCTION + Volume+ cycles stems. */
	st_gesture_process_edge(&s, ST_CTRL_FUNCTION, true, 2000u, &out);
	st_gesture_process_edge(&s, ST_CTRL_VOL_PLUS, true, 2050u, &out);
	st_gesture_process_edge(&s, ST_CTRL_VOL_PLUS, false, 2150u, &out); /* > chord window: single action */
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_FX_STEM_CYCLE_NEXT,
	      "FUNCTION + Volume+ in STEM scope cycles the target stem, not master volume");
	st_gesture_process_edge(&s, ST_CTRL_FUNCTION, false, 2200u, &out);

	/* Bare chord again: closes the FX interface. */
	st_gesture_process_edge(&s, ST_CTRL_VOL_MINUS, true, 3000u, &out);
	st_gesture_process_edge(&s, ST_CTRL_VOL_PLUS, true, 3050u, &out);
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_FX_SCOPE_CLOSE,
	      "the bare chord again closes the (now open) FX interface");
	CHECK(s.fx_scope == ST_FX_SCOPE_NONE, "scope closed");
	st_gesture_process_edge(&s, ST_CTRL_VOL_MINUS, false, 3100u, &out);
	st_gesture_process_edge(&s, ST_CTRL_VOL_PLUS, false, 3120u, &out);

	/* FUNCTION held FIRST, then both volumes: GLOBAL scope. */
	st_gesture_process_edge(&s, ST_CTRL_FUNCTION, true, 4000u, &out);
	st_gesture_process_edge(&s, ST_CTRL_VOL_MINUS, true, 4050u, &out);
	st_gesture_process_edge(&s, ST_CTRL_VOL_PLUS, true, 4090u, &out);
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_FX_SCOPE_OPEN_GLOBAL,
	      "FUNCTION held first, then both volumes, opens GLOBAL scope");
	CHECK(s.fx_scope == ST_FX_SCOPE_GLOBAL, "scope is GLOBAL");
}

static void test_gesture_fx_track_momentary_latch_unlatch(void)
{
	st_gesture_state_t s;
	st_cmd_batch_t out;

	settle(&s);
	s.fx_scope = ST_FX_SCOPE_STEM;

	/* Hold: momentary. */
	st_gesture_process_edge(&s, ST_CTRL_TRACK1, true, 1000u, &out);
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_FX_TRACK_MOMENTARY_START,
	      "holding a Track button while FX is open starts momentary FX");
	st_gesture_process_edge(&s, ST_CTRL_TRACK1, false, 1200u, &out);
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_FX_TRACK_MOMENTARY_END,
	      "releasing without a FUNCTION tap ends the momentary FX");

	/* Hold + tap FUNCTION while held: latch. */
	st_gesture_process_edge(&s, ST_CTRL_TRACK1, true, 2000u, &out);
	st_gesture_process_edge(&s, ST_CTRL_FUNCTION, true, 2100u, &out);
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_FX_TRACK_LATCH,
	      "FUNCTION tapped while the Track is held latches that bank's FX");
	CHECK(s.fx_track_latched[st_fx_bank_of_button(0)], "bank latched");
	st_gesture_process_edge(&s, ST_CTRL_FUNCTION, false, 2120u, &out);
	st_gesture_process_edge(&s, ST_CTRL_TRACK1, false, 2200u, &out);
	CHECK(out.count == 0u, "releasing the track after a latch does not also fire MOMENTARY_END");

	/* Repeat the same sequence: unlatch. */
	st_gesture_process_edge(&s, ST_CTRL_TRACK1, true, 3000u, &out);
	st_gesture_process_edge(&s, ST_CTRL_FUNCTION, true, 3100u, &out);
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_FX_TRACK_UNLATCH,
	      "repeating hold+FUNCTION-tap unlatches the same bank");
	CHECK(!s.fx_track_latched[st_fx_bank_of_button(0)], "bank unlatched");
}

static void test_gesture_scrub_grammar(void)
{
	st_gesture_state_t s;
	st_cmd_batch_t out;

	settle(&s);

	/* 1: hold FUNCTION, deflect rocker -> momentary scrub. */
	st_gesture_process_edge(&s, ST_CTRL_FUNCTION, true, 1000u, &out);
	st_gesture_process_edge(&s, ST_CTRL_ROCKER_FWD, true, 1050u, &out);
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_SCRUB_MOMENTARY_START,
	      "FUNCTION held + rocker deflected begins momentary scrub");
	CHECK(s.scrub_active && s.scrub_direction == 1, "scrub active, forward");

	/* 3: FUNCTION may release while rocker remains held. */
	st_gesture_process_edge(&s, ST_CTRL_FUNCTION, false, 1200u, &out);
	CHECK(out.count == 0u, "releasing FUNCTION while the rocker is still held does nothing");
	CHECK(s.scrub_active, "scrub still active after FUNCTION release");

	/* 7: touching the rocker alone (already active) must not unlatch it --
	 * exercised here as "rocker press while already active is inert". */

	/* 4/5: tap FUNCTION again while rocker held -> arm; release rocker -> latched. */
	st_gesture_process_edge(&s, ST_CTRL_FUNCTION, true, 1300u, &out);
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_SCRUB_LATCH_ARM,
	      "tapping FUNCTION again while the rocker is held arms the latch");
	st_gesture_process_edge(&s, ST_CTRL_FUNCTION, false, 1350u, &out);
	st_gesture_process_edge(&s, ST_CTRL_ROCKER_FWD, false, 1400u, &out);
	CHECK(out.count == 0u, "releasing the rocker after arming emits no extra command");
	CHECK(s.scrub_latched && s.scrub_active, "scrub is now latched");

	/* 6: a completed bare FUNCTION tap with nothing else joined unlatches it. */
	st_gesture_process_edge(&s, ST_CTRL_FUNCTION, true, 2000u, &out);
	st_gesture_process_edge(&s, ST_CTRL_FUNCTION, false, 2100u, &out); /* 100 ms: a tap */
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_SCRUB_UNLATCH,
	      "a bare FUNCTION tap unlatches an already-latched scrub");
	CHECK(!s.scrub_latched && !s.scrub_active, "scrub inactive after unlatch");
}

static void test_gesture_scrub_rocker_alone_never_unlatches(void)
{
	st_gesture_state_t s;
	st_cmd_batch_t out;

	settle(&s);
	s.scrub_active = true;
	s.scrub_latched = true;
	s.scrub_direction = 1;

	st_gesture_process_edge(&s, ST_CTRL_ROCKER_FWD, true, 1000u, &out);
	CHECK(out.count == 0u, "touching the rocker alone while latched emits nothing");
	st_gesture_process_edge(&s, ST_CTRL_ROCKER_FWD, false, 1100u, &out);
	CHECK(out.count == 0u && s.scrub_latched, "and does not unlatch it");
}

static void test_gesture_scrub_owns_volume_never_master(void)
{
	st_gesture_state_t s;
	st_cmd_batch_t out;

	settle(&s);
	s.scrub_active = true;
	s.scrub_speed_index = 1u;

	st_gesture_process_edge(&s, ST_CTRL_VOL_PLUS, true, 1000u, &out);
	st_gesture_process_edge(&s, ST_CTRL_VOL_PLUS, false, 1121u, &out); /* > chord window */
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_SCRUB_SPEED_SELECT,
	      "Volume+ during active scrub selects a speed, never master volume");
	CHECK(s.scrub_speed_index == 2u, "speed index stepped up");
}

static void test_gesture_master_volume_default(void)
{
	st_gesture_state_t s;
	st_cmd_batch_t out;

	settle(&s);
	st_gesture_process_edge(&s, ST_CTRL_VOL_MINUS, true, 1000u, &out);
	st_gesture_process_edge(&s, ST_CTRL_VOL_MINUS, false, 1121u, &out);
	CHECK(out.count == 1u && out.cmds[0].id == ST_CMD_MASTER_VOLUME_STEP,
	      "a lone Volume- with no higher-priority context owns master volume");
}

static void test_gesture_fader_jitter_and_pickup(void)
{
	st_gesture_state_t s;
	st_cmd_t out;

	settle(&s);
	st_gesture_arm_fader_pickup(&s, 0);

	/* First sample: initial snapshot, never user input. */
	st_gesture_process_fader(&s, 0, 2000u, 3000u, 1000u, &out);
	CHECK(out.id == ST_CMD_NONE, "the very first fader sample is a snapshot, not a command");

	/* Small jitter around the same value: suppressed. */
	st_gesture_process_fader(&s, 0, 2003u, 3000u, 1010u, &out);
	CHECK(out.id == ST_CMD_NONE, "+-1 LSB jitter never becomes a mixer command");

	/* Pickup: physical fader must CROSS the persisted level (3000) before
	 * it takes effect. */
	st_gesture_process_fader(&s, 0, 2500u, 3000u, 1020u, &out);
	CHECK(out.id == ST_CMD_NONE, "still below the persisted level: pickup has not crossed yet");
	st_gesture_process_fader(&s, 0, 3100u, 3000u, 1030u, &out);
	CHECK(out.id == ST_CMD_FADER_LEVEL, "crossing the persisted level picks the fader up");
	CHECK(!s.fader_picked_up[0] == false, "picked_up flag is now set");

	/* After pickup, ordinary deadband applies. */
	st_gesture_process_fader(&s, 0, 3200u, 3000u, 1040u, &out);
	CHECK(out.id == ST_CMD_FADER_LEVEL, "a real move past the deadband after pickup emits a command");
}

/* ========================================================================
 * st_scrub
 * ======================================================================== */
static void test_scrub_speeds(void)
{
	CHECK(ST_SCRUB_SPEEDS[0] == 1.25f && ST_SCRUB_SPEEDS[1] == 1.6f &&
	      ST_SCRUB_SPEEDS[2] == 2.5f && ST_SCRUB_SPEEDS[3] == 4.0f,
	      "the four persistent scrub speeds are 1.25x, 1.6x, 2.5x, 4x (documented, not re-chosen)");
	CHECK(ST_SCRUB_DEFAULT_SPEED_INDEX == 1u, "default speed index is 1 (1.6x)");
}

static void test_scrub_forward_release_monotone_to_1x(void)
{
	st_scrub_release_t seg = st_scrub_make_release(2.5f); /* forward shuttle at 2.5x */
	float prev = st_scrub_rate_at(&seg, 0.0f);
	float t;
	int monotone = 1;

	CHECK(prev == 2.5f, "rate at t=0 is exactly the shuttle rate");
	for (t = 0.0f; t <= seg.duration_s; t += seg.duration_s / 50.0f) {
		float r = st_scrub_rate_at(&seg, t);

		if (r > prev + 1e-4f) {
			monotone = 0; /* forward release must never speed back up */
		}
		if (r < 1.0f - 1e-4f) {
			monotone = 0; /* and must never overshoot below 1.0x */
		}
		prev = r;
	}
	CHECK(monotone, "a forward scrub release decelerates monotonically, never below +1.0x");
	CHECK(fabsf(st_scrub_rate_at(&seg, seg.duration_s) - 1.0f) < 1e-5f,
	      "the ramp lands EXACTLY on +1.0x at its end");
}

static void test_scrub_reverse_release_crosses_zero_continuously(void)
{
	st_scrub_release_t seg = st_scrub_make_release(-2.5f); /* reverse shuttle */
	float zc = st_scrub_zero_crossing(&seg);

	CHECK(zc > 0.0f && zc < seg.duration_s, "a reverse release has exactly one zero crossing inside the ramp");
	CHECK(fabsf(st_scrub_rate_at(&seg, zc)) < 1e-3f, "the rate at the solved crossing time is ~0");
	CHECK(st_scrub_rate_at(&seg, 0.0f) < 0.0f, "starts negative (reverse)");
	CHECK(st_scrub_rate_at(&seg, seg.duration_s) > 0.0f, "ends positive (+1.0x)");

	/* Continuity: a reverse release's signed distance integral is NOT
	 * monotonic (it legitimately dips while the rate is still negative,
	 * then rises once the rate crosses positive) -- what "no jump, no
	 * duplicate audio, no discontinuity" actually requires is that
	 * CONSECUTIVE samples never move by more than the ramp's own maximum
	 * possible rate times the sampling step, i.e. no sudden tear. */
	float step = seg.duration_s / 200.0f;
	float max_rate = fmaxf(fabsf(seg.from), fabsf(seg.to));
	float prev_d = st_scrub_distance(&seg, 0.0f);
	float t;
	int continuous = 1;

	for (t = step; t <= seg.duration_s; t += step) {
		float d = st_scrub_distance(&seg, t);
		float step_delta = fabsf(d - prev_d);

		if (step_delta > max_rate * step * 1.5f) { /* generous bound: catches a real tear */
			continuous = 0;
		}
		prev_d = d;
	}
	CHECK(continuous, "position advances smoothly (no discontinuous jump) through the reverse hand-off");
}

static void test_scrub_release_scales_with_span(void)
{
	st_scrub_release_t near = st_scrub_make_release(1.1f);   /* small span from 1.0x */
	st_scrub_release_t far = st_scrub_make_release(4.0f);    /* large span from 1.0x */

	CHECK(near.duration_s < far.duration_s,
	      "a release closer to the musical rate takes less time than a release from a far one");
	CHECK(near.duration_s >= ST_SCRUB_RELEASE_MIN_S && far.duration_s <= ST_SCRUB_RELEASE_MAX_S,
	      "duration stays within the documented [0.02s, 1.2s] bounds");
}

/* ========================================================================
 * st_fx_catalog
 * ======================================================================== */
static void test_fx_catalog(void)
{
	CHECK(strcmp(ST_FX_BANKS[ST_FX_BANK_TONE].algorithms[1].id, "exciter") == 0,
	      "TONE bank algorithm 1 is \"exciter\", not \"isolator\"");
	CHECK(strcmp(ST_FX_BANKS[ST_FX_BANK_TONE].algorithms[0].id, "filter") == 0 &&
	      strcmp(ST_FX_BANKS[ST_FX_BANK_TONE].algorithms[2].id, "dirt") == 0,
	      "TONE bank is exactly Filter, Exciter, Dirt/Crusher, in that order");
	CHECK(ST_FX_BANK_COUNT == 4u, "four banks, twelve algorithms total");

	CHECK(st_fx_bank_of_button(0) == ST_FX_BANK_TONE, "Track1 -> TONE");
	CHECK(st_fx_bank_of_button(1) == ST_FX_BANK_MOTION, "Track2 -> MOTION");
	CHECK(st_fx_bank_of_button(2) == ST_FX_BANK_SPACE, "Track3 -> SPACE");
	CHECK(st_fx_bank_of_button(3) == ST_FX_BANK_MOD, "Track4 -> MOD (RHYTHM)");
}

/* ========================================================================
 * st_led_pattern
 * ======================================================================== */
static void test_led_base_priority(void)
{
	CHECK(st_led_select_base(true, true, true, true) == ST_LED_BASE_STORAGE_ERROR,
	      "storage error outranks every other base state");
	CHECK(st_led_select_base(false, true, true, true) == ST_LED_BASE_LOW_BATTERY,
	      "low battery outranks transfer/loading");
	CHECK(st_led_select_base(false, false, true, true) == ST_LED_BASE_TRANSFER,
	      "transfer outranks loading");
	CHECK(st_led_select_base(false, false, false, true) == ST_LED_BASE_LOADING,
	      "loading is the fallback ahead of idle");
	CHECK(st_led_select_base(false, false, false, false) == ST_LED_BASE_IDLE,
	      "idle is the default with nothing else active");
}

static void test_led_boot_flash_is_one_shot(void)
{
	st_led_oneshot_t o;
	st_led_frame_t f;

	st_led_oneshot_start(&o, ST_LED_ONESHOT_BOOT_FLASH, 1000u);
	CHECK(st_led_oneshot_active(&o, 1000u), "boot flash active right at start");
	CHECK(st_led_oneshot_active(&o, 1399u), "boot flash still active 1 ms before its duration ends");
	CHECK(!st_led_oneshot_active(&o, 1400u), "boot flash expires from firmware time, not a placeholder");

	st_led_render_oneshot(&o, 1000u, &f);
	CHECK(f.level[0] == ST_LED_LEVEL_MAX && f.level[3] == ST_LED_LEVEL_MAX,
	      "boot flash lights all four Track LEDs together");
	CHECK(f.level[4] == 0u, "boot flash does not touch the side row");
}

static void test_led_playing_side_and_battery_never_fabricated(void)
{
	st_led_inputs_t in;
	st_led_frame_t f;

	memset(&in, 0, sizeof(in));
	in.playing = true;
	in.battery_level = 0xFFu; /* unavailable */
	st_led_render_base(ST_LED_BASE_IDLE, &in, 5000u, &f);
	CHECK(f.level[ST_LED_SIDE_PLAY] == ST_LED_LEVEL_MAX, "playing: side LED nearest PLAY is solid");

	in.playing = false;
	st_led_render_base(ST_LED_BASE_IDLE, &in, 5000u, &f);
	CHECK(f.level[ST_LED_SIDE_PLAY] == 0u, "paused: side LED nearest PLAY is off");
	CHECK(f.level[5] == 0u && f.level[6] == 0u && f.level[7] == 0u,
	      "an unavailable battery reading is never fabricated as a specific gauge level");
}

static void test_led_active_stem_and_status_distinguishable(void)
{
	st_led_inputs_t in;
	st_led_frame_t f;

	memset(&in, 0, sizeof(in));
	in.stem_status[0] = ST_LED_STEM_EMPTY;
	in.stem_status[1] = ST_LED_STEM_MUTED;
	in.stem_status[2] = ST_LED_STEM_SOLOED;
	in.stem_status[3] = ST_LED_STEM_LOADED;
	in.stem_activity[3] = 100u;
	in.active_stem = 3u;

	st_led_render_base(ST_LED_BASE_IDLE, &in, 0u, &f);
	CHECK(f.level[0] == 0u, "empty stem stays fully off");
	CHECK(f.level[1] > 0u && f.level[1] < f.level[2], "muted (ghost glow) is dimmer than soloed");
	CHECK(f.level[2] == ST_LED_LEVEL_MAX, "soloed stem is full brightness");
	CHECK(f.level[3] > 100u, "the active stem is brightened above its own base activity level");
}

/* Mirrors st_led_pattern.c's private FX_LATCH_FLASH_MS without exposing it
 * as public API -- 150 ms per that file's UNMEASURED design constant. */
#define FX_LATCH_FLASH_MS_FOR_TEST 150u

static void test_led_fx_latch_flash_restores_prior_state(void)
{
	st_led_oneshot_t o;
	st_led_inputs_t in;
	st_led_frame_t f;

	memset(&in, 0, sizeof(in));
	in.playing = true;
	in.stem_status[0] = ST_LED_STEM_SOLOED;

	st_led_oneshot_start(&o, ST_LED_ONESHOT_FX_LATCH_FLASH, 1000u);
	st_led_render(&o, ST_LED_BASE_IDLE, &in, 1000u, &f);
	CHECK(f.level[0] == ST_LED_LEVEL_MAX && f.level[1] == ST_LED_LEVEL_MAX,
	      "the FX-latch-flash one-shot overrides the whole Track row");

	/* Once expired, the VERY NEXT render recomputes idle from current
	 * inputs -- restore is automatic, never a stale snapshot. */
	st_led_render(&o, ST_LED_BASE_IDLE, &in, 1000u + FX_LATCH_FLASH_MS_FOR_TEST, &f);
	CHECK(f.level[0] == ST_LED_LEVEL_MAX, "the soloed stem's true state is restored (still full)");
	CHECK(f.level[ST_LED_SIDE_PLAY] == ST_LED_LEVEL_MAX,
	      "the underlying playing indication is restored correctly after the flash expires");
}

static void test_led_scrub_chase_direction(void)
{
	st_led_inputs_t in;
	st_led_frame_t f;

	memset(&in, 0, sizeof(in));
	in.scrub_active = true;
	in.scrub_direction = 1;
	in.scrub_speed_index = 0u;

	st_led_render_base(ST_LED_BASE_IDLE, &in, 0u, &f);
	int lit_count = 0;
	uint8_t i;

	for (i = 0; i < ST_LED_TRACK_ROW_COUNT; i++) {
		if (f.level[i] == ST_LED_LEVEL_MAX) {
			lit_count++;
		}
	}
	CHECK(lit_count == 1, "the scrub chase lights exactly one Track LED at a time");
}

static void test_led_transfer_pattern(void)
{
	st_led_inputs_t in;
	st_led_frame_t f;

	memset(&in, 0, sizeof(in));
	st_led_render_base(ST_LED_BASE_TRANSFER, &in, 0u, &f);
	CHECK(f.level[0] == ST_LED_LEVEL_MAX && f.level[1] == ST_LED_LEVEL_MAX &&
	      f.level[2] == ST_LED_LEVEL_MAX && f.level[3] == ST_LED_LEVEL_MAX,
	      "transfer mode: all four Track LEDs blink together (on-phase)");
}

/* ========================================================================
 * st_stem_validate — the atomic-commit gate wired into the real
 * xfer_service() 'Z' verb in firmware/stemtape_player/src/main.c.
 * ======================================================================== */
static st_stem_commit_t make_valid_commit(void)
{
	st_stem_commit_t c;
	uint32_t i;

	c.present_mask = 0x0Fu;
	c.track_block_capacity = 100000u;
	for (i = 0; i < ST_STEM_COUNT; i++) {
		c.frame_count[i] = 12345u;
		c.declared_crc32[i] = 0xAAAA0000u + i;
		c.actual_crc32[i] = c.declared_crc32[i];
	}
	return c;
}

static void test_stem_validate_accepts_valid_commit(void)
{
	st_stem_commit_t c = make_valid_commit();

	CHECK(st_stem_validate_commit(&c) == ST_STEM_OK,
	      "a fully valid 4-stem commit (all present, equal length, matching CRCs) is accepted");
}

static void test_stem_validate_rejects_missing_stem(void)
{
	st_stem_commit_t c = make_valid_commit();

	c.present_mask = 0x07u; /* only 3 of 4 stems declared present */
	CHECK(st_stem_validate_commit(&c) == ST_STEM_ERR_MISSING_STEM,
	      "fewer than 4 declared-present stems is rejected, not partially activated");
}

static void test_stem_validate_rejects_zero_length(void)
{
	st_stem_commit_t c = make_valid_commit();

	c.frame_count[2] = 0u;
	CHECK(st_stem_validate_commit(&c) == ST_STEM_ERR_ZERO_LENGTH,
	      "a zero-length declared stem is rejected");
}

static void test_stem_validate_rejects_length_mismatch(void)
{
	st_stem_commit_t c = make_valid_commit();

	c.frame_count[3] = c.frame_count[0] + 1u; /* one stem one block longer than the other three */
	CHECK(st_stem_validate_commit(&c) == ST_STEM_ERR_LENGTH_MISMATCH,
	      "mismatched per-stem lengths are rejected -- the shared-transport invariant "
	      "every stem song's synchronization depends on");
}

static void test_stem_validate_rejects_oversize(void)
{
	st_stem_commit_t c = make_valid_commit();

	c.frame_count[0] = c.frame_count[1] = c.frame_count[2] = c.frame_count[3] =
		c.track_block_capacity + 1u;
	CHECK(st_stem_validate_commit(&c) == ST_STEM_ERR_TOO_LARGE,
	      "a shared length exceeding the device's real per-track block capacity is rejected");
}

static void test_stem_validate_rejects_crc_mismatch(void)
{
	st_stem_commit_t c = make_valid_commit();

	c.actual_crc32[1] ^= 0xFFFFFFFFu; /* the real read-back CRC does not match what was declared */
	CHECK(st_stem_validate_commit(&c) == ST_STEM_ERR_CRC_MISMATCH,
	      "a stem whose real read-back CRC-32 does not match the declared value is rejected "
	      "(corrupt/truncated/reordered upload)");
}

static void test_stem_validate_check_order_is_deterministic(void)
{
	/* Multiple simultaneous violations -> the fixed, documented check
	 * order (presence, then zero-length, then mismatch, then size, then
	 * CRC) determines which single reason is reported, not an unordered
	 * "rejected" -- exercise one such case explicitly. */
	st_stem_commit_t c = make_valid_commit();

	c.present_mask = 0x0Fu;
	c.frame_count[0] = 0u;             /* zero-length ... */
	c.frame_count[1] = 999999999u;     /* ... AND a length mismatch ... */
	c.actual_crc32[2] ^= 1u;           /* ... AND a CRC mismatch, all at once */
	CHECK(st_stem_validate_commit(&c) == ST_STEM_ERR_ZERO_LENGTH,
	      "with multiple simultaneous violations, zero-length is reported first (documented order)");
}

/* ========================================================================
 * st_stem_validate against the deterministic fixture
 * (tests/fixtures/generate_fixture.py, tests/fixtures/manifest.json) --
 * real fixture bytes on disk, run through the REAL st_crc32_compute() and
 * st_stem_validate_commit() production functions, not synthetic in-test
 * values. Run from the repo root (matches the CI host-test step's
 * working-directory and this repo's own `cc ... && ./audit/test_...`
 * convention).
 * ======================================================================== */
#define FIXTURE_DIR "firmware/stemtape_player/tests/fixtures/"
#define FIXTURE_EMMC_BLOCK_SIZE 512u
#define FIXTURE_FRAME_COUNT_BLOCKS 8u
#define FIXTURE_STEM_BYTES (FIXTURE_FRAME_COUNT_BLOCKS * FIXTURE_EMMC_BLOCK_SIZE)

/* Manifest-recorded CRC32 values (regenerate via generate_fixture.py and
 * update both manifest.json and these constants together if the fixture
 * ever changes -- the self-check below fails loudly if they drift apart). */
static const uint32_t FIXTURE_STEM_CRC32[ST_STEM_COUNT] = {
	0x09CF6B00u, 0xB029AC6Du, 0x2CD21102u, 0x90EC77F9u,
};
static const uint32_t FIXTURE_STEM0_CORRUPT_CRC32 = 0x5317C22Cu;

static bool fixture_read(const char *name, uint8_t *buf, size_t len)
{
	char path[256];
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "%s%s", FIXTURE_DIR, name);
	f = fopen(path, "rb");
	if (!f) return false;
	n = fread(buf, 1, len, f);
	fclose(f);
	return n == len;
}

static void test_stem_validate_fixture_manifest_crc_matches_generator(void)
{
	/* Recomputing the CRC the same way the generator did (zlib.crc32 is
	 * the identical reflected-IEEE-802.3 algorithm st_crc32_compute()
	 * implements) proves the checked-in fixture bytes are exactly what
	 * manifest.json / this file's constants claim -- catches silent
	 * fixture-file corruption or a stale hardcoded constant. */
	static uint8_t buf[FIXTURE_STEM_BYTES];
	uint32_t i;
	bool all_present = true;
	bool all_match = true;
	char name[16];

	for (i = 0; i < ST_STEM_COUNT; i++) {
		snprintf(name, sizeof(name), "stem%u.bin", i);
		if (!fixture_read(name, buf, sizeof(buf))) { all_present = false; continue; }
		if (st_crc32_compute(buf, sizeof(buf)) != FIXTURE_STEM_CRC32[i]) all_match = false;
	}
	CHECK(all_present, "all 4 fixture stem files are present and the declared size");
	CHECK(all_match, "st_crc32_compute() over each fixture stem reproduces the "
	      "manifest-recorded CRC32 exactly");
}

static void test_stem_validate_fixture_valid_set_accepted(void)
{
	static uint8_t buf[ST_STEM_COUNT][FIXTURE_STEM_BYTES];
	st_stem_commit_t c;
	uint32_t i;
	char name[16];

	c.present_mask = 0x0Fu;
	c.track_block_capacity = 1000000u;
	for (i = 0; i < ST_STEM_COUNT; i++) {
		snprintf(name, sizeof(name), "stem%u.bin", i);
		(void)fixture_read(name, buf[i], sizeof(buf[i]));
		c.frame_count[i] = FIXTURE_FRAME_COUNT_BLOCKS;
		/* the uploader declares the CRC of the bytes it sent; here that's
		 * exactly the on-disk fixture, matching a real untampered upload. */
		c.declared_crc32[i] = st_crc32_compute(buf[i], sizeof(buf[i]));
		/* "read back from the card" -- the real xfer_service() streams this
		 * in ZBURST-block bursts through st_crc32_update(); a single-shot
		 * st_crc32_compute() over the same bytes is CRC32-equivalent
		 * (streaming update is associative over the concatenated input). */
		c.actual_crc32[i] = st_crc32_compute(buf[i], sizeof(buf[i]));
	}
	CHECK(st_stem_validate_commit(&c) == ST_STEM_OK,
	      "the deterministic fixture's real 4-stem set, read from disk and CRC'd "
	      "with the real st_crc32_compute(), is accepted by the real st_stem_validate_commit()");
}

static void test_stem_validate_fixture_corrupt_set_rejected(void)
{
	static uint8_t good[FIXTURE_STEM_BYTES];
	static uint8_t corrupt[FIXTURE_STEM_BYTES];
	st_stem_commit_t c;
	uint32_t i;
	char name[16];

	c.present_mask = 0x0Fu;
	c.track_block_capacity = 1000000u;
	(void)fixture_read("stem0_corrupt.bin", corrupt, sizeof(corrupt));
	for (i = 0; i < ST_STEM_COUNT; i++) {
		snprintf(name, sizeof(name), "stem%u.bin", i);
		(void)fixture_read(name, good, sizeof(good));
		c.frame_count[i] = FIXTURE_FRAME_COUNT_BLOCKS;
		/* the uploader still DECLARES the original, uncorrupted CRC ... */
		c.declared_crc32[i] = FIXTURE_STEM_CRC32[i];
	}
	/* ... but what the card actually reads back for stem 0 is the
	 * corrupted variant (one flipped bit) -- simulating real-world
	 * corruption occurring between the host declaring the CRC and the
	 * bytes actually landing on eMMC. */
	c.actual_crc32[0] = st_crc32_compute(corrupt, sizeof(corrupt));
	CHECK(c.actual_crc32[0] == FIXTURE_STEM0_CORRUPT_CRC32,
	      "the corrupt fixture variant's real CRC32 matches the manifest-recorded value");
	for (i = 1u; i < ST_STEM_COUNT; i++)
		c.actual_crc32[i] = FIXTURE_STEM_CRC32[i];
	CHECK(st_stem_validate_commit(&c) == ST_STEM_ERR_CRC_MISMATCH,
	      "a real single-bit-corrupted fixture stem, read from disk and CRC'd with the "
	      "real st_crc32_compute(), is rejected by the real st_stem_validate_commit()");
}

/* ========================================================================
 * st_midi_queue — GATE 1: incoming USB-MIDI (Universal MIDI Packet) decode
 * + bounded event queue, exercised the SAME way main.c's real rx_packet_cb
 * does (st_midi_decode_ump32() on ump.data[0], then st_midi_queue_push()),
 * per the directive's "inject real USB MIDI packets into the same parser
 * and event queue used by firmware" requirement.
 * ======================================================================== */

/* Builds one raw 32-bit UMP word, bit-for-bit the way a real class-
 * compliant MIDI 1.0 Channel Voice message arrives packed in UMP (Zephyr
 * v4.3.0 <zephyr/audio/midi.h> UMP_MT/UMP_MIDI_COMMAND/UMP_MIDI_CHANNEL/
 * UMP_MIDI1_P1/UMP_MIDI1_P2 macros) -- see st_midi_queue.h's header
 * comment. `group` is a UMP Group (0-15); st_midi_decode_ump32() does not
 * use it (this codebase has exactly one incoming MIDI cable/group), so
 * tests exercise a couple of different group values to prove that's true
 * rather than assumed. */
static uint32_t mk_ump32(uint8_t group, uint8_t command, uint8_t channel,
			  uint8_t p1, uint8_t p2)
{
	uint32_t mt = 0x02u; /* UMP_MT_MIDI1_CHANNEL_VOICE */
	uint32_t status = (uint32_t)((command & 0x0Fu) << 4) | (channel & 0x0Fu);

	return (mt << 28) | ((uint32_t)(group & 0x0Fu) << 24) |
	       (status << 16) | ((uint32_t)(p1 & 0x7Fu) << 8) | (uint32_t)(p2 & 0x7Fu);
}

static void test_midi_decode_note_on(void)
{
	st_midi_event_t ev;
	uint32_t word = mk_ump32(0, 0x9u, 3, 60, 100);

	CHECK(st_midi_decode_ump32(word, &ev) == true,
	      "a real UMP Note On word decodes");
	CHECK(ev.kind == ST_MIDI_EVT_NOTE_ON, "decoded kind is NOTE_ON");
	CHECK(ev.channel == 3, "decoded channel matches the packed value (3)");
	CHECK(ev.note == 60, "decoded note matches the packed value (60)");
	CHECK(ev.velocity == 100, "decoded velocity matches the packed value (100)");
}

static void test_midi_decode_note_on_velocity_zero_is_note_off(void)
{
	st_midi_event_t ev;
	/* Per docs/FIRMWARE_CONTRACT_V1.md section 6: a Note On with velocity
	 * 0 normalizes to Note Off -- a real, common MIDI 1.0 running-status
	 * convention many controllers (and bridges) use in place of an
	 * explicit Note Off. */
	uint32_t word = mk_ump32(0, 0x9u, 9, 42, 0);

	CHECK(st_midi_decode_ump32(word, &ev) == true,
	      "a Note On with velocity 0 still decodes");
	CHECK(ev.kind == ST_MIDI_EVT_NOTE_OFF,
	      "velocity-0 Note On normalizes to NOTE_OFF");
	CHECK(ev.channel == 9, "channel is preserved across the normalization");
	CHECK(ev.note == 42, "note number is preserved across the normalization");
}

static void test_midi_decode_explicit_note_off(void)
{
	st_midi_event_t ev;
	uint32_t word = mk_ump32(0, 0x8u, 0, 127, 64);

	CHECK(st_midi_decode_ump32(word, &ev) == true,
	      "an explicit UMP Note Off word decodes");
	CHECK(ev.kind == ST_MIDI_EVT_NOTE_OFF, "decoded kind is NOTE_OFF");
	CHECK(ev.channel == 0, "decoded channel matches the packed value (0)");
	CHECK(ev.note == 127, "decoded note matches the packed value (127)");
}

static void test_midi_decode_all_notes_off_cc123(void)
{
	st_midi_event_t ev;
	uint32_t word = mk_ump32(0, 0xBu, 5, 123, 0);

	CHECK(st_midi_decode_ump32(word, &ev) == true,
	      "Control Change 123 (All Notes Off) decodes");
	CHECK(ev.kind == ST_MIDI_EVT_ALL_NOTES_OFF, "decoded kind is ALL_NOTES_OFF");
	CHECK(ev.channel == 5, "decoded channel matches the packed value (5)");

	/* All Notes Off must be recognized regardless of its (unused, by
	 * convention 0) CC value byte. */
	word = mk_ump32(0, 0xBu, 5, 123, 77);
	CHECK(st_midi_decode_ump32(word, &ev) == true,
	      "CC123 decodes as ALL_NOTES_OFF regardless of its data-byte value");
	CHECK(ev.kind == ST_MIDI_EVT_ALL_NOTES_OFF,
	      "decoded kind is still ALL_NOTES_OFF with a non-zero CC value byte");
}

static void test_midi_decode_drops_other_control_changes(void)
{
	st_midi_event_t ev;
	uint32_t word;
	int i, tested = 0;

	memset(&ev, 0xAA, sizeof(ev)); /* poison: must stay untouched on drop */
	for (i = 0; i < 128; i++) {
		if (i == 123) {
			continue; /* CC123 is the one CC that's accepted */
		}
		word = mk_ump32(0, 0xBu, 1, (uint8_t)i, 64);
		if (st_midi_decode_ump32(word, &ev)) {
			CHECK(0, "CC %d must be dropped, not decoded", i);
		}
		tested++;
	}
	CHECK(tested == 127, "all 127 non-123 Control Change numbers were exercised");
}

static void test_midi_decode_drops_non_note_message_types(void)
{
	st_midi_event_t ev;
	uint32_t word;

	/* Polyphonic Key Pressure (0xA), Program Change (0xC), Channel
	 * Pressure (0xD), Pitch Bend (0xE) -- all real MIDI 1.0 Channel
	 * Voice commands, all outside this contract's accepted set (see
	 * docs/FIRMWARE_CONTRACT_V1.md section 6: only noteOn/noteOff/
	 * allNotesOff are accepted; everything else, including these, is
	 * dropped). */
	uint8_t commands[] = { 0xAu, 0xCu, 0xDu, 0xEu };
	size_t i;

	for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		word = mk_ump32(0, commands[i], 2, 60, 60);
		CHECK(st_midi_decode_ump32(word, &ev) == false,
		      "MIDI command 0x%X is dropped (not decoded)", commands[i]);
	}
}

static void test_midi_decode_drops_non_midi1_channel_voice_message_type(void)
{
	st_midi_event_t ev;
	uint32_t word;

	/* Utility (MT 0x0), System Real Time (MT 0x1), SysEx7 (MT 0x3),
	 * MIDI2 Channel Voice (MT 0x4) -- even a byte pattern that would
	 * otherwise look like a Note On in the lower 24 bits must be dropped
	 * if the Message Type nibble isn't MIDI1 Channel Voice (0x2). This
	 * codebase never enables UMP Stream / MIDI2 CV negotiation (see
	 * main.c's USB-MIDI wiring comment), so only MT 0x2 is meaningful
	 * here. */
	uint8_t mts[] = { 0x0u, 0x1u, 0x3u, 0x4u, 0xFu };
	size_t i;

	for (i = 0; i < sizeof(mts) / sizeof(mts[0]); i++) {
		word = (mts[i] << 28) | (0x9u << 20) | (3u << 16) | (60u << 8) | 100u;
		CHECK(st_midi_decode_ump32(word, &ev) == false,
		      "Message Type 0x%X is dropped even with Note-On-shaped lower bits",
		      mts[i]);
	}
}

static void test_midi_decode_preserves_arbitrary_channel_and_note(void)
{
	/* Per the directive: "do not hardcode particular KO II pad notes" --
	 * cue identity is whatever (channel, note) actually arrives. Sweeps
	 * every real channel and a representative spread of note numbers
	 * (not a fixed KO II layout) to prove nothing narrows the accepted
	 * range. */
	uint8_t ch, note;
	uint8_t notes[] = { 0, 1, 35, 36, 44, 60, 96, 126, 127 };
	size_t ni;

	for (ch = 0; ch < 16; ch++) {
		for (ni = 0; ni < sizeof(notes) / sizeof(notes[0]); ni++) {
			st_midi_event_t ev;
			uint32_t word;

			note = notes[ni];
			word = mk_ump32((uint8_t)(ch & 0x0Fu), 0x9u, ch, note, 100);
			CHECK(st_midi_decode_ump32(word, &ev) == true,
			      "channel %u note %u decodes", ch, note);
			CHECK(ev.channel == ch && ev.note == note,
			      "channel %u note %u round-trip exactly (got ch=%u note=%u)",
			      ch, note, ev.channel, ev.note);
		}
	}
}

static void test_midi_queue_fifo_order(void)
{
	st_midi_queue_t q;
	st_midi_event_t in[5], out;
	int i;

	st_midi_queue_init(&q);
	for (i = 0; i < 5; i++) {
		in[i].kind = ST_MIDI_EVT_NOTE_ON;
		in[i].channel = 0;
		in[i].note = (uint8_t)(20 + i);
		in[i].velocity = 100;
		st_midi_queue_push(&q, &in[i]);
	}
	CHECK(q.count == 5, "queue holds all 5 pushed events");
	for (i = 0; i < 5; i++) {
		CHECK(st_midi_queue_pop(&q, &out) == true, "pop %d succeeds", i);
		CHECK(out.note == in[i].note,
		      "pop %d returns events in FIFO order (expected note %u, got %u)",
		      i, in[i].note, out.note);
	}
	CHECK(st_midi_queue_pop(&q, &out) == false, "popping an empty queue fails");
	CHECK(q.count == 0, "queue count is 0 after draining");
}

static void test_midi_queue_overflow_drops_oldest(void)
{
	st_midi_queue_t q;
	st_midi_event_t ev, out;
	uint32_t i;
	const uint32_t n = ST_MIDI_QUEUE_CAPACITY + 7u;

	st_midi_queue_init(&q);
	for (i = 0; i < n; i++) {
		ev.kind = ST_MIDI_EVT_NOTE_ON;
		ev.channel = 1;
		ev.note = (uint8_t)(i % 128u);
		ev.velocity = 100;
		st_midi_queue_push(&q, &ev);
	}
	CHECK(q.count == ST_MIDI_QUEUE_CAPACITY,
	      "queue never grows past its fixed capacity (%u)", ST_MIDI_QUEUE_CAPACITY);
	CHECK(q.dropped == (n - ST_MIDI_QUEUE_CAPACITY),
	      "dropped counter reflects exactly the events that didn't fit (%u)",
	      n - ST_MIDI_QUEUE_CAPACITY);
	/* the documented overflow policy is "drop the OLDEST": the first
	 * event still in the queue must be the (n - CAPACITY)'th one pushed,
	 * not the very first. */
	CHECK(st_midi_queue_pop(&q, &out) == true, "pop after overflow succeeds");
	CHECK(out.note == (uint8_t)((n - ST_MIDI_QUEUE_CAPACITY) % 128u),
	      "the oldest surviving event is exactly the (n-capacity)th pushed "
	      "(got note %u)", out.note);
}

static void test_midi_queue_clear_preserves_dropped_counter(void)
{
	st_midi_queue_t q;
	st_midi_event_t ev;
	uint32_t i;
	uint32_t dropped_before;

	st_midi_queue_init(&q);
	for (i = 0; i < ST_MIDI_QUEUE_CAPACITY + 3u; i++) {
		ev.kind = ST_MIDI_EVT_NOTE_ON;
		ev.channel = 0;
		ev.note = 1;
		ev.velocity = 1;
		st_midi_queue_push(&q, &ev);
	}
	dropped_before = q.dropped;
	CHECK(dropped_before == 3u, "3 events were dropped before clear()");
	st_midi_queue_clear(&q);
	CHECK(q.count == 0, "clear() empties the queue");
	CHECK(q.dropped == dropped_before,
	      "clear() does not reset the lifetime `dropped` diagnostic counter");
}

static void test_midi_pipeline_ump_word_to_queue_round_trip(void)
{
	/* The exact two-step pipeline main.c's real rx_packet_cb runs:
	 * st_midi_decode_ump32() on the incoming UMP word, then
	 * st_midi_queue_push() of the decoded event -- proving the SAME
	 * parser and event queue used by firmware, driven end-to-end by a
	 * synthetic but byte-for-byte real USB-MIDI packet. */
	st_midi_queue_t q;
	st_midi_event_t decoded, popped;
	uint32_t word;

	st_midi_queue_init(&q);

	/* KO II pad press: Note On channel 2, note 44, velocity 127. (44 is
	 * a plausible, not hardcoded/assumed, pad note -- this test proves
	 * the pipeline preserves whatever arrives, not that 44 is special.) */
	word = mk_ump32(0, 0x9u, 2, 44, 127);
	CHECK(st_midi_decode_ump32(word, &decoded) == true,
	      "pipeline step 1: incoming UMP word decodes");
	st_midi_queue_push(&q, &decoded);
	CHECK(st_midi_queue_pop(&q, &popped) == true,
	      "pipeline step 2: decoded event pops back out of the queue");
	CHECK(popped.kind == ST_MIDI_EVT_NOTE_ON && popped.channel == 2 &&
	      popped.note == 44 && popped.velocity == 127,
	      "the event that reaches the consumer is exactly what the wire sent "
	      "(kind=%d channel=%u note=%u velocity=%u)",
	      popped.kind, popped.channel, popped.note, popped.velocity);

	/* KO II pad release: Note Off channel 2, note 44 -- same pipeline. */
	word = mk_ump32(0, 0x8u, 2, 44, 0);
	CHECK(st_midi_decode_ump32(word, &decoded) == true,
	      "pipeline step 1 (release): incoming UMP word decodes");
	st_midi_queue_push(&q, &decoded);
	CHECK(st_midi_queue_pop(&q, &popped) == true,
	      "pipeline step 2 (release): decoded event pops back out");
	CHECK(popped.kind == ST_MIDI_EVT_NOTE_OFF && popped.channel == 2 && popped.note == 44,
	      "the release event also reaches the consumer intact "
	      "(kind=%d channel=%u note=%u)", popped.kind, popped.channel, popped.note);

	/* USB reset / disconnect / lost connection -> firmware pushes a
	 * synthetic ALL_NOTES_OFF (see main.c's midi_ready_cb()) -- proves
	 * the same queue carries that safety event through identically. */
	decoded.kind = ST_MIDI_EVT_ALL_NOTES_OFF;
	decoded.channel = 0;
	decoded.note = 0;
	decoded.velocity = 0;
	st_midi_queue_push(&q, &decoded);
	CHECK(st_midi_queue_pop(&q, &popped) == true,
	      "a synthetic disconnect ALL_NOTES_OFF pops out of the same queue");
	CHECK(popped.kind == ST_MIDI_EVT_ALL_NOTES_OFF,
	      "the disconnect event's kind survives the round trip");
}

int main(void)
{
	RUN(test_crc32);
	RUN(test_storage_layout);
	RUN(test_sector_codec_hand_built_fixture);
	RUN(test_sector_codec_round_trip);
	RUN(test_transfer_happy_path);
	RUN(test_transfer_wrong_stem_crc_rejected);
	RUN(test_transfer_corrupt_sector_rejected);
	RUN(test_transfer_interrupted_upload_never_commits);
	RUN(test_transfer_abort_and_token);
	RUN(test_transfer_bad_slot_and_oversize);

	RUN(test_stem_validate_accepts_valid_commit);
	RUN(test_stem_validate_rejects_missing_stem);
	RUN(test_stem_validate_rejects_zero_length);
	RUN(test_stem_validate_rejects_length_mismatch);
	RUN(test_stem_validate_rejects_oversize);
	RUN(test_stem_validate_rejects_crc_mismatch);
	RUN(test_stem_validate_check_order_is_deterministic);
	RUN(test_stem_validate_fixture_manifest_crc_matches_generator);
	RUN(test_stem_validate_fixture_valid_set_accepted);
	RUN(test_stem_validate_fixture_corrupt_set_rejected);

	RUN(test_gesture_idle_zero_actions);
	RUN(test_gesture_boot_baseline_not_input);
	RUN(test_gesture_play_pause_tap);
	RUN(test_gesture_global_loop_momentary_and_latch);
	RUN(test_gesture_fx_scope_toggle_and_ownership);
	RUN(test_gesture_fx_track_momentary_latch_unlatch);
	RUN(test_gesture_scrub_grammar);
	RUN(test_gesture_scrub_rocker_alone_never_unlatches);
	RUN(test_gesture_scrub_owns_volume_never_master);
	RUN(test_gesture_master_volume_default);
	RUN(test_gesture_fader_jitter_and_pickup);
	/* NOTE: solo/link/song-select/audition dispatch logic (st_gesture.c
	 * track-press/release rewrite) and st_mixer.c were designed (see
	 * st_gesture.h's updated header comment and st_cmd_id_t additions:
	 * ST_CMD_TRACK_SOLO_TOGGLE, ST_CMD_TRACK_LINK_TOGGLE,
	 * ST_CMD_TRACK_AUDITION_START/END, ST_CMD_SONG_BANK_JUMP,
	 * ST_CMD_SONG_NEXT_IN_BANK) but NOT implemented before this codebase's
	 * runtime approach was superseded (see README/report) -- no test call
	 * here for functions that were never written. */

	RUN(test_scrub_speeds);
	RUN(test_scrub_forward_release_monotone_to_1x);
	RUN(test_scrub_reverse_release_crosses_zero_continuously);
	RUN(test_scrub_release_scales_with_span);

	RUN(test_fx_catalog);

	RUN(test_led_base_priority);
	RUN(test_led_boot_flash_is_one_shot);
	RUN(test_led_playing_side_and_battery_never_fabricated);
	RUN(test_led_active_stem_and_status_distinguishable);
	RUN(test_led_fx_latch_flash_restores_prior_state);
	RUN(test_led_scrub_chase_direction);
	RUN(test_led_transfer_pattern);

	RUN(test_midi_decode_note_on);
	RUN(test_midi_decode_note_on_velocity_zero_is_note_off);
	RUN(test_midi_decode_explicit_note_off);
	RUN(test_midi_decode_all_notes_off_cc123);
	RUN(test_midi_decode_drops_other_control_changes);
	RUN(test_midi_decode_drops_non_note_message_types);
	RUN(test_midi_decode_drops_non_midi1_channel_voice_message_type);
	RUN(test_midi_decode_preserves_arbitrary_channel_and_note);
	RUN(test_midi_queue_fifo_order);
	RUN(test_midi_queue_overflow_drops_oldest);
	RUN(test_midi_queue_clear_preserves_dropped_counter);
	RUN(test_midi_pipeline_ump_word_to_queue_round_trip);

	printf("\n");
	printf("%d distinct test cases, %d assertion checks\n", g_test_cases, g_checks);
	if (g_failures) {
		printf("STEMTAPE PLAYER SELF-TEST FAILED (%d of %d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("STEMTAPE PLAYER SELF-TEST PASSED (%d test cases, %d checks)\n", g_test_cases, g_checks);
	return 0;
}
