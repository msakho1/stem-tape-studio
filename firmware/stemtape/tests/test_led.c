/*
 * test_led.c — Stem Tape LED Feedback Protocol v1: host-runnable tests.
 *
 * Builds and runs with the host's own C compiler, no Zephyr/nRF toolchain,
 * no hardware:
 *
 *     cc -std=c11 -Wall -Wextra -I../src \
 *        ../src/led_duty.c ../src/led_frame.c ../src/led_midi.c \
 *        ../src/led_battery.c test_led.c \
 *        -o test_led && ./test_led
 *
 * This links the EXACT same led_duty.c/led_frame.c/led_midi.c/led_battery.c
 * translation units the firmware compiles (see ../CMakeLists.txt) — nothing
 * here is a reimplementation of the protocol logic, so a pass here is
 * evidence about the real firmware behavior, not a parallel model of it.
 * led_render.c is NOT linked here (it touches the Zephyr PWM driver); its
 * hardware-facing behavior is verified only by code review and the full
 * Zephyr build/CI, not by this file.
 *
 * Exit code 0 = every check passed. Any failure prints [FAIL] and the
 * process exits non-zero, matching the repo's other self-checking audit
 * scripts (.github/scripts/m0_assertions_selftest.py).
 */

#include <stdio.h>
#include <string.h>

#include "led_battery.h"
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
 * exact eight-output inventory + physical-location-to-GPIO mapping
 * ------------------------------------------------------------------------ */
static void test_index_to_pin_mapping(void)
{
	/* Exactly eight MCU-controllable LEDs — no ninth/tenth channel for
	 * the Function dots or the red triangle (those are static enclosure
	 * markings, not LEDs). */
	CHECK(LED_PHYSICAL_COUNT == 8u, "exactly eight physical LED channels");

	/* Track row: identity, confirmed unambiguous by all sources. Side
	 * row: PLAY-end-to-FUNCTION-end per led_protocol.h's best-effort
	 * inference (this firmware's own pinned leds[] array order) — see
	 * that header's physical-inventory comment for why neither
	 * community source's numbering could be used instead. */
	static const led_physical_pin_t expected[LED_PHYSICAL_COUNT] = {
		{ 0, 29 }, { 0, 26 }, { 1, 15 }, { 1, 14 }, /* Track 1-4 */
		{ 1, 13 }, { 0,  0 }, { 1, 12 }, { 0,  1 }, /* side: PLAY..FUNCTION */
	};

	for (uint8_t i = 0; i < LED_PHYSICAL_COUNT; i++) {
		CHECK(led_physical_pin_map[i].port == expected[i].port &&
		      led_physical_pin_map[i].pin == expected[i].pin,
		      "index %u -> P%u.%02u", i,
		      led_physical_pin_map[i].port, led_physical_pin_map[i].pin);
	}

	/* The named endpoints match the table above. */
	CHECK(led_physical_pin_map[LED_IDX_SIDE_PLAY].port == 1 &&
	      led_physical_pin_map[LED_IDX_SIDE_PLAY].pin == 13,
	      "LED_IDX_SIDE_PLAY (nearest PLAY) -> P1.13");
	CHECK(led_physical_pin_map[LED_IDX_SIDE_FUNCTION].port == 0 &&
	      led_physical_pin_map[LED_IDX_SIDE_FUNCTION].pin == 1,
	      "LED_IDX_SIDE_FUNCTION (nearest FUNCTION) -> P0.01");
}

/* ------------------------------------------------------------------------
 * level-to-track-duty, level-to-side-duty, zero/max clamping
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

	/* Side row (idx 4..7): ceiling 66 us. */
	CHECK(led_level_to_pulse_us(LED_IDX_SIDE_PLAY, 0) == 0,
	      "side level 0 -> 0 us (truly off)");
	CHECK(led_level_to_pulse_us(LED_IDX_SIDE_FUNCTION, LED_LEVEL_MAX) == LED_SIDE_MAX_PULSE_US,
	      "side level 127 -> %u us (row ceiling)", LED_SIDE_MAX_PULSE_US);
	for (uint8_t i = LED_TRACK_ROW_COUNT; i < LED_PHYSICAL_COUNT; i++)
		CHECK(led_row_max_pulse_us(i) == LED_SIDE_MAX_PULSE_US,
		      "row ceiling lookup for side idx %u", i);

	/* Clamp before touching hardware: an out-of-range 7-bit-violating
	 * value must never exceed the row ceiling. */
	CHECK(led_level_to_pulse_us(LED_IDX_TRACK1, 255) == LED_TRACK_MAX_PULSE_US,
	      "level 255 clamped to the track row ceiling");
	CHECK(led_level_to_pulse_us(LED_IDX_SIDE_PLAY, 200) == LED_SIDE_MAX_PULSE_US,
	      "level 200 clamped to the side row ceiling");

	/* Monotonic: brightness never decreases as level increases. */
	uint32_t prev = 0;
	int monotonic = 1;

	for (int lvl = 0; lvl <= (int)LED_LEVEL_MAX; lvl++) {
		uint32_t p = led_level_to_pulse_us(LED_IDX_SIDE_PLAY, (uint8_t)lvl);

		if (p < prev)
			monotonic = 0;
		prev = p;
	}
	CHECK(monotonic, "level -> pulse is monotonic non-decreasing over 0..127");
}

/* ------------------------------------------------------------------------
 * unchanged-frame render suppression (led_duty_diff_frame)
 * ------------------------------------------------------------------------ */
static void test_diff_frame(void)
{
	uint8_t cache[LED_PHYSICAL_COUNT] = { 0 };
	uint8_t frame[LED_PHYSICAL_COUNT] = { 0 };
	bool changed[LED_PHYSICAL_COUNT];

	CHECK(!led_duty_diff_frame(cache, frame, changed),
	      "identical all-zero frame against a zeroed cache: nothing changed");

	frame[LED_IDX_TRACK2] = 80;
	frame[LED_IDX_SIDE_FUNCTION] = 40;
	bool any = led_duty_diff_frame(cache, frame, changed);

	CHECK(any, "two channels changed: diff reports something changed");
	for (uint8_t i = 0; i < LED_PHYSICAL_COUNT; i++) {
		bool expect = (i == LED_IDX_TRACK2 || i == LED_IDX_SIDE_FUNCTION);

		CHECK(changed[i] == expect, "channel %u changed flag == %d", i, (int)expect);
	}
	CHECK(memcmp(cache, frame, LED_PHYSICAL_COUNT) == 0,
	      "cache updated to match the new frame after a diff");

	/* Re-applying the SAME frame now reports nothing changed. */
	CHECK(!led_duty_diff_frame(cache, frame, changed),
	      "resending the same frame a second time: nothing changed (skip the resend)");
}

/* ------------------------------------------------------------------------
 * MIDI channel/CC dispatch
 * ------------------------------------------------------------------------ */
static void test_midi_dispatch(void)
{
	led_midi_decoded_t d;

	/* channel-1 controls (and every channel but 16) never enter the LED
	 * protocol, even on the exact same CC numbers. */
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

	/* channel-16 traffic decodes to LED-only actions; the decoder never
	 * touches surface state (it is a pure function with no reference to
	 * any surface/midi_note/decode_bands symbol — the type signature
	 * above proves it cannot). */
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
 * Battery / Play local baseline
 * ------------------------------------------------------------------------ */
static void test_battery_baseline(void)
{
	CHECK(led_battery_step(0) == 0, "battery value 0 -> step 0 (empty)");
	CHECK(led_battery_step(127) == LED_BATTERY_STEP_COUNT, "battery value 127 -> step 4 (full)");
	CHECK(led_battery_is_low(0), "value 0 is the low-battery step");
	CHECK(!led_battery_is_low(127), "value 127 is not the low-battery step");

	/* Monotonic step function. */
	uint8_t prev_step = 0;
	int monotonic = 1;

	for (int v = 0; v <= (int)LED_LEVEL_MAX; v++) {
		uint8_t step = led_battery_step((uint8_t)v);

		if (step < prev_step)
			monotonic = 0;
		prev_step = step;
	}
	CHECK(monotonic, "battery value -> step is monotonic non-decreasing");

	/* Full frame: Track row always off, side row lit ascending from
	 * SIDE_PLAY toward SIDE_FUNCTION, exactly `step` of the 4 lit. */
	uint8_t frame[LED_PHYSICAL_COUNT];

	led_battery_frame(127, frame); /* step 4: all 4 side LEDs lit */
	for (uint8_t i = 0; i < LED_TRACK_ROW_COUNT; i++)
		CHECK(frame[i] == 0, "battery frame: Track LED %u stays off", i);
	for (uint8_t i = 0; i < LED_SIDE_ROW_COUNT; i++)
		CHECK(frame[LED_TRACK_ROW_COUNT + i] == LED_LEVEL_MAX,
		      "battery frame at full charge: side LED %u fully lit", i);

	led_battery_frame(0, frame); /* step 0: nothing lit */
	for (uint8_t i = 0; i < LED_SIDE_ROW_COUNT; i++)
		CHECK(frame[LED_TRACK_ROW_COUNT + i] == 0,
		      "battery frame at empty: side LED %u off", i);

	/* A mid-range value lights a partial, ascending-from-PLAY count. */
	uint8_t mid_step = led_battery_step(70);

	led_battery_frame(70, frame);
	for (uint8_t i = 0; i < LED_SIDE_ROW_COUNT; i++) {
		bool expect_lit = i < mid_step;

		CHECK((frame[LED_TRACK_ROW_COUNT + i] == LED_LEVEL_MAX) == expect_lit,
		      "battery frame value=70 (step %u): side LED %u lit == %d",
		      mid_step, i, (int)expect_lit);
	}
}

/* ------------------------------------------------------------------------
 * Render-source precedence, including the local battery/Play fallback and
 * the low-battery override
 * ------------------------------------------------------------------------ */
static void test_render_precedence(void)
{
	CHECK(led_render_select(true, false, true) == LED_RENDER_SOURCE_PATTERN,
	      "safety active always wins, even with a host frame and battery OK");
	CHECK(led_render_select(true, true, true) == LED_RENDER_SOURCE_PATTERN,
	      "safety active wins over low battery AND a host frame");
	CHECK(led_render_select(false, true, true) == LED_RENDER_SOURCE_LOCAL,
	      "low battery outranks an owned host frame (no safety pattern active)");
	CHECK(led_render_select(false, true, false) == LED_RENDER_SOURCE_LOCAL,
	      "low battery + no host frame: local baseline");
	CHECK(led_render_select(false, false, true) == LED_RENDER_SOURCE_HOST,
	      "battery OK + host owned + no safety: host frame wins");
	CHECK(led_render_select(false, false, false) == LED_RENDER_SOURCE_LOCAL,
	      "battery OK + no host frame: local baseline, NOT all-off");
}

static void test_capability_gate(void)
{
	CHECK(led_capability_should_answer(true), "renderer ready: capability query is answered");
	CHECK(!led_capability_should_answer(false),
	      "renderer NOT ready: capability query is never answered as supported");
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
 * stage without visible application
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
 * incomplete first frame rejected / complete first frame accepted
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
	CHECK(s.last_activity_ms == 1000, "last_activity_ms recorded at commit time");
}

/* ------------------------------------------------------------------------
 * partial subsequent frame retaining unchanged values
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
 * duplicate commit idempotence
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
 * modulo-128 sequence wrap + stale commit rejection
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
 * Heartbeat sequence validation: matching / stale / future
 * ------------------------------------------------------------------------ */
static void test_heartbeat_sequence_validation(void)
{
	led_frame_state_t s;
	uint8_t levels[LED_PHYSICAL_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	led_frame_reset(&s);

	led_heartbeat_result_t hb = led_frame_heartbeat(&s, 0, 1000);

	CHECK(hb == LED_HEARTBEAT_IGNORED_NOT_OWNED, "heartbeat before any commit is ignored");
	CHECK(!s.owned, "heartbeat never begins ownership by itself");

	stage_all(&s, levels);
	led_frame_commit(&s, 5, 1000); /* last_seq = 5 */

	/* Matching: extends. */
	hb = led_frame_heartbeat(&s, 5, 1100);
	CHECK(hb == LED_HEARTBEAT_EXTENDED, "heartbeat matching the last committed sequence extends the lease");
	CHECK(s.last_activity_ms == 1100, "matching heartbeat records last_activity_ms");
	CHECK(s.stale_heartbeats == 0, "no stale-heartbeat counted for a match");

	/* Stale: seq behind last_seq. Must NOT extend. */
	hb = led_frame_heartbeat(&s, 4, 1150);
	CHECK(hb == LED_HEARTBEAT_REJECTED_SEQ_MISMATCH,
	      "heartbeat with a STALE sequence (4, last committed 5) is rejected");
	CHECK(s.last_activity_ms == 1100, "a mismatched heartbeat does not extend the lease");
	CHECK(s.stale_heartbeats == 1, "stale_heartbeats counted for the stale mismatch");

	/* Future: seq ahead of last_seq (host hasn't actually committed it
	 * yet). Must also NOT extend — "must extend ownership only when its
	 * value EQUALS the most recently accepted commit sequence". */
	hb = led_frame_heartbeat(&s, 6, 1200);
	CHECK(hb == LED_HEARTBEAT_REJECTED_SEQ_MISMATCH,
	      "heartbeat with a FUTURE sequence (6, last committed 5) is rejected");
	CHECK(s.last_activity_ms == 1100, "a future-seq heartbeat does not extend the lease either");
	CHECK(s.stale_heartbeats == 2, "stale_heartbeats counts every mismatch, stale or future");

	/* Confirm the lease actually expires on schedule against the
	 * MATCHING heartbeat's activity time (1100) — the two mismatched
	 * heartbeats after it never advanced last_activity_ms, so the
	 * deadline is 1100 + LED_LEASE_TIMEOUT_MS, not any later. */
	CHECK(s.last_activity_ms == 1100, "last_activity_ms still 1100: mismatches never advanced it");
	CHECK(!led_frame_check_lease_timeout(&s, 1100 + LED_LEASE_TIMEOUT_MS - 1),
	      "not yet timed out one ms before the (unextended-by-mismatches) deadline");
	CHECK(led_frame_check_lease_timeout(&s, 1100 + LED_LEASE_TIMEOUT_MS),
	      "times out exactly on schedule: the mismatched heartbeats never extended it");
}

/* ------------------------------------------------------------------------
 * Explicit release, timeout release, disconnect release: full transactional
 * reset of session state, cumulative counters preserved
 * ------------------------------------------------------------------------ */
static void check_full_session_clear(const led_frame_state_t *s, const char *why)
{
	int staged_zero = 1, active_zero = 1;

	for (uint8_t i = 0; i < LED_PHYSICAL_COUNT; i++) {
		if (s->staged[i] != 0)
			staged_zero = 0;
		if (s->active[i] != 0)
			active_zero = 0;
	}
	CHECK(!s->owned, "%s: ownership cleared", why);
	CHECK(s->staged_mask == 0, "%s: staged_mask cleared", why);
	CHECK(staged_zero, "%s: every staged[] value cleared (no stale value leaks into a future frame)", why);
	CHECK(active_zero, "%s: every active[] value cleared (runtime frame off, not stale)", why);
	CHECK(!s->had_commit, "%s: had_commit cleared", why);
	CHECK(s->last_seq == 0, "%s: last_seq cleared", why);
	CHECK(s->last_activity_ms == 0, "%s: last_activity_ms cleared", why);
}

static void test_explicit_release(void)
{
	led_frame_state_t s;
	uint8_t levels[LED_PHYSICAL_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	led_frame_reset(&s);
	stage_all(&s, levels);
	led_frame_commit(&s, 1, 1000);

	led_frame_release(&s, LED_RELEASE_EXPLICIT, 1500);
	check_full_session_clear(&s, "explicit release");
	CHECK(s.explicit_releases == 1, "explicit_releases counted");
	CHECK(s.valid_commits == 1, "cumulative diagnostic counters survive the release");

	/* "require a new complete eight-channel frame before takeover resumes" —
	 * and pre-release staged values must NOT silently complete it. */
	led_frame_stage(&s, 0, 9);
	led_commit_result_t r = led_frame_commit(&s, 2, 1600);

	CHECK(r == LED_COMMIT_REJECTED_INCOMPLETE,
	      "a single re-staged index after release is rejected exactly like the very first commit");
}

static void test_timeout_release(void)
{
	led_frame_state_t s;
	uint8_t levels[LED_PHYSICAL_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	led_frame_reset(&s);
	stage_all(&s, levels);
	led_frame_commit(&s, 1, 1000); /* last_activity_ms = 1000, deadline 2000 */

	CHECK(!led_frame_check_lease_timeout(&s, 1999), "no timeout 1 ms before the deadline");
	bool timed_out = led_frame_check_lease_timeout(&s, 2000);

	CHECK(timed_out, "lease timeout fires exactly at the 1000 ms deadline");
	check_full_session_clear(&s, "timeout release");
	CHECK(s.lease_timeouts == 1, "lease_timeouts counted");
}

static void test_disconnect_release(void)
{
	led_frame_state_t s;
	uint8_t levels[LED_PHYSICAL_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	led_frame_reset(&s);
	stage_all(&s, levels);
	led_frame_commit(&s, 1, 1000);

	led_frame_release(&s, LED_RELEASE_DISCONNECT, 1200);
	check_full_session_clear(&s, "disconnect release");
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
	check_full_session_clear(&s, "REINIT release");
	CHECK(s.explicit_releases == 0 && s.lease_timeouts == 0 && s.disconnect_releases == 0 &&
	      s.render_failure_releases == 0,
	      "REINIT (boot / fresh MIDI connect) increments no misbehavior counter");
}

/* Renderer-failure release: its own counter, and nothing else. */
static void test_render_failure_release(void)
{
	led_frame_state_t s;
	uint8_t levels[LED_PHYSICAL_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	led_frame_reset(&s);
	stage_all(&s, levels);
	led_frame_commit(&s, 1, 1000);

	led_frame_release(&s, LED_RELEASE_RENDER_FAILURE, 1050);
	check_full_session_clear(&s, "render-failure release");
	CHECK(s.render_failure_releases == 1, "render_failure_releases counted");
	CHECK(s.explicit_releases == 0 && s.lease_timeouts == 0 && s.disconnect_releases == 0,
	      "render-failure release increments only its own counter");
}

/* ------------------------------------------------------------------------
 * Lease timeout: wrap-safe unsigned elapsed-time arithmetic across
 * UINT32_MAX
 * ------------------------------------------------------------------------ */
static void test_uptime_wrap(void)
{
	led_frame_state_t s;
	uint8_t levels[LED_PHYSICAL_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint32_t near_max = 0xFFFFFFFFu - 200u; /* commit 200 ms before the counter wraps */

	led_frame_reset(&s);
	stage_all(&s, levels);
	led_commit_result_t r = led_frame_commit(&s, 1, near_max);

	CHECK(r == LED_COMMIT_ACCEPTED, "commit accepted right before uint32 wraparound");
	CHECK(s.last_activity_ms == near_max, "last_activity_ms recorded at the pre-wrap timestamp");

	/* Pre-deadline check, still before the wrap. */
	CHECK(!led_frame_check_lease_timeout(&s, near_max + 199u),
	      "not yet timed out 199 ms after a pre-wrap commit (limit is 1000 ms)");

	/* Matching heartbeat right at the wrap boundary extends the lease. */
	led_heartbeat_result_t hb = led_frame_heartbeat(&s, 1, 0xFFFFFFFFu);

	CHECK(hb == LED_HEARTBEAT_EXTENDED, "heartbeat at 0xFFFFFFFF (one ms before wrap) extends the lease");
	CHECK(s.last_activity_ms == 0xFFFFFFFFu, "last_activity_ms == 0xFFFFFFFF");

	/* now_ms has wrapped around to a small value: 50 ms of wall-clock
	 * time after 0xFFFFFFFF is now_ms == 49 (0xFFFFFFFF + 50 == 49,
	 * modulo 2^32). Elapsed = (uint32_t)(49 - 0xFFFFFFFF) == 50. */
	CHECK(!led_frame_check_lease_timeout(&s, 49u),
	      "50 ms of wrap-safe elapsed time is still well under the 1000 ms deadline");

	/* Exact timeout, computed the same wrap-safe way: elapsed reaches
	 * exactly LED_LEASE_TIMEOUT_MS at now_ms == 0xFFFFFFFF + 1000 (mod
	 * 2^32) == 999 (since 0xFFFFFFFF + 1 wraps to 0). */
	uint32_t exact_timeout_now = (uint32_t)(0xFFFFFFFFu + LED_LEASE_TIMEOUT_MS);

	CHECK(!led_frame_check_lease_timeout(&s, exact_timeout_now - 1u),
	      "one ms before exact timeout, across the wrap: still not timed out");
	CHECK(led_frame_check_lease_timeout(&s, exact_timeout_now),
	      "exact timeout fires correctly across a uint32 wraparound");
	check_full_session_clear(&s, "post-wrap timeout release");

	/* Post-timeout reacquisition, with now_ms itself a small
	 * (post-wrap) value: the ordinary "complete frame required" rule
	 * still applies and still works. */
	led_frame_stage(&s, 0, 5); /* only 1 of 8: incomplete */
	led_commit_result_t r2 = led_frame_commit(&s, 1, exact_timeout_now + 10u);

	CHECK(r2 == LED_COMMIT_REJECTED_INCOMPLETE,
	      "post-wrap reacquisition still requires a complete first frame");

	stage_all(&s, levels);
	led_commit_result_t r3 = led_frame_commit(&s, 1, exact_timeout_now + 10u);

	CHECK(r3 == LED_COMMIT_ACCEPTED,
	      "post-wrap reacquisition succeeds once the frame is complete again");
	CHECK(s.owned, "ownership re-established after the wrap-boundary timeout");
}

int main(void)
{
	test_index_to_pin_mapping();
	test_level_to_duty();
	test_diff_frame();
	test_midi_dispatch();
	test_battery_baseline();
	test_render_precedence();
	test_capability_gate();
	test_stage_not_visible();
	test_first_commit();
	test_partial_subsequent_commit();
	test_duplicate_commit();
	test_sequence_wrap_and_stale();
	test_heartbeat_sequence_validation();
	test_explicit_release();
	test_timeout_release();
	test_disconnect_release();
	test_reinit_release_is_silent();
	test_render_failure_release();
	test_uptime_wrap();

	printf("\n");
	if (g_failures) {
		printf("LED SELF-TEST FAILED (%d of %d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("LED SELF-TEST PASSED (%d checks)\n", g_checks);
	return 0;
}
