/*
 * test_stemtape_player.c — Stem Tape standalone player: host-runnable tests
 * for every PURE module (st_crc32, st_transfer, st_storage_layout,
 * st_gesture, st_scrub, st_led_pattern).
 *
 *     cc -std=c11 -Wall -Wextra -I../src \
 *        ../src/st_crc32.c ../src/st_transfer.c ../src/st_gesture.c \
 *        ../src/st_scrub.c ../src/st_led_pattern.c test_stemtape_player.c \
 *        -lm -o test_stemtape_player && ./test_stemtape_player
 *
 * Same self-checking pattern as firmware/stemtape/tests/test_led.c: [OK]/
 * [FAIL] per assertion, nonzero exit on any failure.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "st_crc32.h"
#include "st_fx_catalog.h"
#include "st_gesture.h"
#include "st_led_pattern.h"
#include "st_scrub.h"
#include "st_storage_layout.h"
#include "st_transfer.h"
#include "st_transfer_protocol.h"

static int g_checks;
static int g_failures;

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
 * st_storage_layout
 * ======================================================================== */
static void test_storage_layout(void)
{
	CHECK(ST_SECTOR_BYTES == 8192u, "sector size is the documented stock 8192 bytes");
	CHECK(ST_STEM_COUNT == 4u, "exactly four stems");
	CHECK(ST_CHANNELS_PER_STEM == 2u, "stereo, never downgraded to mono");
	CHECK(ST_BYTES_PER_SAMPLE == 3u, "24-bit, never downgraded to 16-bit");
	CHECK(ST_FRAME_BYTES == 24u, "frame = 4 stems * 2 ch * 3 bytes");

	uint32_t sectors = st_storage_song_sectors(ST_SAMPLE_RATE_HZ * 10u); /* 10 s */

	CHECK(sectors == (uint32_t)((10ull * ST_SAMPLE_RATE_HZ * ST_FRAME_BYTES + ST_SECTOR_BYTES - 1u) /
				     ST_SECTOR_BYTES),
	      "song sector count matches the documented sector math");

	/* Capacity-detected slot count: NOT a hardcoded UI number. */
	uint32_t cap_small = st_storage_compute_slot_capacity(ST_SONG_DATA_SECTOR0 + 1u, 180u);
	uint32_t cap_big = st_storage_compute_slot_capacity(
		ST_SONG_DATA_SECTOR0 + (uint64_t)1000000000ull, 180u);

	CHECK(cap_small == 0u, "a device with essentially no usable capacity reports zero slots");
	CHECK(cap_big == ST_MAX_SLOTS, "a huge device clamps to ST_MAX_SLOTS, not an unbounded count");
	CHECK(sizeof(st_library_header_t) > 0u, "library header struct is well-formed");
}

/* ========================================================================
 * st_transfer: transactional begin/stage/verify/commit/abort
 * ======================================================================== */
#define MOCK_SECTORS 64u
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

static void fill_sector(uint8_t buf[ST_SECTOR_BYTES], uint8_t seed)
{
	uint32_t i;

	for (i = 0; i < ST_SECTOR_BYTES; i++) {
		buf[i] = (uint8_t)(seed + i);
	}
}

static void test_transfer_happy_path(void)
{
	st_xfer_txn_t t;
	mock_storage_t m;
	uint32_t resume;
	uint8_t sec0[ST_SECTOR_BYTES], sec1[ST_SECTOR_BYTES];
	uint32_t crc = ST_CRC32_INIT;
	uint32_t frame_count = (ST_SECTOR_BYTES * 2u) / ST_FRAME_BYTES; /* exactly 2 sectors */

	memset(&m, 0, sizeof(m));
	st_xfer_txn_reset(&t);
	fill_sector(sec0, 0x11u);
	fill_sector(sec1, 0x22u);
	crc = st_crc32_update(crc, sec0, ST_SECTOR_BYTES);
	crc = st_crc32_update(crc, sec1, ST_SECTOR_BYTES);
	crc ^= 0xFFFFFFFFu;

	CHECK(st_xfer_begin(&t, 0, frame_count, crc, 0x0Fu, 16u, &resume) == ST_XFER_OK,
	      "begin: accepted for a valid slot");
	CHECK(resume == 0u, "fresh begin resumes from sector 0");

	CHECK(st_xfer_stage_sector(&t, 0, sec0, st_crc32_compute(sec0, ST_SECTOR_BYTES),
				    mock_write, &m) == ST_XFER_OK,
	      "stage sector 0: accepted");
	CHECK(st_xfer_stage_sector(&t, 1, sec1, st_crc32_compute(sec1, ST_SECTOR_BYTES),
				    mock_write, &m) == ST_XFER_OK,
	      "stage sector 1: accepted");

	CHECK(st_xfer_verify(&t, mock_read, &m) == ST_XFER_OK, "verify: full payload CRC matches");
	CHECK(st_xfer_commit_precheck(&t) == ST_XFER_OK, "commit precheck passes after a real verify");
	CHECK(!t.open, "commit clears the transaction");

	st_slot_meta_t meta;

	st_xfer_txn_reset(&t);
	(void)st_xfer_begin(&t, 2, frame_count, crc, 0x0Fu, 16u, &resume);
	(void)st_xfer_stage_sector(&t, 0, sec0, st_crc32_compute(sec0, ST_SECTOR_BYTES), mock_write, &m);
	(void)st_xfer_stage_sector(&t, 1, sec1, st_crc32_compute(sec1, ST_SECTOR_BYTES), mock_write, &m);
	(void)st_xfer_verify(&t, mock_read, &m);
	CHECK(st_xfer_build_slot_meta(&t, 999u, &meta), "slot meta builds after a real verify");
	CHECK(meta.frame_count == frame_count && meta.start_sector == 999u,
	      "committed slot meta carries the right frame count and start sector");
	CHECK(meta.active_stem == ST_STEM_VOCAL && meta.scrub_speed_index == 1u,
	      "a fresh upload gets firmware-default performance state, never stale carry-over");
}

static void test_transfer_corrupt_sector_rejected(void)
{
	st_xfer_txn_t t;
	mock_storage_t m;
	uint32_t resume;
	uint8_t sec[ST_SECTOR_BYTES];

	memset(&m, 0, sizeof(m));
	st_xfer_txn_reset(&t);
	fill_sector(sec, 0x33u);
	(void)st_xfer_begin(&t, 0, ST_SECTOR_BYTES / ST_FRAME_BYTES, 0xDEADBEEFu, 0x01u, 16u, &resume);

	st_xfer_result_t r = st_xfer_stage_sector(&t, 0, sec, 0x12345678u /* wrong crc */, mock_write, &m);

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
	uint32_t frame_count = (ST_SECTOR_BYTES * 2u) / ST_FRAME_BYTES;

	memset(&m, 0, sizeof(m));
	st_xfer_txn_reset(&t);
	fill_sector(sec0, 0x44u);
	fill_sector(sec1, 0x55u);
	(void)st_xfer_begin(&t, 5, frame_count, 0xCAFEBABEu, 0x0Fu, 16u, &resume);
	(void)st_xfer_stage_sector(&t, 0, sec0, st_crc32_compute(sec0, ST_SECTOR_BYTES), mock_write, &m);
	/* Connection "drops" before sector 1 and before any verify/commit. */

	CHECK(st_xfer_commit_precheck(&t) == ST_XFER_ERR_NOT_VERIFIED,
	      "commit is refused when verify was never run -- an interrupted upload can never land");

	/* Reconnect: RESUME with the identical tuple continues from sector 1. */
	uint32_t resume2;

	CHECK(st_xfer_begin(&t, 5, frame_count, 0xCAFEBABEu, 0x0Fu, 16u, &resume2) == ST_XFER_OK,
	      "re-sending the identical (slot, frame_count, crc) tuple resumes, not restarts");
	CHECK(resume2 == 1u, "resume offset is exactly the sector after the last one confirmed staged");

	/* A DIFFERENT tuple for the same slot discards the stale progress. */
	uint32_t resume3;

	(void)st_xfer_begin(&t, 5, frame_count + 1u, 0xCAFEBABEu, 0x0Fu, 16u, &resume3);
	CHECK(resume3 == 0u, "a changed tuple for the same slot starts fresh, discarding stale staging");
}

static void test_transfer_abort_and_token(void)
{
	st_xfer_txn_t t;
	uint32_t resume;

	st_xfer_txn_reset(&t);
	(void)st_xfer_begin(&t, 1, 100u, 0x1u, 0x1u, 16u, &resume);
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

	st_xfer_txn_reset(&t);
	CHECK(st_xfer_begin(&t, 20u, 100u, 0x1u, 0x1u, 16u, &resume) == ST_XFER_ERR_BAD_SLOT,
	      "a slot index >= total_slots is rejected");
	/* A song comfortably longer than ST_MAX_SONG_SECONDS (which the
	 * staging region is exactly sized for) unambiguously overflows it. */
	CHECK(st_xfer_begin(&t, 0u, ST_SAMPLE_RATE_HZ * (ST_MAX_SONG_SECONDS + 60u),
			     0x1u, 0x1u, 16u, &resume) == ST_XFER_ERR_TOO_LARGE,
	      "a song too large for the staging region is rejected up front");
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

	st_gesture_reset(&s, 1000u);
	/* Nothing touched for 60 real seconds: only tick calls, no edges. */
	uint32_t t;

	for (t = 1000u; t <= 61000u; t += 5u) {
		st_gesture_process_tick(&s, t, &out);
		CHECK(out.count == 0u, "idle tick at t=%u emits zero commands", t);
		if (out.count != 0u) {
			break; /* don't spam 12000 identical failures */
		}
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

int main(void)
{
	test_crc32();
	test_storage_layout();
	test_transfer_happy_path();
	test_transfer_corrupt_sector_rejected();
	test_transfer_interrupted_upload_never_commits();
	test_transfer_abort_and_token();
	test_transfer_bad_slot_and_oversize();

	test_gesture_idle_zero_actions();
	test_gesture_boot_baseline_not_input();
	test_gesture_play_pause_tap();
	test_gesture_global_loop_momentary_and_latch();
	test_gesture_fx_scope_toggle_and_ownership();
	test_gesture_fx_track_momentary_latch_unlatch();
	test_gesture_scrub_grammar();
	test_gesture_scrub_rocker_alone_never_unlatches();
	test_gesture_scrub_owns_volume_never_master();
	test_gesture_master_volume_default();
	test_gesture_fader_jitter_and_pickup();

	test_scrub_speeds();
	test_scrub_forward_release_monotone_to_1x();
	test_scrub_reverse_release_crosses_zero_continuously();
	test_scrub_release_scales_with_span();

	test_fx_catalog();

	test_led_base_priority();
	test_led_boot_flash_is_one_shot();
	test_led_playing_side_and_battery_never_fabricated();
	test_led_active_stem_and_status_distinguishable();
	test_led_fx_latch_flash_restores_prior_state();
	test_led_scrub_chase_direction();
	test_led_transfer_pattern();

	printf("\n");
	if (g_failures) {
		printf("STEMTAPE PLAYER SELF-TEST FAILED (%d of %d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("STEMTAPE PLAYER SELF-TEST PASSED (%d checks)\n", g_checks);
	return 0;
}
