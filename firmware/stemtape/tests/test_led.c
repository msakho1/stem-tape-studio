/*
 * test_led.c — Stem Tape LED Feedback Protocol v1: host-runnable tests.
 *
 * Builds and runs with the host's own C compiler, no Zephyr/nRF toolchain,
 * no hardware:
 *
 *     cc -std=c11 -Wall -Wextra -I../src \
 *        ../src/led_duty.c ../src/led_frame.c ../src/led_midi.c test_led.c \
 *        -o test_led && ./test_led
 *
 * This links the EXACT same led_duty.c/led_frame.c/led_midi.c translation
 * units the firmware compiles (see ../CMakeLists.txt) — nothing here is a
 * reimplementation of the protocol logic, so a pass here is evidence about
 * the real firmware behavior, not a parallel model of it.
 *
 * Exit code 0 = every check passed. Any failure prints [FAIL] and the
 * process exits non-zero, matching the repo's other self-checking audit
 * scripts (.github/scripts/m0_assertions_selftest.py).
 */

#include <stdio.h>
#include <string.h>

#include "led_duty.h"
#include "led_frame.h"
#include "led_midi.h"
#include "led_protocol.h"

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

/* ------------------------------------------------------------------------
 * 1. exact index-to-pin mapping
 * ------------------------------------------------------------------------ */
static void test_index_to_pin_mapping(void)
{
	static const led_physical_pin_t expected[LED_PHYSICAL_COUNT] = {
		{ 0, 29 }, { 0, 26 }, { 1, 15 }, { 1, 14 }, /* Track 1-4 */
		{ 0,  1 }, { 1, 12 }, { 0,  0 }, { 1, 13 }, /* Playback 1-4 */
	};

	for (uint8_t i = 0; i < LED_PHYSICAL_COUNT; i++) {
		CHECK(led_physical_pin_map[i].port == expected[i].port &&
		      led_physical_pin_map[i].pin == expected[i].pin,
		      "index %u -> P%u.%02u", i,
		      led_physical_pin_map[i].port, led_physical_pin_map[i].pin);
	}
}

/* ------------------------------------------------------------------------
 * 2/3/4. level-to-track-duty, level-to-playback-duty, zero/max clamping
 * ------------------------------------------------------------------------ */
static void test_level_to_duty(void)
{
	/* Track row (idx 0..3): ceiling 52 us. */
	CHECK(led_level_to_pulse_us(LED_IDX_TRACK1, 0) == 0,
	      "track level 0 -> 0 us (truly off)");
	CHECK(led_level_to_pulse_us(LED_IDX_TRACK1, LED_LEVEL_MAX) == LED_TRACK_MAX_PULSE_US,
	      "track level 127 -> %u us (row ceiling)", LED_TRACK_MAX_PULSE_US);
	CHECK(led_level_to_pulse_us(LED_IDX_TRACK4, 64) == (64u * LED_TRACK_MAX_PULSE_US + 63u) / 127u,
	      "track level 64 -> round-to-nearest linear scale");
	for (uint8_t i = 0; i < LED_TRACK_ROW_COUNT; i++)
		CHECK(led_row_max_pulse_us(i) == LED_TRACK_MAX_PULSE_US,
		      "row ceiling lookup for track idx %u", i);

	/* Playback row (idx 4..7): ceiling 66 us. */
	CHECK(led_level_to_pulse_us(LED_IDX_PLAYBACK1, 0) == 0,
	      "playback level 0 -> 0 us (truly off)");
	CHECK(led_level_to_pulse_us(LED_IDX_PLAYBACK4, LED_LEVEL_MAX) == LED_PLAYBACK_MAX_PULSE_US,
	      "playback level 127 -> %u us (row ceiling)", LED_PLAYBACK_MAX_PULSE_US);
	for (uint8_t i = LED_TRACK_ROW_COUNT; i < LED_PHYSICAL_COUNT; i++)
		CHECK(led_row_max_pulse_us(i) == LED_PLAYBACK_MAX_PULSE_US,
		      "row ceiling lookup for playback idx %u", i);

	/* Clamp before touching hardware: an out-of-range 7-bit-violating
	 * value must never exceed the row ceiling. */
	CHECK(led_level_to_pulse_us(LED_IDX_TRACK1, 255) == LED_TRACK_MAX_PULSE_US,
	      "level 255 clamped to the track row ceiling");
	CHECK(led_level_to_pulse_us(LED_IDX_PLAYBACK1, 200) == LED_PLAYBACK_MAX_PULSE_US,
	      "level 200 clamped to the playback row ceiling");

	/* Monotonic: brightness never decreases as level increases. */
	uint32_t prev = 0;
	int monotonic = 1;

	for (int lvl = 0; lvl <= (int)LED_LEVEL_MAX; lvl++) {
		uint32_t p = led_level_to_pulse_us(LED_IDX_PLAYBACK1, (uint8_t)lvl);

		if (p < prev)
			monotonic = 0;
		prev = p;
	}
	CHECK(monotonic, "level -> pulse is monotonic non-decreasing over 0..127");
}

/* ------------------------------------------------------------------------
 * MIDI channel/CC dispatch
 * ------------------------------------------------------------------------ */
static void test_midi_dispatch(void)
{
	led_midi_decoded_t d;

	/* 17. channel-1 controls (and every channel but 16) never enter the
	 * LED protocol, even on the exact same CC numbers. */
	for (uint8_t ch = 0; ch < 16; ch++) {
		if (ch == LED_MIDI_CHANNEL)
			continue;
		d = led_midi_decode(ch, LED_CC_STAGE_FIRST, 100);
		CHECK(d.action == LED_MIDI_ACTION_NONE,
		      "channel %u CC80 (not channel 16): ignored", ch);
		d = led_midi_decode(ch, LED_CC_COMMIT, 5);
		CHECK(d.action == LED_MIDI_ACTION_NONE,
		      "channel %u CC88 (not channel 16): ignored", ch);
	}

	/* 18. channel-16 traffic decodes to LED-only actions; the decoder
	 * never touches surface state (it is a pure function with no
	 * reference to any surface/midi_note/decode_bands symbol — the type
	 * signature above proves it cannot). */
	for (uint8_t i = 0; i < LED_PHYSICAL_COUNT; i++) {
		d = led_midi_decode(LED_MIDI_CHANNEL, (uint8_t)(LED_CC_STAGE_FIRST + i), 42);
		CHECK(d.action == LED_MIDI_ACTION_STAGE && d.index == i && d.value == 42,
		      "channel 16 CC%u -> STAGE index %u value 42", LED_CC_STAGE_FIRST + i, i);
	}
	d = led_midi_decode(LED_MIDI_CHANNEL, LED_CC_COMMIT, 7);
	CHECK(d.action == LED_MIDI_ACTION_COMMIT && d.value == 7, "channel 16 CC88 -> COMMIT seq 7");
	d = led_midi_decode(LED_MIDI_CHANNEL, LED_CC_HEARTBEAT, 7);
	CHECK(d.action == LED_MIDI_ACTION_HEARTBEAT && d.value == 7, "channel 16 CC89 -> HEARTBEAT seq 7");
	d = led_midi_decode(LED_MIDI_CHANNEL, LED_CC_RELEASE, 0);
	CHECK(d.action == LED_MIDI_ACTION_RELEASE, "channel 16 CC90 -> RELEASE");
	d = led_midi_decode(LED_MIDI_CHANNEL, LED_CC_CAPABILITY, 0);
	CHECK(d.action == LED_MIDI_ACTION_CAPABILITY_QUERY, "channel 16 CC91 value 0 -> CAPABILITY_QUERY");
	d = led_midi_decode(LED_MIDI_CHANNEL, LED_CC_CAPABILITY, 3);
	CHECK(d.action == LED_MIDI_ACTION_NONE, "channel 16 CC91 value != 0 -> ignored, not a query");
	d = led_midi_decode(LED_MIDI_CHANNEL, 79, 1);
	CHECK(d.action == LED_MIDI_ACTION_NONE, "channel 16 CC79 (below the reserved range) -> ignored");
	d = led_midi_decode(LED_MIDI_CHANNEL, 92, 1);
	CHECK(d.action == LED_MIDI_ACTION_NONE, "channel 16 CC92 (above the reserved range) -> ignored");
}

/* ------------------------------------------------------------------------
 * Frame helpers
 * ------------------------------------------------------------------------ */
static void stage_all(led_frame_state_t *s, const uint8_t levels[LED_PHYSICAL_COUNT])
{
	for (uint8_t i = 0; i < LED_PHYSICAL_COUNT; i++)
		led_frame_stage(s, i, levels[i]);
}

/* ------------------------------------------------------------------------
 * 5. stage without visible application
 * ------------------------------------------------------------------------ */
static void test_stage_not_visible(void)
{
	led_frame_state_t s;

	led_frame_reset(&s);
	led_frame_stage(&s, LED_IDX_TRACK1, 100);
	CHECK(s.active[LED_IDX_TRACK1] == 0, "staging one index leaves `active` untouched");

	uint8_t levels[LED_PHYSICAL_COUNT] = { 10, 20, 30, 40, 50, 60, 70, 80 };

	stage_all(&s, levels);
	int all_zero = 1;

	for (uint8_t i = 0; i < LED_PHYSICAL_COUNT; i++)
		if (s.active[i] != 0)
			all_zero = 0;
	CHECK(all_zero, "staging all 8 indices still leaves `active` untouched before commit");
}

/* ------------------------------------------------------------------------
 * 6/7. incomplete first frame rejected / complete first frame accepted
 * ------------------------------------------------------------------------ */
static void test_first_commit(void)
{
	led_frame_state_t s;

	led_frame_reset(&s);
	led_frame_stage(&s, LED_IDX_TRACK1, 50);
	led_frame_stage(&s, LED_IDX_TRACK2, 60);
	/* Only 2 of 8 staged. */
	led_commit_result_t r = led_frame_commit(&s, 1, 1000);

	CHECK(r == LED_COMMIT_REJECTED_INCOMPLETE, "commit with only 2/8 staged is rejected");
	CHECK(!s.owned, "a rejected first commit does not create ownership");
	CHECK(s.rejected_commits == 1, "rejected_commits counted");

	uint8_t levels[LED_PHYSICAL_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	stage_all(&s, levels);
	r = led_frame_commit(&s, 1, 1000);
	CHECK(r == LED_COMMIT_ACCEPTED, "commit with all 8/8 staged is accepted");
	CHECK(s.owned, "ownership begins only after a valid complete commit");
	CHECK(memcmp(s.active, levels, LED_PHYSICAL_COUNT) == 0,
	      "the complete staged frame is copied to `active` atomically");
	CHECK(s.last_seq == 1 && s.valid_commits == 1, "sequence and valid-commit count recorded");
}

/* ------------------------------------------------------------------------
 * 8. partial subsequent frame retaining unchanged values
 * ------------------------------------------------------------------------ */
static void test_partial_subsequent_commit(void)
{
	led_frame_state_t s;
	uint8_t levels[LED_PHYSICAL_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	led_frame_reset(&s);
	stage_all(&s, levels);
	led_frame_commit(&s, 1, 1000);

	/* Update only index 3; the rest of staged[] retains its prior value
	 * from the full frame above. */
	led_frame_stage(&s, 3, 99);
	led_commit_result_t r = led_frame_commit(&s, 2, 1100);

	CHECK(r == LED_COMMIT_ACCEPTED, "partial subsequent commit accepted");
	CHECK(s.active[3] == 99, "the updated index takes effect");
	uint8_t expect[LED_PHYSICAL_COUNT] = { 1, 2, 3, 99, 5, 6, 7, 8 };

	CHECK(memcmp(s.active, expect, LED_PHYSICAL_COUNT) == 0,
	      "every untouched index retains its previously staged value");
}

/* ------------------------------------------------------------------------
 * 9. duplicate commit idempotence
 * ------------------------------------------------------------------------ */
static void test_duplicate_commit(void)
{
	led_frame_state_t s;
	uint8_t levels[LED_PHYSICAL_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	led_frame_reset(&s);
	stage_all(&s, levels);
	led_frame_commit(&s, 5, 1000);
	uint8_t before[LED_PHYSICAL_COUNT];

	memcpy(before, s.active, LED_PHYSICAL_COUNT);

	led_commit_result_t r1 = led_frame_commit(&s, 5, 1010);
	led_commit_result_t r2 = led_frame_commit(&s, 5, 1020);

	CHECK(r1 == LED_COMMIT_ACCEPTED_DUPLICATE && r2 == LED_COMMIT_ACCEPTED_DUPLICATE,
	      "repeating the same committed sequence is reported as duplicate, every time");
	CHECK(memcmp(s.active, before, LED_PHYSICAL_COUNT) == 0,
	      "a duplicate commit never changes `active` (idempotent)");
	CHECK(s.duplicate_commits == 2, "duplicate_commits counts every repeat");
}

/* ------------------------------------------------------------------------
 * 10/11. modulo-128 sequence wrap + stale commit rejection
 * ------------------------------------------------------------------------ */
static void test_sequence_wrap_and_stale(void)
{
	CHECK(led_seq_compare(1, 0) > 0, "1 is newer than 0");
	CHECK(led_seq_compare(0, 1) < 0, "0 is older than 1 (not wrapped)");
	CHECK(led_seq_compare(5, 5) == 0, "equal sequence numbers compare equal");
	/* Wrap: 0 is newer than 127 (the 127 -> 0 rollover). */
	CHECK(led_seq_compare(0, 127) > 0, "seq wraps: 0 is newer than 127");
	CHECK(led_seq_compare(127, 0) < 0, "seq wraps: 127 is older than 0");
	/* Half-window boundary. */
	CHECK(led_seq_compare(63, 0) > 0, "forward distance 63 (just inside the window) is newer");
	CHECK(led_seq_compare(64, 0) < 0, "forward distance 64 (at the window edge) is stale");

	led_frame_state_t s;
	uint8_t levels[LED_PHYSICAL_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	led_frame_reset(&s);
	stage_all(&s, levels);
	led_frame_commit(&s, 10, 1000);
	uint8_t before[LED_PHYSICAL_COUNT];

	memcpy(before, s.active, LED_PHYSICAL_COUNT);
	led_frame_stage(&s, 0, 250); /* would be visible if the stale commit below were misapplied */

	led_commit_result_t r = led_frame_commit(&s, 3, 1010); /* 3 is "behind" 10 across the wrap */

	CHECK(r == LED_COMMIT_REJECTED_STALE, "an out-of-order sequence is rejected as stale");
	CHECK(memcmp(s.active, before, LED_PHYSICAL_COUNT) == 0,
	      "a rejected stale commit never changes `active`");
	CHECK(s.last_seq == 10, "the last accepted sequence is unchanged by a stale rejection");
	CHECK(s.rejected_commits == 1, "rejected_commits counts the stale rejection");
}

/* ------------------------------------------------------------------------
 * 12/13. heartbeat extends the lease / never creates ownership
 * ------------------------------------------------------------------------ */
static void test_heartbeat(void)
{
	led_frame_state_t s;

	led_frame_reset(&s);
	led_heartbeat_result_t hb = led_frame_heartbeat(&s, 0, 1000);

	CHECK(hb == LED_HEARTBEAT_IGNORED_NOT_OWNED, "heartbeat before any commit is ignored");
	CHECK(!s.owned, "heartbeat never begins ownership by itself");

	uint8_t levels[LED_PHYSICAL_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	stage_all(&s, levels);
	led_frame_commit(&s, 1, 1000); /* lease deadline = 1000 + 1000 = 2000 */

	CHECK(!led_frame_check_lease_timeout(&s, 1999), "not yet timed out just before the deadline");
	hb = led_frame_heartbeat(&s, 1, 1900);
	CHECK(hb == LED_HEARTBEAT_EXTENDED, "heartbeat while owned extends the lease");
	CHECK(s.lease_deadline_ms == 1900 + LED_LEASE_TIMEOUT_MS, "lease deadline pushed out by the heartbeat");
	CHECK(!led_frame_check_lease_timeout(&s, 2500),
	      "the heartbeat-extended lease survives past the original (un-extended) deadline");
}

/* ------------------------------------------------------------------------
 * 14. explicit release
 * ------------------------------------------------------------------------ */
static void test_explicit_release(void)
{
	led_frame_state_t s;
	uint8_t levels[LED_PHYSICAL_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	led_frame_reset(&s);
	stage_all(&s, levels);
	led_frame_commit(&s, 1, 1000);

	led_frame_release(&s, LED_RELEASE_EXPLICIT, 1500);
	CHECK(!s.owned, "explicit release clears ownership");
	CHECK(!led_frame_all_staged(&s), "explicit release clears staging completeness");
	int all_zero = 1;

	for (uint8_t i = 0; i < LED_PHYSICAL_COUNT; i++)
		if (s.active[i] != 0)
			all_zero = 0;
	CHECK(all_zero, "explicit release turns the runtime frame off");
	CHECK(s.explicit_releases == 1, "explicit_releases counted");

	/* "require a new complete eight-channel frame before takeover resumes" */
	led_frame_stage(&s, 0, 9);
	led_commit_result_t r = led_frame_commit(&s, 2, 1600);

	CHECK(r == LED_COMMIT_REJECTED_INCOMPLETE,
	      "a partial frame after release is rejected exactly like the very first commit");
}

/* ------------------------------------------------------------------------
 * 15. timeout release
 * ------------------------------------------------------------------------ */
static void test_timeout_release(void)
{
	led_frame_state_t s;
	uint8_t levels[LED_PHYSICAL_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	led_frame_reset(&s);
	stage_all(&s, levels);
	led_frame_commit(&s, 1, 1000); /* deadline 2000 */

	CHECK(!led_frame_check_lease_timeout(&s, 1999), "no timeout 1 ms before the deadline");
	bool timed_out = led_frame_check_lease_timeout(&s, 2000);

	CHECK(timed_out, "lease timeout fires exactly at the 1000 ms deadline");
	CHECK(!s.owned, "a timed-out lease clears ownership");
	CHECK(s.lease_timeouts == 1, "lease_timeouts counted");
	int all_zero = 1;

	for (uint8_t i = 0; i < LED_PHYSICAL_COUNT; i++)
		if (s.active[i] != 0)
			all_zero = 0;
	CHECK(all_zero, "a timed-out lease turns the runtime frame off — never stays illuminated");
}

/* ------------------------------------------------------------------------
 * 16. MIDI disconnect release
 * ------------------------------------------------------------------------ */
static void test_disconnect_release(void)
{
	led_frame_state_t s;
	uint8_t levels[LED_PHYSICAL_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	led_frame_reset(&s);
	stage_all(&s, levels);
	led_frame_commit(&s, 1, 1000);

	led_frame_release(&s, LED_RELEASE_DISCONNECT, 1200);
	CHECK(!s.owned, "MIDI disconnect release clears ownership");
	CHECK(s.disconnect_releases == 1, "disconnect_releases counted");
	CHECK(s.explicit_releases == 0 && s.lease_timeouts == 0,
	      "disconnect release increments only its own counter");
}

/* REINIT (boot / fresh MIDI connect) bumps no diagnostic counter. */
static void test_reinit_release_is_silent(void)
{
	led_frame_state_t s;
	uint8_t levels[LED_PHYSICAL_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	led_frame_reset(&s);
	stage_all(&s, levels);
	led_frame_commit(&s, 1, 1000);

	led_frame_release(&s, LED_RELEASE_REINIT, 1200);
	CHECK(!s.owned, "REINIT release clears ownership");
	CHECK(s.explicit_releases == 0 && s.lease_timeouts == 0 && s.disconnect_releases == 0,
	      "REINIT (boot / fresh MIDI connect) increments no misbehavior counter");
}

/* ------------------------------------------------------------------------
 * 19. safety override outranking host ownership
 * ------------------------------------------------------------------------ */
static void test_safety_precedence(void)
{
	CHECK(led_render_select(true, true) == LED_RENDER_SOURCE_PATTERN,
	      "safety active + host owned -> safety pattern wins");
	CHECK(led_render_select(true, false) == LED_RENDER_SOURCE_PATTERN,
	      "safety active + host not owned -> safety pattern wins");
	CHECK(led_render_select(false, true) == LED_RENDER_SOURCE_HOST,
	      "safety inactive + host owned -> host frame wins");
	CHECK(led_render_select(false, false) == LED_RENDER_SOURCE_PATTERN,
	      "safety inactive + host not owned -> idle pattern fallback");
}

int main(void)
{
	test_index_to_pin_mapping();
	test_level_to_duty();
	test_midi_dispatch();
	test_stage_not_visible();
	test_first_commit();
	test_partial_subsequent_commit();
	test_duplicate_commit();
	test_sequence_wrap_and_stale();
	test_heartbeat();
	test_explicit_release();
	test_timeout_release();
	test_disconnect_release();
	test_reinit_release_is_silent();
	test_safety_precedence();

	printf("\n");
	if (g_failures) {
		printf("LED SELF-TEST FAILED (%d of %d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("LED SELF-TEST PASSED (%d checks)\n", g_checks);
	return 0;
}
