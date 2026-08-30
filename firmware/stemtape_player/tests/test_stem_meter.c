/*
 * test_stem_meter.c — st_stem_meter.c: per-stem envelope + perceptual
 * brightness curve, host-tested.
 *
 * These drive the pure functions directly with explicit magnitudes and
 * explicit elapsed times. The magnitudes are not fabricated AUDIO -- this
 * module has no audio concept, only "a peak magnitude arrived" -- so this
 * is the standard way to test a pure envelope/curve, not the fixture
 * fabrication this suite's own non-fabrication rule forbids.
 *
 *     cc -std=c11 -Wall -Wextra -I../src \
 *        ../src/st_stem_meter.c test_stem_meter.c -o test_stem_meter
 */

#include <stdio.h>

#include "st_stem_meter.h"

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

#define FS ST_STEM_METER_FULL_SCALE

/* A freshly reset meter is fully dark -- a stem that has never produced a
 * sample must not light its LED at all. */
static void test_reset_is_dark(void)
{
	st_stem_meter_t m;

	st_stem_meter_reset(&m);
	CHECK(m.env == 0u, "reset zeroes the envelope");
	CHECK(st_stem_meter_brightness(&m) == 0u, "reset brightness is 0 (LED off)");
}

/* ATTACK IS INSTANT: the transient is the thing being displayed, so a
 * peak must land at full value on the very first update, not ramp. */
static void test_attack_is_instant(void)
{
	st_stem_meter_t m;

	st_stem_meter_reset(&m);
	st_stem_meter_update(&m, FS, 25u);
	CHECK(m.env == FS, "a full-scale peak reaches the envelope immediately");
	CHECK(st_stem_meter_brightness(&m) == 255u, "full scale is full brightness");
}

/* RELEASE IS PROPORTIONAL: the fall is a constant FRACTION per unit time,
 * so the decibel rate is constant from any starting level. At dt equal to
 * one tenth of the release constant the envelope must lose exactly one
 * tenth of its value. */
static void test_release_is_proportional(void)
{
	st_stem_meter_t m;
	uint32_t dt = ST_STEM_METER_RELEASE_MS / 10u;   /* 25 ms */

	st_stem_meter_reset(&m);
	st_stem_meter_update(&m, 1000000u, 0u);
	CHECK(m.env == 1000000u, "dt of 0 applies the peak with no decay");

	st_stem_meter_update(&m, 0u, dt);
	CHECK(m.env == 900000u, "one tenth of the release constant drops exactly 10%%");

	st_stem_meter_update(&m, 0u, dt);
	CHECK(m.env == 810000u, "the fall is proportional, not linear (810000, not 800000)");
}

/* A gap at least as long as the whole release constant must land exactly
 * on zero -- never a negative wrap, never a lingering remainder. */
static void test_long_gap_falls_to_zero(void)
{
	st_stem_meter_t m;

	st_stem_meter_reset(&m);
	st_stem_meter_update(&m, FS, 0u);
	st_stem_meter_update(&m, 0u, ST_STEM_METER_RELEASE_MS);
	CHECK(m.env == 0u, "a gap of one full release constant reaches exactly 0");

	st_stem_meter_update(&m, FS, 0u);
	st_stem_meter_update(&m, 0u, ST_STEM_METER_RELEASE_MS * 100u);
	CHECK(m.env == 0u, "a very long gap stays at 0 (no unsigned wrap)");
}

/* THE STUCK-LIGHT REGRESSION: a proportional decay never mathematically
 * reaches zero, so without a floor a light would stay faintly lit forever
 * after any hit. Everything at or below the floor must read as 0. */
static void test_floor_turns_the_led_fully_off(void)
{
	st_stem_meter_t m;

	st_stem_meter_reset(&m);
	m.env = ST_STEM_METER_FLOOR;
	CHECK(st_stem_meter_brightness(&m) == 0u, "exactly at the floor reads as off");

	m.env = ST_STEM_METER_FLOOR - 1u;
	CHECK(st_stem_meter_brightness(&m) == 0u, "below the floor reads as off");

	m.env = 1u;
	CHECK(st_stem_meter_brightness(&m) == 0u, "a single-LSB tail reads as off");

	m.env = ST_STEM_METER_FLOOR * 4u;
	CHECK(st_stem_meter_brightness(&m) > 0u, "two octaves above the floor is visibly lit");
}

/* A MUTED / SOLO-SILENCED STEM: main.c publishes peak 0 for it. Given
 * repeated zero peaks the light must actually reach off, not asymptote. */
static void test_silent_stem_reaches_off(void)
{
	st_stem_meter_t m;
	int i;

	st_stem_meter_reset(&m);
	st_stem_meter_update(&m, FS, 0u);
	for (i = 0; i < 200; i++) {
		st_stem_meter_update(&m, 0u, 25u);
	}
	CHECK(st_stem_meter_brightness(&m) == 0u,
	      "a stem silenced from full scale is fully dark within 5 s");
}

/* THE UNIFORMITY REGRESSION, stated as a curve property: real programme
 * material lives in the top ~20 dB, and a LINEAR map renders all of it
 * near-full -- which is what made the first implementation look uniform.
 * The log curve must place a 10x amplitude drop far down the visible range.
 *
 * THE BANDS HERE WERE WIDENED, DELIBERATELY. They used to require -40 dBFS
 * to sit "near half", which followed from mapping the whole ~66 dB between
 * the noise floor and full scale onto 255 steps. That is 0.26 dB per step,
 * and on hardware it made the row read as almost static: a drum hit decaying
 * by a very audible 18 dB moved its light by a quarter of its range. The
 * display window is now the SPAN octaves below the sensitivity reference
 * (see st_stem_meter.h), which trades reach at the very quiet end for
 * contrast where the music actually is. -40 dBFS now reads at the minimum
 * visible step rather than at half, and that is the intended behaviour, not
 * a regression -- it is the tuning knob to reach for if quiet passages turn
 * out to matter more than punch on real material. */
static void test_curve_spreads_real_material(void)
{
	st_stem_meter_t m;
	uint8_t b_full, b_tenth, b_hundredth;

	st_stem_meter_reset(&m);
	m.env = FS;
	b_full = st_stem_meter_brightness(&m);
	m.env = FS / 10u;
	b_tenth = st_stem_meter_brightness(&m);
	m.env = FS / 100u;
	b_hundredth = st_stem_meter_brightness(&m);

	CHECK(b_full == 255u, "full scale is 255");
	CHECK(b_tenth < 200u && b_tenth > 100u,
	      "-20 dB sits well down the range (got %u), not pinned at the top",
	      b_tenth);
	CHECK(b_full > b_tenth && b_tenth >= b_hundredth,
	      "the curve is monotonic");
	CHECK(b_hundredth >= ST_STEM_METER_MIN_ON,
	      "-40 dB is quiet, but it is not silence: it must not read below "
	      "the minimum visible step (got %u)", b_hundredth);
}

/*
 * THE DISPLAY WINDOW, walked octave by octave.
 *
 * Inside the window every octave must be strictly dimmer than the one above
 * it -- that is what makes four stems at different levels look different.
 * Below the window the curve flattens onto the minimum visible step, which is
 * deliberate: material that far down is present but not worth spending
 * brightness range on, and it must read as "quiet" rather than as "off".
 *
 * The count of distinct steps is checked against SPAN_OCTAVES rather than a
 * fixed number, so retuning the span by eye moves this expectation with it
 * instead of breaking it.
 */
static void test_octaves_are_evenly_spaced(void)
{
	st_stem_meter_t m;
	/* 256, not 255: seeded above the maximum the curve can return, so the
	 * FIRST octave (full scale, which legitimately reads 255) is compared
	 * against something it can actually be below. */
	unsigned prev = 256u;
	uint32_t e;
	int steps = 0, distinct = 0;

	st_stem_meter_reset(&m);
	for (e = FS; e > ST_STEM_METER_FLOOR * 2u; e /= 2u) {
		uint8_t b;

		m.env = e;
		b = st_stem_meter_brightness(&m);
		CHECK((unsigned)b <= prev,
		      "octave %d is not brighter than the one above it (%u <= %u)",
		      steps, (unsigned)b, prev);
		if ((unsigned)b < prev) {
			distinct++;
		}
		CHECK(b >= ST_STEM_METER_MIN_ON,
		      "octave %d reads %u, below the minimum visible step -- "
		      "audible material must never be dimmer than that",
		      steps, (unsigned)b);
		prev = b;
		steps++;
	}
	CHECK(steps >= 10, "the sweep must cover the whole range (got %d)", steps);
	CHECK(distinct >= (int)ST_STEM_METER_SPAN_OCTAVES,
	      "only %d distinct octave levels across a %u-octave display "
	      "window", distinct, ST_STEM_METER_SPAN_OCTAVES);
	CHECK(distinct <= (int)ST_STEM_METER_SPAN_OCTAVES + 2,
	      "%d distinct levels for a %u-octave window: the window is wider "
	      "than it is declared to be", distinct,
	      ST_STEM_METER_SPAN_OCTAVES);
}

/* Defensive: a caller-supplied peak above full scale (impossible from the
 * real 24-bit decoder, but this module must not depend on that) clamps
 * rather than distorting the curve. */
static void test_over_full_scale_clamps(void)
{
	st_stem_meter_t m;

	st_stem_meter_reset(&m);
	st_stem_meter_update(&m, 0xFFFFFFFFu, 0u);
	CHECK(m.env == FS, "a peak above full scale clamps to full scale");
	CHECK(st_stem_meter_brightness(&m) == 255u, "and reads as full brightness");
}

int main(void)
{
	RUN(test_reset_is_dark);
	RUN(test_attack_is_instant);
	RUN(test_release_is_proportional);
	RUN(test_long_gap_falls_to_zero);
	RUN(test_floor_turns_the_led_fully_off);
	RUN(test_silent_stem_reaches_off);
	RUN(test_curve_spreads_real_material);
	RUN(test_octaves_are_evenly_spaced);
	RUN(test_over_full_scale_clamps);

	printf("\n%d distinct test cases, %d assertion checks\n", g_test_cases, g_checks);
	if (g_failures) {
		printf("STEM METER TEST FAILED (%d/%d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("STEM METER TEST PASSED (%d test cases, %d checks)\n", g_test_cases, g_checks);
	return 0;
}
