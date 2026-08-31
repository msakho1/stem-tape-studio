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
	CHECK(m.fast == 0u && m.body == 0u, "reset zeroes both envelopes");
	CHECK(st_stem_meter_brightness(&m) == 0u, "reset brightness is 0 (LED off)");
}

/*
 * THE TWO ENVELOPES, and the lag between them.
 *
 * FAST rises instantly -- the transient is the thing being displayed. BODY
 * deliberately does NOT: its lagging attack is the entire mechanism by which
 * an accent exists, because the accent is the gap between them. A body that
 * rose instantly would sit on top of fast at every hit, the gap would always
 * be zero, and the display would collapse to the single flat envelope this
 * replaced.
 *
 * Also note what full-scale BODY alone reads: NOT 255. The body is capped at
 * its share of the range so that sustained material sits in the middle with
 * headroom above it. Full brightness is reachable only with an accent on top,
 * which is the point -- a light already at maximum has nowhere to animate.
 */
static void test_fast_is_instant_body_lags(void)
{
	st_stem_meter_t m;
	const uint32_t usable = ST_STEM_METER_MAX - ST_STEM_METER_MIN_ON;
	const uint32_t body_cap = ST_STEM_METER_MIN_ON +
		(usable * ST_STEM_METER_BODY_SHARE_Q8) / 256u;
	uint8_t at_attack, settled;

	st_stem_meter_reset(&m);
	st_stem_meter_update(&m, FS, 10u);
	CHECK(m.fast == FS, "a full-scale peak reaches FAST immediately");
	CHECK(m.body < FS / 2u,
	      "BODY must still be far behind after 10 ms (got %u of %u) -- its "
	      "lag is what opens the accent", m.body, (unsigned)FS);

	at_attack = st_stem_meter_brightness(&m);
	/*
	 * A hit into a dark stem is a big jump but NOT the maximum, and that
	 * ordering is deliberate rather than an accident of the arithmetic.
	 * The brightest thing the display can show is a stem that is BOTH
	 * loud and accenting -- more total energy than one isolated hit in
	 * silence. test_maximum_is_reachable() below pins the top end.
	 */
	CHECK(at_attack > (ST_STEM_METER_MAX * 2u) / 3u,
	      "a full-scale hit against a dark body must read high (got %u of "
	      "%u)", at_attack, ST_STEM_METER_MAX);
	CHECK(at_attack > body_cap,
	      "and above the body cap (%u): the accent is what put it there",
	      body_cap);

	/* Hold the same level until the body has caught up. */
	{
		int i;

		for (i = 0; i < 200; i++) {
			st_stem_meter_update(&m, FS, 10u);
		}
	}
	settled = st_stem_meter_brightness(&m);
	CHECK(settled == (uint8_t)body_cap,
	      "held at full scale, the light settles to the BODY CAP (%u), not "
	      "to maximum (got %u) -- the top of the range belongs to accents",
	      body_cap, settled);
	CHECK(settled < ST_STEM_METER_MAX,
	      "and that cap really is below maximum, or there is no headroom "
	      "for an accent to use");
}

/*
 * THE TOP OF THE RANGE MUST BE REACHABLE.
 *
 * Splitting the range between a capped body and an accent invites a display
 * whose maximum no real signal can ever produce -- brightness the hardware
 * has but the music never uses. The combination that must reach it is the
 * musically brightest one: a stem already sounding at the reference level
 * that then accents on top of that.
 */
static void test_maximum_is_reachable(void)
{
	st_stem_meter_t m;
	uint8_t b;

	st_stem_meter_reset(&m);
	/* Body settled at the sensitivity reference, fast a full accent span
	 * above it -- a loud passage with a strong hit in it. */
	m.body = ST_STEM_METER_REF;
	m.fast = ST_STEM_METER_REF << ST_STEM_METER_ACCENT_SPAN_OCTAVES;
	if (m.fast > ST_STEM_METER_FULL_SCALE) {
		m.fast = ST_STEM_METER_FULL_SCALE;
	}
	b = st_stem_meter_brightness(&m);
	/*
	 * WITHIN A FEW STEPS OF THE TOP, not exactly at it, and the reason is
	 * arithmetic rather than tuning. A full accent needs FAST to sit a
	 * whole ACCENT_SPAN above a BODY that is itself already at REF -- that
	 * is REF << ACCENT_SPAN, which with the default -6 dBFS reference is
	 * 2^23, one LSB above the largest magnitude a 24-bit sample can hold.
	 * So the last handful of brightness steps are unreachable by exactly
	 * that one LSB.
	 *
	 * The thing worth guarding is that the display uses essentially all of
	 * its range, which it does; chasing the final step would mean moving
	 * the sensitivity for a difference no eye can see.
	 */
	CHECK(b >= ST_STEM_METER_MAX - 8u,
	      "a loud stem accenting on top of itself must reach within a few "
	      "steps of full brightness (got %u of %u)", b, ST_STEM_METER_MAX);
}

/* RELEASE IS PROPORTIONAL: the fall is a constant FRACTION per unit time, so
 * the decibel rate is constant from any starting level. At dt equal to one
 * tenth of the release constant the envelope must lose exactly one tenth of
 * its value.
 *
 * Seeded directly rather than through update(): with a lagging body attack,
 * feeding a peak in no longer places it in the body, and this case is about
 * the FALL. */
static void test_release_is_proportional(void)
{
	st_stem_meter_t m;
	uint32_t dt = ST_STEM_METER_BODY_RELEASE_MS / 10u;

	st_stem_meter_reset(&m);
	m.body = 1000000u;
	m.fast = 1000000u;

	st_stem_meter_update(&m, 0u, dt);
	CHECK(m.body == 900000u,
	      "one tenth of the body release drops exactly 10%% (got %u)",
	      m.body);

	st_stem_meter_update(&m, 0u, dt);
	CHECK(m.body == 810000u,
	      "the fall is proportional, not linear (810000, not 800000; got %u)",
	      m.body);
}

/* A gap at least as long as the whole release constant must land exactly
 * on zero -- never a negative wrap, never a lingering remainder. */
static void test_long_gap_falls_to_zero(void)
{
	st_stem_meter_t m;

	st_stem_meter_reset(&m);
	m.fast = FS;
	m.body = FS;
	st_stem_meter_update(&m, 0u, ST_STEM_METER_BODY_RELEASE_MS);
	CHECK(m.body == 0u && m.fast == 0u,
	      "a gap of one full body release reaches exactly 0");

	m.fast = FS;
	m.body = FS;
	st_stem_meter_update(&m, 0u, ST_STEM_METER_BODY_RELEASE_MS * 100u);
	CHECK(m.body == 0u && m.fast == 0u,
	      "a very long gap stays at 0 (no unsigned wrap)");
}

/* THE STUCK-LIGHT REGRESSION: a proportional decay never mathematically
 * reaches zero, so without a floor a light would stay faintly lit forever
 * after any hit. Everything at or below the floor must read as 0. */
static void test_floor_turns_the_led_fully_off(void)
{
	st_stem_meter_t m;

	st_stem_meter_reset(&m);
	m.fast = ST_STEM_METER_FLOOR;
	m.body = m.fast;
	CHECK(st_stem_meter_brightness(&m) == 0u, "exactly at the floor reads as off");

	m.fast = ST_STEM_METER_FLOOR - 1u;
	m.body = m.fast;
	CHECK(st_stem_meter_brightness(&m) == 0u, "below the floor reads as off");

	m.fast = 1u;
	m.body = m.fast;
	CHECK(st_stem_meter_brightness(&m) == 0u, "a single-LSB tail reads as off");

	m.fast = ST_STEM_METER_FLOOR * 4u;
	m.body = m.fast;
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
	m.fast = FS;
	m.body = m.fast;
	b_full = st_stem_meter_brightness(&m);
	m.fast = FS / 10u;
	m.body = m.fast;
	b_tenth = st_stem_meter_brightness(&m);
	m.fast = FS / 100u;
	m.body = m.fast;
	b_hundredth = st_stem_meter_brightness(&m);

	/* Bands expressed against the BODY CAP rather than against 255, so
	 * that retuning BODY_SHARE_Q8 by eye moves them with it instead of
	 * breaking them. These are body-only readings: fast == body here, so
	 * there is no accent and the top of the range is legitimately out of
	 * reach (see test_maximum_is_reachable). */
	{
		const uint32_t usable = ST_STEM_METER_MAX - ST_STEM_METER_MIN_ON;
		const uint32_t cap = ST_STEM_METER_MIN_ON +
			(usable * ST_STEM_METER_BODY_SHARE_Q8) / 256u;
		const uint32_t glow = cap - ST_STEM_METER_MIN_ON;

		CHECK(b_full == (uint8_t)cap,
		      "full scale fills the body's own range (%u), not the "
		      "whole scale (got %u)", cap, b_full);
		CHECK(b_tenth > ST_STEM_METER_MIN_ON + glow / 5u &&
		      b_tenth < ST_STEM_METER_MIN_ON + (glow * 4u) / 5u,
		      "-20 dB sits well down the body range (got %u, band %u..%u)",
		      b_tenth, ST_STEM_METER_MIN_ON + glow / 5u,
		      ST_STEM_METER_MIN_ON + (glow * 4u) / 5u);
	}
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

		m.fast = e;
	m.body = m.fast;
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
	CHECK(distinct >= (int)ST_STEM_METER_BODY_SPAN_OCTAVES,
	      "only %d distinct octave levels across a %u-octave display "
	      "window", distinct, ST_STEM_METER_BODY_SPAN_OCTAVES);
	CHECK(distinct <= (int)ST_STEM_METER_BODY_SPAN_OCTAVES + 2,
	      "%d distinct levels for a %u-octave window: the window is wider "
	      "than it is declared to be", distinct,
	      ST_STEM_METER_BODY_SPAN_OCTAVES);
}

/* Defensive: a caller-supplied peak above full scale (impossible from the
 * real 24-bit decoder, but this module must not depend on that) clamps
 * rather than distorting the curve. */
static void test_over_full_scale_clamps(void)
{
	st_stem_meter_t m;

	st_stem_meter_reset(&m);
	st_stem_meter_update(&m, 0xFFFFFFFFu, 0u);
	CHECK(m.fast == FS && m.body <= FS,
	      "a peak above full scale clamps to full scale");
	{
		const uint32_t usable = ST_STEM_METER_MAX - ST_STEM_METER_MIN_ON;
		const uint32_t cap = ST_STEM_METER_MIN_ON +
			(usable * ST_STEM_METER_BODY_SHARE_Q8) / 256u;

		CHECK(st_stem_meter_brightness(&m) >= (uint8_t)cap,
		      "and reads at least the full body level");
	}
}

int main(void)
{
	RUN(test_reset_is_dark);
	RUN(test_fast_is_instant_body_lags);
	RUN(test_maximum_is_reachable);
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
