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

#include "st_planar.h"
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

/*
 * THE DOMAIN CHECK, and the reason it is in this file rather than in a planar
 * test: every other case here is written against ST_STEM_METER_FULL_SCALE,
 * ST_STEM_METER_REF and ST_STEM_METER_FLOOR, so it scales with whatever those
 * constants happen to say. That is right for testing the CURVE and blind to
 * the thing that actually broke -- the constants describing a different domain
 * from the one the PRODUCER feeds them.
 *
 * stem_render_run() meters exactly what st_pl_decode_stem_inline() returned,
 * so this decodes a real full-scale stored frame through that real function
 * and asserts the magnitude lands on full scale here. If the stored width ever
 * moves again and these constants do not, this is the case that says so.
 */
static void test_meter_domain_matches_the_decoder(void)
{
	uint8_t group[ST_PL_GROUP_BYTES] = { 0 };
	int32_t l, r;

	/*
	 * Positive full scale for a STORED sample, written as bytes rather than
	 * derived from any meter constant -- 0x7F in the most significant byte
	 * and 0xFF below it, little-endian, which is 0x7FFF at v1.3's two bytes
	 * per sample and 0x7FFFFF at v1.2's three. Building the pattern out of
	 * ST_STEM_METER_FULL_SCALE would make the whole case circular: the
	 * wrong constant would produce the bytes that decode back to itself.
	 */
	{
		uint32_t i;
		const uint32_t off = st_pl_frame_off(0u);

		for (i = 0u; i < ST11_BYTES_PER_SAMPLE; i++) {
			const uint8_t b = (i == ST11_BYTES_PER_SAMPLE - 1u) ?
					  0x7Fu : 0xFFu;

			group[off + i] = b;
			group[off + ST11_BYTES_PER_SAMPLE + i] = b;
		}
	}

	st_pl_decode_stem_inline(group, 0u, &l, &r);

	CHECK(l == (int32_t)ST_STEM_METER_FULL_SCALE,
	      "a full-scale stored sample decodes to %d, the meter's full scale",
	      (int)ST_STEM_METER_FULL_SCALE);
	CHECK(r == l, "both channels land in the same domain");

	/* And the window the curve draws in has to be INSIDE that domain. The
	 * v1.3 regression was exactly this: REF sat 256x above the largest
	 * magnitude a stored sample could reach, so the body range was
	 * unreachable and the row never rose above MIN_ON. */
	CHECK(ST_STEM_METER_REF <= ST_STEM_METER_FULL_SCALE,
	      "the body reference is reachable by a real sample");
	CHECK(ST_STEM_METER_FLOOR > 0u &&
	      ST_STEM_METER_FLOOR < ST_STEM_METER_REF,
	      "the noise floor is above zero and below the reference");

	/* The end-to-end consequence, stated as behaviour rather than as
	 * arithmetic: sustained decoded full-scale material must light the LED
	 * to at least the full BODY level.
	 *
	 * The body specifically, and settled -- not a single dt=0 update. A
	 * lone peak with the body still at zero lights the row through the
	 * ACCENT path, which is relative (fast far above body) and so fires at
	 * any magnitude, in any domain. That is precisely the check that would
	 * have passed straight through the v1.3 regression. Holding the peak
	 * for a second of service passes puts body ON the peak and asks the
	 * question that actually distinguishes the two domains: is a real
	 * full-scale sample inside the body window at all?
	 */
	{
		st_stem_meter_t m;
		const uint32_t usable = ST_STEM_METER_MAX - ST_STEM_METER_MIN_ON;
		const uint32_t body_range =
			(usable * ST_STEM_METER_BODY_SHARE_Q8) / 256u;
		int pass;

		st_stem_meter_reset(&m);
		/* 300 passes is 3 s at the real service rate, comfortably past
		 * the 120 ms body attack settling exactly onto the peak. */
		for (pass = 0; pass < 300; pass++) {
			st_stem_meter_update(&m, (uint32_t)l, 10u);
		}
		CHECK(m.body == (uint32_t)l,
		      "a held full-scale peak settles the body onto it");
		CHECK(st_stem_meter_brightness(&m) >=
		      (uint8_t)(ST_STEM_METER_MIN_ON + body_range),
		      "and sustained full scale drives the LED to the full body level");
	}
}

/*
 * A PROPORTIONAL DECAY THAT STALLS ABOVE THE FLOOR NEVER GOES DARK.
 *
 * span * dt / tc is integer division, so a small remaining span rounds to a
 * zero step. At 16 bits the floor is 8 and the stall region is span < 30, so
 * this is reachable by real material -- the LED would rest faintly lit after
 * the stem stopped. Drive an envelope from just inside that region and require
 * that it actually reaches zero in bounded time.
 */
static void test_small_magnitudes_still_reach_the_floor(void)
{
	st_stem_meter_t m;
	int pass;

	st_stem_meter_reset(&m);
	m.fast = ST_STEM_METER_FLOOR + 20u;
	m.body = ST_STEM_METER_FLOOR + 20u;

	CHECK(st_stem_meter_brightness(&m) > 0u,
	      "an envelope just above the floor starts lit");

	/* 10 ms a pass is the real LED service rate; 400 passes is 4 s, far
	 * longer than the 300 ms release needs and still a bound. */
	for (pass = 0; pass < 400; pass++) {
		st_stem_meter_update(&m, 0u, 10u);
		if (m.fast <= ST_STEM_METER_FLOOR && m.body <= ST_STEM_METER_FLOOR) {
			break;
		}
	}

	CHECK(pass < 400, "it reaches the floor rather than stalling above it");
	CHECK(st_stem_meter_brightness(&m) == 0u, "and the LED is fully off");

	/* The step never crosses the target: decaying toward a NON-zero level
	 * must settle ON it, not undershoot and creep back. */
	st_stem_meter_reset(&m);
	m.fast = ST_STEM_METER_FLOOR + 20u;
	for (pass = 0; pass < 400; pass++) {
		st_stem_meter_update(&m, ST_STEM_METER_FLOOR + 5u, 10u);
	}
	CHECK(m.fast == ST_STEM_METER_FLOOR + 5u,
	      "and a decay toward a held level settles exactly on it");
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
	RUN(test_meter_domain_matches_the_decoder);
	RUN(test_small_magnitudes_still_reach_the_floor);

	printf("\n%d distinct test cases, %d assertion checks\n", g_test_cases, g_checks);
	if (g_failures) {
		printf("STEM METER TEST FAILED (%d/%d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("STEM METER TEST PASSED (%d test cases, %d checks)\n", g_test_cases, g_checks);
	return 0;
}
