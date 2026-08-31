/*
 * test_led.c — Stem Tape LED Feedback Protocol v1: host-runnable tests.
 *
 * Builds and runs with the host's own C compiler, no Zephyr/nRF toolchain,
 * no hardware:
 *
 *     cc -std=c11 -Wall -Wextra -I../src \
 *        ../src/led_duty.c ../src/led_frame.c ../src/led_midi.c \
 *        ../src/led_battery.c ../src/led_render_policy.c test_led.c \
 *        -o test_led && ./test_led
 *
 * This links the EXACT same led_duty.c/led_frame.c/led_midi.c/led_battery.c/
 * led_render_policy.c translation units the firmware compiles (see
 * ../CMakeLists.txt) — nothing here is a reimplementation of the protocol
 * logic, so a pass here is evidence about the real firmware behavior, not a
 * parallel model of it. led_render.c itself is NOT linked here (it touches
 * the Zephyr PWM driver directly); its hardware-facing behavior is verified
 * only by code review and the full Zephyr build/CI. But led_render.c is now
 * a thin adapter over led_render_policy.c's write/retry/fault-latch POLICY,
 * and THAT — the part that decides what to do about a failed write — is
 * pure and IS linked and tested here, driven through a mocked
 * led_channel_write_fn that can be told to fail on demand (see
 * mock_write()/mock_pwm_t below).
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
#include "led_render_policy.h"

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
 * exact eight-output inventory + the ONE authoritative hardware table
 * (index -> physical role -> GPIO -> PWM instance/channel -> gauge step)
 * ------------------------------------------------------------------------ */
static void test_index_to_pin_mapping(void)
{
	/* Exactly eight MCU-controllable LEDs — no ninth/tenth channel for
	 * the Function dots or the red triangle (those are static enclosure
	 * markings, not LEDs). */
	CHECK(LED_PHYSICAL_COUNT == 8u, "exactly eight physical LED channels");

	/* Track row: identity, confirmed unambiguous by all sources, PWM2
	 * channels 0-3 in index order. Side row: PLAY-end-to-FUNCTION-end per
	 * led_protocol.h's best-effort inference (this firmware's own pinned
	 * leds[] array order); PWM3 channels are REVERSED relative to index
	 * order (index 4 -> channel 3 ... index 7 -> channel 0) — this table
	 * is the single source every consumer (led_render.c, the diagnostic
	 * sweep, CDC output, docs) reads from, so a regression here is a
	 * regression everywhere at once instead of driving the wrong channel
	 * or printing a backward sweep. */
	static const led_channel_t expected[LED_PHYSICAL_COUNT] = {
		{ 0, 0, 29, 2, 0, LED_GAUGE_STEP_NONE, "Track 1" },
		{ 1, 0, 26, 2, 1, LED_GAUGE_STEP_NONE, "Track 2" },
		{ 2, 1, 15, 2, 2, LED_GAUGE_STEP_NONE, "Track 3" },
		{ 3, 1, 14, 2, 3, LED_GAUGE_STEP_NONE, "Track 4" },
		{ 4, 1, 13, 3, 3, 0, "Side, nearest PLAY" },
		{ 5, 0,  0, 3, 2, 1, "Side, PLAY-side middle" },
		{ 6, 1, 12, 3, 1, 2, "Side, FUNCTION-side middle" },
		{ 7, 0,  1, 3, 0, 3, "Side, nearest FUNCTION" },
	};

	for (uint8_t i = 0; i < LED_PHYSICAL_COUNT; i++) {
		const led_channel_t *c = &led_channel_table[i];

		CHECK(c->index == i, "index %u -> table entry index == %u", i, i);
		CHECK(c->port == expected[i].port && c->pin == expected[i].pin,
		      "index %u -> P%u.%02u", i, c->port, c->pin);
		CHECK(c->pwm_instance == expected[i].pwm_instance,
		      "index %u -> PWM instance %u", i, c->pwm_instance);
		CHECK(c->pwm_channel == expected[i].pwm_channel,
		      "index %u -> PWM channel %u", i, c->pwm_channel);
		CHECK(c->gauge_step == expected[i].gauge_step,
		      "index %u -> gauge_step %u", i, c->gauge_step);
	}

	/* The named endpoints match the table above. */
	CHECK(led_channel_table[LED_IDX_SIDE_PLAY].port == 1 &&
	      led_channel_table[LED_IDX_SIDE_PLAY].pin == 13,
	      "LED_IDX_SIDE_PLAY (nearest PLAY) -> P1.13");
	CHECK(led_channel_table[LED_IDX_SIDE_FUNCTION].port == 0 &&
	      led_channel_table[LED_IDX_SIDE_FUNCTION].pin == 1,
	      "LED_IDX_SIDE_FUNCTION (nearest FUNCTION) -> P0.01");

	/* The specific regression this table exists to prevent: the side
	 * row's PWM channels are 3,2,1,0 for indices 4-7 — NOT 0,1,2,3. A
	 * hand-derived "index - 4" (the bug that once made the diagnostic
	 * sweep print this backward) would fail every one of these. */
	CHECK(led_channel_table[LED_IDX_SIDE_PLAY].pwm_channel == 3,
	      "regression: index 4 (SIDE_PLAY) -> PWM3 channel 3, not 0");
	CHECK(led_channel_table[LED_IDX_SIDE_MID1].pwm_channel == 2,
	      "regression: index 5 (SIDE_MID1) -> PWM3 channel 2, not 1");
	CHECK(led_channel_table[LED_IDX_SIDE_MID2].pwm_channel == 1,
	      "regression: index 6 (SIDE_MID2) -> PWM3 channel 1, not 2");
	CHECK(led_channel_table[LED_IDX_SIDE_FUNCTION].pwm_channel == 0,
	      "regression: index 7 (SIDE_FUNCTION) -> PWM3 channel 0, not 3");

	/* gauge_step ascends bottom-to-top from SIDE_PLAY exactly like the
	 * table's own row order — "one verified physical bottom-to-top index
	 * map", not a second assumption. */
	for (uint8_t i = LED_TRACK_ROW_COUNT; i < LED_PHYSICAL_COUNT; i++) {
		CHECK(led_channel_table[i].gauge_step == i - LED_TRACK_ROW_COUNT,
		      "gauge_step for side index %u == %u (bottom-to-top order)",
		      i, i - LED_TRACK_ROW_COUNT);
	}
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
 * Battery / charging local baseline: the real SP-1 charging-gauge state
 * machine (led_battery.h), replacing the old arbitrary quintile model.
 * ------------------------------------------------------------------------ */
static void test_battery_unavailable_and_fault_are_never_low(void)
{
	led_battery_gauge_t g;

	led_battery_gauge_reset(&g);
	CHECK(led_battery_classify(&g, false, false) == LED_BATTERY_UNAVAILABLE,
	      "never-seeded gauge classifies UNAVAILABLE, not empty/low");
	CHECK(!led_battery_state_is_low(LED_BATTERY_UNAVAILABLE),
	      "UNAVAILABLE is never classified as low battery");

	/* A failed read after at least one valid sample -> FAULT, not LOW,
	 * and the sticky level/EMA from the prior valid sample survive. */
	led_battery_gauge_update(&g, true, 1000); /* valid, seeds level 1 (below thr1) */
	uint8_t level_before = g.level;

	led_battery_gauge_update(&g, false, 0); /* failed read: sticky */
	CHECK(g.level == level_before, "a failed read leaves the sticky level unchanged");
	CHECK(led_battery_classify(&g, false, false) == LED_BATTERY_FAULT,
	      "a failed read after a valid one classifies FAULT, not LOW");
	CHECK(!led_battery_state_is_low(LED_BATTERY_FAULT),
	      "FAULT is never classified as low battery");

	/* Charger-status fault: nCHG asserted without nPGOOD is contradictory. */
	CHECK(led_battery_classify(&g, false, true) == LED_BATTERY_FAULT,
	      "charging_now with charger_present == false is a charger-status FAULT");

	/* Neither UNAVAILABLE nor FAULT ever blocks host rendering: the ONLY
	 * state allowed to preempt an owned host frame is LED_BATTERY_LOW —
	 * "must never suppress a valid host LED frame". */
	CHECK(led_render_select(false, led_battery_state_is_low(LED_BATTERY_UNAVAILABLE), true) ==
		      LED_RENDER_SOURCE_HOST,
	      "UNAVAILABLE battery state never blocks an owned host frame");
	CHECK(led_render_select(false, led_battery_state_is_low(LED_BATTERY_FAULT), true) ==
		      LED_RENDER_SOURCE_HOST,
	      "FAULT battery state never blocks an owned host frame");
}

static void test_battery_charging_states_distinct(void)
{
	led_battery_gauge_t g;

	led_battery_gauge_reset(&g);
	led_battery_gauge_update(&g, true, 1000); /* well below THR_1: bottom quarter */

	CHECK(led_battery_classify(&g, false, false) == LED_BATTERY_LOW,
	      "charger absent + valid bottom-quarter reading classifies LOW");
	CHECK(led_battery_classify(&g, true, true) == LED_BATTERY_CHARGING,
	      "charger present + nCHG asserted -> CHARGING");
	CHECK(led_battery_classify(&g, true, false) == LED_BATTERY_CHARGE_COMPLETE,
	      "charger present + nCHG deasserted -> CHARGE_COMPLETE");
	CHECK(led_battery_classify(&g, false, false) != led_battery_classify(&g, true, true),
	      "CHARGER_ABSENT/LOW and CHARGING are distinct states");
	CHECK(led_battery_classify(&g, true, true) != led_battery_classify(&g, true, false),
	      "CHARGING and CHARGE_COMPLETE are distinct states");
	CHECK(!led_battery_state_is_low(LED_BATTERY_CHARGING) &&
	      !led_battery_state_is_low(LED_BATTERY_CHARGE_COMPLETE),
	      "neither CHARGING nor CHARGE_COMPLETE is ever classified as low battery");
}

static void test_battery_low_threshold_and_hysteresis(void)
{
	led_battery_gauge_t g;

	led_battery_gauge_reset(&g);
	/* Below THR_1: bottom quarter, charger absent -> LOW. */
	led_battery_gauge_update(&g, true, (int32_t)LED_BATTERY_THR_1 - 100);
	CHECK(g.level == 1, "a reading well below THR_1 seeds gauge level 1");
	CHECK(led_battery_classify(&g, false, false) == LED_BATTERY_LOW,
	      "charger absent, valid, bottom quarter -> LOW");
	CHECK(led_battery_state_is_low(led_battery_classify(&g, false, false)),
	      "LOW is the only state where led_battery_state_is_low() is true");

	/* Comfortably above THR_3: top quarter, charger absent -> not low. */
	led_battery_gauge_reset(&g);
	led_battery_gauge_update(&g, true, (int32_t)LED_BATTERY_THR_3 + 200);
	CHECK(g.level == 4, "a reading well above THR_3 seeds gauge level 4");
	CHECK(led_battery_classify(&g, false, false) == LED_BATTERY_CHARGER_ABSENT,
	      "charger absent, valid, top quarter -> CHARGER_ABSENT, not LOW");

	/* Hysteresis at the level 1->2 boundary, exercised directly against
	 * the EMA rather than through repeated smoothing (whose exact
	 * trajectory is an implementation detail): an EMA that has crossed
	 * THR_1 but not yet THR_1 + LED_BATTERY_HYSTERESIS_COUNTS must NOT
	 * bump the sticky level yet — "a single sample per pass with no
	 * hysteresis let ADC noise ... flip the level" [looper a8dd127:4622-4629]
	 * is exactly the flicker this guards against. */
	led_battery_gauge_reset(&g);
	g.ema = 2000;         /* just under THR_1 (2020), already established */
	g.level = 1;
	g.ever_valid = true;
	g.last_read_ok = true;
	led_battery_gauge_update(&g, true, 2200); /* ema -> 2000+(2200-2000)/8 = 2025: > THR_1, inside hysteresis */
	CHECK(g.level == 1,
	      "EMA just past THR_1 but inside the hysteresis band does not bump the level yet");
	led_battery_gauge_update(&g, true, 2300); /* ema -> 2025+(2300-2025)/8 = 2059: past THR_1+18 */
	CHECK(g.level == 2,
	      "EMA past THR_1 + LED_BATTERY_HYSTERESIS_COUNTS finally bumps the level");
}

static void test_battery_gauge_frame_distinct(void)
{
	led_battery_gauge_t g;
	uint8_t frame[LED_PHYSICAL_COUNT];

	led_battery_gauge_reset(&g);

	/* Never seeded: entire side row off, never fabricated. */
	led_battery_gauge_frame(&g, false, false, frame);
	for (uint8_t i = 0; i < LED_TRACK_ROW_COUNT; i++)
		CHECK(frame[i] == 0, "never-seeded gauge frame: Track LED %u off", i);
	for (uint8_t i = LED_TRACK_ROW_COUNT; i < LED_PHYSICAL_COUNT; i++)
		CHECK(frame[i] == 0, "never-seeded gauge frame: side LED %u off (not fabricated)", i);

	/* Full level (4), not charging: all four side LEDs solid. */
	led_battery_gauge_update(&g, true, (int32_t)LED_BATTERY_THR_3 + 300);
	led_battery_gauge_frame(&g, false, false, frame);
	for (uint8_t i = LED_TRACK_ROW_COUNT; i < LED_PHYSICAL_COUNT; i++)
		CHECK(frame[i] == LED_LEVEL_MAX, "full level, not charging: side LED %u solid", i);

	/* Partial level (2), not charging: below-level solid, at-level solid
	 * (not charging), above-level off — and this must differ from the
	 * charging-blink rendering of the exact same level. */
	led_battery_gauge_reset(&g);
	led_battery_gauge_update(&g, true, (int32_t)LED_BATTERY_THR_1 + 40); /* -> level 2 */
	CHECK(g.level == 2, "seeded at level 2 for the partial-frame checks");

	uint8_t not_charging_frame[LED_PHYSICAL_COUNT];
	uint8_t charging_blink_off_frame[LED_PHYSICAL_COUNT];
	uint8_t charging_blink_on_frame[LED_PHYSICAL_COUNT];

	led_battery_gauge_frame(&g, false, false, not_charging_frame);
	led_battery_gauge_frame(&g, true, false, charging_blink_off_frame);
	led_battery_gauge_frame(&g, true, true, charging_blink_on_frame);

	/* Step 0 (bottom, LED_IDX_SIDE_PLAY): strictly below level 2 -> solid
	 * in all three (never blinks). */
	CHECK(not_charging_frame[LED_IDX_SIDE_PLAY] == LED_LEVEL_MAX &&
	      charging_blink_off_frame[LED_IDX_SIDE_PLAY] == LED_LEVEL_MAX &&
	      charging_blink_on_frame[LED_IDX_SIDE_PLAY] == LED_LEVEL_MAX,
	      "the bottom (below-level) side LED is solid regardless of charging or blink phase");

	/* Step 1 (LED_IDX_SIDE_MID1) is the current/top level: solid when not
	 * charging, follows blink_phase when charging — "the next level
	 * blinking while charging, and all four solid when charging is
	 * complete" behavior, exercised directly. */
	CHECK(not_charging_frame[LED_IDX_SIDE_MID1] == LED_LEVEL_MAX,
	      "not charging: the current level's LED is solid");
	CHECK(charging_blink_off_frame[LED_IDX_SIDE_MID1] == 0,
	      "charging, blink phase off: the current level's LED is dark");
	CHECK(charging_blink_on_frame[LED_IDX_SIDE_MID1] == LED_LEVEL_MAX,
	      "charging, blink phase on: the current level's LED is lit");
	CHECK(memcmp(not_charging_frame, charging_blink_off_frame, LED_PHYSICAL_COUNT) != 0,
	      "charge-complete (not charging) and mid-blink-off frames are visibly distinct");

	/* Steps 2-3: strictly above level 2 -> off in all three. */
	CHECK(not_charging_frame[LED_IDX_SIDE_MID2] == 0 &&
	      not_charging_frame[LED_IDX_SIDE_FUNCTION] == 0,
	      "above-level side LEDs stay off");
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
 * led_render_policy.c: write/retry/fault-latch policy, driven through a
 * MOCKED physical write (led_channel_write_fn) that can be told to fail on
 * demand — this is the "mocked PWM-write coverage" the corrected renderer
 * requires, exercising the exact same pure module led_render.c binds to
 * pwm_set_pulse_dt() through.
 * ------------------------------------------------------------------------ */
typedef struct {
	bool fail[LED_PHYSICAL_COUNT];
	int  fail_rc;
	int  write_count[LED_PHYSICAL_COUNT];
	uint32_t last_pulse[LED_PHYSICAL_COUNT];
} mock_pwm_t;

static void mock_reset(mock_pwm_t *m)
{
	uint8_t i;

	for (i = 0; i < LED_PHYSICAL_COUNT; i++) {
		m->fail[i] = false;
		m->write_count[i] = 0;
		m->last_pulse[i] = 0xFFFFFFFFu;
	}
	m->fail_rc = -1;
}

static int mock_write(uint8_t index, uint32_t pulse_us, void *ctx)
{
	mock_pwm_t *m = (mock_pwm_t *)ctx;

	m->write_count[index]++;
	if (m->fail[index]) {
		return m->fail_rc;
	}
	m->last_pulse[index] = pulse_us;
	return 0;
}

static void test_render_policy_bringup(void)
{
	led_render_policy_t p;
	mock_pwm_t m;
	bool ok;
	uint8_t i;

	led_render_policy_init(&p);
	CHECK(!led_render_policy_is_ready(&p), "policy starts not ready before any bringup");

	mock_reset(&m);
	ok = led_render_policy_bringup(&p, mock_write, &m);
	CHECK(ok, "bringup with all 8 channels succeeding reports success");
	CHECK(led_render_policy_is_ready(&p), "is_ready reflects a fully successful bringup");
	for (i = 0; i < LED_PHYSICAL_COUNT; i++)
		CHECK(m.write_count[i] == 1, "bringup proves channel %u with exactly one write", i);
}

static void test_render_policy_bringup_partial_failure(void)
{
	led_render_policy_t p;
	mock_pwm_t m;
	bool ok;
	uint8_t idx;
	int rc;

	led_render_policy_init(&p);
	mock_reset(&m);
	m.fail[3] = true;
	m.fail_rc = -5;

	ok = led_render_policy_bringup(&p, mock_write, &m);
	CHECK(!ok, "bringup with one failing channel reports failure");
	CHECK(!led_render_policy_is_ready(&p), "is_ready stays false after a partial bringup failure");

	led_render_policy_last_failure(&p, &idx, &rc);
	CHECK(idx == 3 && rc == -5,
	      "the exact failing index (3) and return code (-5) are recorded for CDC");
}

static void test_render_policy_unnecessary_rewrite_suppressed(void)
{
	led_render_policy_t p;
	mock_pwm_t m;
	uint8_t levels[LED_PHYSICAL_COUNT] = { 0 };
	int rc;

	led_render_policy_init(&p);
	mock_reset(&m);
	led_render_policy_bringup(&p, mock_write, &m); /* all-zero proven */

	mock_reset(&m);
	levels[2] = 50;
	rc = led_render_policy_apply(&p, levels, mock_write, &m);
	CHECK(rc == 0, "apply with one changed channel fully succeeds");
	CHECK(m.write_count[2] == 1, "the changed channel is written");
	for (uint8_t i = 0; i < LED_PHYSICAL_COUNT; i++)
		if (i != 2)
			CHECK(m.write_count[i] == 0,
			      "unchanged channel %u is not unnecessarily rewritten", i);

	mock_reset(&m);
	rc = led_render_policy_apply(&p, levels, mock_write, &m);
	CHECK(rc == 0 && m.write_count[2] == 0,
	      "resending the identical frame writes nothing at all (already known-good)");
}

static void test_render_policy_failed_write_not_cached_and_retried(void)
{
	led_render_policy_t p;
	mock_pwm_t m;
	uint8_t levels[LED_PHYSICAL_COUNT] = { 0 };
	int rc, prev_count;

	led_render_policy_init(&p);
	mock_reset(&m);
	led_render_policy_bringup(&p, mock_write, &m);

	mock_reset(&m);
	m.fail[5] = true;
	m.fail_rc = -7;
	levels[5] = 90;
	rc = led_render_policy_apply(&p, levels, mock_write, &m);
	CHECK(rc == -1, "one failed channel: apply reports exactly one failure");
	CHECK(m.write_count[5] == 1, "the failing channel was attempted once");
	CHECK(!p.cache_valid[5],
	      "a failed write is NOT cached as successful (cache_valid cleared)");
	CHECK(p.dirty[5], "a failed write leaves the channel dirty for a deterministic retry");
	CHECK(led_render_policy_is_ready(&p),
	      "a single failure (below the consecutive-fail threshold) does not latch a fault");

	m.fail[5] = false;
	prev_count = m.write_count[5];
	rc = led_render_policy_apply(&p, levels, mock_write, &m);
	CHECK(rc == 0, "retrying after the transient failure clears now succeeds");
	CHECK(m.write_count[5] == prev_count + 1,
	      "the SAME requested level (90) is retried on the very next apply()");
	CHECK(p.cache_valid[5] && p.cached_level[5] == 90,
	      "a successful retry finally caches the level");
}

static void test_render_policy_fault_latch_and_safe_state(void)
{
	led_render_policy_t p;
	mock_pwm_t m;
	uint8_t levels[LED_PHYSICAL_COUNT] = { 0 };
	int rc;
	uint8_t k;

	led_render_policy_init(&p);
	mock_reset(&m);
	led_render_policy_bringup(&p, mock_write, &m);

	levels[1] = 10;
	m.fail[1] = true;
	m.fail_rc = -9;

	for (k = 0; (unsigned)(k + 1) < LED_RENDER_MAX_CONSECUTIVE_FAILS; k++) {
		rc = led_render_policy_apply(&p, levels, mock_write, &m);
		CHECK(rc == -1 && led_render_policy_is_ready(&p),
		      "still ready after %u consecutive failure(s) on one channel", (unsigned)(k + 1));
	}

	mock_reset(&m);
	m.fail[1] = true;
	m.fail_rc = -9;
	rc = led_render_policy_apply(&p, levels, mock_write, &m);
	CHECK(!led_render_policy_is_ready(&p),
	      "renderer latches not-ready after LED_RENDER_MAX_CONSECUTIVE_FAILS consecutive "
	      "failures on one channel");
	CHECK(rc < 0, "apply reports failure on the call that latches the fault");
	CHECK(!led_capability_should_answer(led_render_policy_is_ready(&p)),
	      "capability response is suppressed the instant the fault latches");

	/* Safe state: the fault-latching call also forces every channel to a
	 * best-effort 0us write ("put the outputs into the documented safe
	 * state"), not just the one that failed. */
	CHECK(m.write_count[0] >= 1 && m.write_count[7] >= 1,
	      "channels never touched by the failing frame still receive a forced safe-state write");
	CHECK(m.last_pulse[0] == 0u, "the forced safe-state write drives an untouched channel to 0us");

	/* Fully suppressed while faulted: no writes of any kind. */
	mock_reset(&m);
	rc = led_render_policy_apply(&p, levels, mock_write, &m);
	CHECK(rc == -1, "apply() while faulted returns immediately");
	for (uint8_t i = 0; i < LED_PHYSICAL_COUNT; i++)
		CHECK(m.write_count[i] == 0, "no physical write of any kind is issued while faulted (channel %u)", i);
}

static void test_render_policy_recovery_requires_all_eight(void)
{
	led_render_policy_t p;
	mock_pwm_t m;
	uint8_t levels[LED_PHYSICAL_COUNT] = { 0 };
	bool ok;
	uint8_t k;

	led_render_policy_init(&p);
	mock_reset(&m);
	led_render_policy_bringup(&p, mock_write, &m);

	/* Drive channel 4 to a latched fault. */
	levels[4] = 77;
	m.fail[4] = true;
	m.fail_rc = -2;
	for (k = 0; k < LED_RENDER_MAX_CONSECUTIVE_FAILS; k++) {
		mock_reset(&m);
		m.fail[4] = true;
		m.fail_rc = -2;
		(void)led_render_policy_apply(&p, levels, mock_write, &m);
	}
	CHECK(!led_render_policy_is_ready(&p), "channel 4 fault-latched the renderer as expected");

	/* Recovery attempt with one channel STILL bad: must stay not-ready. */
	mock_reset(&m);
	m.fail[2] = true;
	m.fail_rc = -3;
	ok = led_render_policy_bringup(&p, mock_write, &m);
	CHECK(!ok && !led_render_policy_is_ready(&p),
	      "a recovery attempt with one channel still failing stays not-ready");
	CHECK(!led_capability_should_answer(led_render_policy_is_ready(&p)),
	      "capability stays suppressed through a partial recovery attempt");

	/* Now every channel is healthy. */
	mock_reset(&m);
	ok = led_render_policy_bringup(&p, mock_write, &m);
	CHECK(ok && led_render_policy_is_ready(&p),
	      "recovery restores capability only once all eight channels are usable");
	CHECK(led_capability_should_answer(led_render_policy_is_ready(&p)),
	      "capability is answered again immediately after full recovery");
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
	test_battery_unavailable_and_fault_are_never_low();
	test_battery_charging_states_distinct();
	test_battery_low_threshold_and_hysteresis();
	test_battery_gauge_frame_distinct();
	test_render_precedence();
	test_capability_gate();
	test_render_policy_bringup();
	test_render_policy_bringup_partial_failure();
	test_render_policy_unnecessary_rewrite_suppressed();
	test_render_policy_failed_write_not_cached_and_retried();
	test_render_policy_fault_latch_and_safe_state();
	test_render_policy_recovery_requires_all_eight();
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
