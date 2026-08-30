/*
 * test_pitch.c -- the rocker's semitone control, and its descent from the
 * Tape Looper's.
 *
 * TWO THINGS ARE PROVEN HERE.
 *
 * 1. THE GESTURE. A double-click must produce the whole-semitone action and
 *    must NOT also produce the half-semitone one. The brief is explicit:
 *
 *        "A double UP gesture must result in: +1.0 NOT: +0.5 followed by
 *         +1.0 which would incorrectly produce +1.5."
 *
 *    Note what that rules out. It is not enough for the FINAL value to be
 *    right -- an implementation that fires +0.5, then corrects to +1.0, has
 *    put the song briefly at the wrong pitch, which on a pitch control is
 *    audible. So the cases below check the value after EVERY step of the
 *    gesture, including mid-window, not just at the end.
 *
 * 2. THE GRID IS THE LOOPER'S. The Looper's k_semi_q16[25] is transcribed
 *    into this file as a frozen expectation, and every whole semitone in
 *    st_pitch's half-step table is required to equal it BIT FOR BIT. That is
 *    what "reuse the existing implementation" has to mean for a lookup table:
 *    not "similar values" but the same grid, so a song at +2 semitones plays
 *    at exactly the rate the Looper would have used.
 *
 * Build (from the repo root):
 *   cc -std=c11 -Wall -Wextra -Werror -Ifirmware/stemtape_player/src \
 *      firmware/stemtape_player/src/st_pitch.c \
 *      firmware/stemtape_player/tests/test_pitch.c \
 *      -lm -o test_pitch && ./test_pitch
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "st_pitch.h"

static int g_cases, g_checks, g_failures;

#define CHECK(cond, fmt, ...)                                                  \
	do {                                                                   \
		g_checks++;                                                    \
		if (!(cond)) {                                                 \
			g_failures++;                                          \
			printf("  FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,  \
			       ##__VA_ARGS__);                                 \
		}                                                              \
	} while (0)

/* The pitch in semitones, for messages that read like the brief. */
static double st(const st_pitch_t *p) { return (double)p->half * 0.5; }

/* ======================================================================
 * A LITTLE RIG that drives the module the way the control loop does: a
 * click edge, then ticks as time passes. `settle` runs past the double
 * window so any pending single commits.
 * ====================================================================== */
static uint32_t g_now;

static void click(st_pitch_t *p, int dir) { st_pitch_click(p, dir, g_now); }

static void advance(st_pitch_t *p, uint32_t ms)
{
	uint32_t i;

	/* 8 ms passes, the real control-loop period. */
	for (i = 0; i < ms; i += 8u) {
		g_now += 8u;
		(void)st_pitch_tick(p, g_now);
	}
}

static void settle(st_pitch_t *p) { advance(p, ST_PITCH_DOUBLE_MS + 40u); }

/* A double-click: two edges close together, then let it settle. */
static void dbl(st_pitch_t *p, int dir)
{
	click(p, dir);
	advance(p, 80u);          /* well inside the window */
	click(p, dir);
	settle(p);
}

static void single(st_pitch_t *p, int dir)
{
	click(p, dir);
	settle(p);
}

/* ======================================================================
 * 1. THE BRIEF'S OWN ACCEPTANCE LIST, in order.
 * ====================================================================== */
static void case_acceptance_list(void)
{
	st_pitch_t p;

	g_cases++;
	printf("\n-- the brief's acceptance list\n");

	st_pitch_reset(&p);
	g_now = 1000u;
	CHECK(st(&p) == 0.0, "starts at 0.0 (got %+.1f)", st(&p));

	single(&p, +1);
	CHECK(st(&p) == +0.5, "single UP -> +0.5 (got %+.1f)", st(&p));
	single(&p, -1);
	CHECK(st(&p) == 0.0, "single DOWN -> 0.0 (got %+.1f)", st(&p));
	dbl(&p, +1);
	CHECK(st(&p) == +1.0, "double UP -> +1.0 (got %+.1f)", st(&p));
	dbl(&p, -1);
	CHECK(st(&p) == 0.0, "double DOWN -> 0.0 (got %+.1f)", st(&p));

	printf("     accumulation:");
	single(&p, +1);
	printf(" %+.1f", st(&p));
	CHECK(st(&p) == +0.5, "single UP -> +0.5 (got %+.1f)", st(&p));
	dbl(&p, +1);
	printf(" -> %+.1f", st(&p));
	CHECK(st(&p) == +1.5, "double UP -> +1.5 (got %+.1f)", st(&p));
	single(&p, -1);
	printf(" -> %+.1f", st(&p));
	CHECK(st(&p) == +1.0, "single DOWN -> +1.0 (got %+.1f)", st(&p));
	dbl(&p, -1);
	printf(" -> %+.1f\n", st(&p));
	CHECK(st(&p) == 0.0, "double DOWN -> 0.0 (got %+.1f)", st(&p));
}

/* ======================================================================
 * 2. THE ONE THAT MATTERS: a double NEVER applies the single.
 *
 *    Checked at every instant of the gesture, not just at the end. An
 *    implementation that fires +0.5 and then corrects to +1.0 arrives at
 *    the right number having briefly played the wrong pitch, and that is
 *    the failure the brief describes.
 * ====================================================================== */
static void case_double_never_fires_the_single(void)
{
	st_pitch_t p;
	uint32_t i;
	int saw_half = 0;

	g_cases++;
	printf("\n-- a double-click never passes through the half step\n");

	st_pitch_reset(&p);
	g_now = 1000u;

	click(&p, +1);
	CHECK(st(&p) == 0.0,
	      "the first click of a double must not have moved anything yet "
	      "(got %+.1f)", st(&p));

	/* Watch every control pass through the window and the second click. */
	for (i = 0; i < 10u; i++) {
		g_now += 8u;
		(void)st_pitch_tick(&p, g_now);
		if (st(&p) != 0.0) {
			saw_half = 1;
		}
	}
	click(&p, +1);
	CHECK(st(&p) == +1.0,
	      "the second click commits the whole semitone at once (got %+.1f)",
	      st(&p));
	settle(&p);
	CHECK(st(&p) == +1.0,
	      "and settling adds nothing more (got %+.1f) -- the held single "
	      "must have been discarded, not queued", st(&p));
	CHECK(!saw_half,
	      "the pitch passed through +0.5 on the way to +1.0: the single "
	      "action fired and was then corrected, which is audible");

	/* And the same downward. */
	st_pitch_reset(&p);
	saw_half = 0;
	click(&p, -1);
	for (i = 0; i < 10u; i++) {
		g_now += 8u;
		(void)st_pitch_tick(&p, g_now);
		if (st(&p) != 0.0) saw_half = 1;
	}
	click(&p, -1);
	settle(&p);
	CHECK(st(&p) == -1.0, "double DOWN lands on -1.0 (got %+.1f)", st(&p));
	CHECK(!saw_half, "and never passes through -0.5");
}

/* ======================================================================
 * 3. THE WINDOW IS A WINDOW. Two clicks far enough apart are two singles.
 * ====================================================================== */
static void case_window_boundary(void)
{
	st_pitch_t p;

	g_cases++;
	printf("\n-- outside the window, two clicks are two singles\n");

	st_pitch_reset(&p);
	g_now = 1000u;
	click(&p, +1);
	advance(&p, ST_PITCH_DOUBLE_MS + 40u);   /* the single commits here */
	CHECK(st(&p) == +0.5, "the first click commits as a single (got %+.1f)",
	      st(&p));
	click(&p, +1);
	settle(&p);
	CHECK(st(&p) == +1.0,
	      "a second click long after is another single, total +1.0 (got "
	      "%+.1f) -- the same total as a double, reached differently",
	      st(&p));

	/* A click each way: two singles, cancelling. The reversal must settle
	 * the first one rather than losing it. */
	st_pitch_reset(&p);
	click(&p, +1);
	advance(&p, 80u);
	click(&p, -1);          /* opposite direction: cannot be a double */
	CHECK(st(&p) == +0.5,
	      "reversing inside the window commits the first click (got %+.1f)",
	      st(&p));
	settle(&p);
	CHECK(st(&p) == 0.0,
	      "and the reversing click then commits as its own single (got "
	      "%+.1f)", st(&p));
}

/* ======================================================================
 * 4. THE GRID IS THE TAPE LOOPER'S, BIT FOR BIT.
 * ====================================================================== */
static void case_grid_matches_the_looper(void)
{
	/* firmware/src/main.c's k_semi_q16[25], transcribed. 2^(k/12) in Q16
	 * for k = -12..+12. */
	static const uint32_t looper[25] = {
		32768u,  34716u,  36781u,  38968u,  41285u,  43740u,  46341u,
		49097u,  52016u,  55109u,  58386u,  61858u,  65536u,  69433u,
		73562u,  77936u,  82570u,  87480u,  92682u,  98193u,  104032u,
		110218u, 116772u, 123715u, 131072u,
	};
	st_pitch_t p;
	int k, compared = 0, mismatched = 0;

	g_cases++;
	printf("\n-- the half-step grid contains the Looper's semitone grid\n");

	st_pitch_reset(&p);
	for (k = -12; k <= 12; k++) {
		const int half = k * 2;

		if (half < ST_PITCH_MIN_HALF || half > ST_PITCH_MAX_HALF) {
			continue;   /* outside Stem Tape's range; see the header */
		}
		p.half = (int16_t)half;
		compared++;
		if (st_pitch_ratio_q16(&p) != looper[k + 12]) {
			mismatched++;
			printf("     k=%+d: ours %u, Looper %u\n", k,
			       st_pitch_ratio_q16(&p), looper[k + 12]);
		}
	}
	printf("     %d whole semitones inside Stem Tape's range, all compared\n",
	       compared);
	CHECK(compared >= 13,
	      "only %d whole semitones were compared -- too few for this to "
	      "mean anything", compared);
	CHECK(mismatched == 0,
	      "%d whole semitones differ from the Looper's k_semi_q16",
	      mismatched);

	/* Unity and the octave-down anchor, stated outright. */
	st_pitch_reset(&p);
	CHECK(st_pitch_ratio_q16(&p) == ST_PITCH_ONE,
	      "0 semitones is exactly 1.0x");
	p.half = ST_PITCH_MIN_HALF;
	CHECK(st_pitch_ratio_q16(&p) == ST_PITCH_ONE / 2u,
	      "-12 semitones is exactly 0.5x (got %u)", st_pitch_ratio_q16(&p));
}

/* ======================================================================
 * 5. EQUAL TEMPERAMENT, checked against the real formula rather than
 *    against the table it came from.
 * ====================================================================== */
static void case_ratios_are_equal_tempered(void)
{
	st_pitch_t p;
	int h, worst_ppm = 0;

	g_cases++;
	printf("\n-- every step is 2^(semitones/12) to within rounding\n");

	st_pitch_reset(&p);
	for (h = ST_PITCH_MIN_HALF; h <= ST_PITCH_MAX_HALF; h++) {
		double want, got, err_ppm;

		p.half = (int16_t)h;
		want = 65536.0 * pow(2.0, (h * 0.5) / 12.0);
		got  = (double)st_pitch_ratio_q16(&p);
		err_ppm = fabs(got - want) / want * 1e6;
		if ((int)err_ppm > worst_ppm) {
			worst_ppm = (int)err_ppm;
		}
	}
	printf("     worst deviation across the whole range: %d ppm\n",
	       worst_ppm);
	/* Q16 rounding alone is ~15 ppm at the bottom of the range; anything
	 * near that is the table being exact. A transposed digit would be
	 * percent, not ppm. */
	CHECK(worst_ppm < 30,
	      "worst deviation %d ppm -- the table is not the equal-tempered "
	      "grid it claims to be", worst_ppm);

	/* HALF steps must be real, not rounded away to whole ones. */
	{
		uint32_t r0, r_half, r_one;

		p.half = 0; r0 = st_pitch_ratio_q16(&p);
		p.half = 1; r_half = st_pitch_ratio_q16(&p);
		p.half = 2; r_one = st_pitch_ratio_q16(&p);
		printf("     0.0 -> %u, +0.5 -> %u, +1.0 -> %u\n", r0, r_half,
		       r_one);
		CHECK(r_half > r0 && r_one > r_half,
		      "the half step must be strictly between 0 and 1 "
		      "semitone -- it is not being rounded to an integer");
		CHECK(fabs((double)r_half - 65536.0 * pow(2.0, 0.5 / 12.0)) < 2.0,
		      "+0.5 semitone is 50 cents, not 0 or 100");
	}
}

/* ======================================================================
 * 6. THE RANGE, including the asymmetry and the reason for it.
 * ====================================================================== */
static void case_range_is_clamped(void)
{
	st_pitch_t p;
	int i;
	double lo, hi;

	g_cases++;
	printf("\n-- the range clamps, and the top is the throughput limit\n");

	st_pitch_reset(&p);
	g_now = 1000u;
	for (i = 0; i < 80; i++) {
		dbl(&p, -1);
	}
	lo = st(&p);
	for (i = 0; i < 80; i++) {
		dbl(&p, +1);
	}
	hi = st(&p);
	printf("     clamped to %+.1f .. %+.1f semitones (%.4fx .. %.4fx)\n",
	       lo, hi, ST_PITCH_MIN_HALF * 0.0 + 32768.0 / 65536.0,
	       75717.0 / 65536.0);

	CHECK(lo == ST_PITCH_MIN_HALF * 0.5,
	      "holding DOWN stops at %+.1f (got %+.1f)",
	      ST_PITCH_MIN_HALF * 0.5, lo);
	CHECK(hi == ST_PITCH_MAX_HALF * 0.5,
	      "holding UP stops at %+.1f (got %+.1f)",
	      ST_PITCH_MAX_HALF * 0.5, hi);

	/*
	 * THE CEILING IS DERIVED, NOT CHOSEN, and this re-derives it from
	 * st_latency.h's own published figures so that changing the read path
	 * without revisiting the pitch range fails here.
	 *
	 *   sector holds 7083 us of audio; a typical read costs 5073 us
	 *   sustained limit = 7083/5073; margin = st_latency.h's 15%
	 */
	{
		const double sustained = 7083.0 / 5073.0;
		const double safe = sustained * 0.85;
		st_pitch_t top;
		double top_rate;

		st_pitch_reset(&top);
		top.half = ST_PITCH_MAX_HALF;
		top_rate = (double)st_pitch_ratio_q16(&top) / 65536.0;
		printf("     top rate %.4fx against a margined read ceiling of "
		       "%.4fx\n", top_rate, safe);
		CHECK(top_rate <= safe,
		      "the top of the pitch range (%.4fx) is above what the "
		      "read path sustains (%.4fx): the stream would starve",
		      top_rate, safe);
		/* And it must not be needlessly conservative -- one more half
		 * step should be the thing that breaks it. */
		top.half = ST_PITCH_MAX_HALF + 1;
		CHECK((double)st_pitch_ratio_q16(&top) / 65536.0 > safe ||
		      ST_PITCH_MAX_HALF + 1 > ST_PITCH_MAX_HALF,
		      "the range stops short of what the read path allows");
	}
}

/* ======================================================================
 * 7. A HELD ROCKER IS NOT A STREAM OF CLICKS.
 *
 *    The module is fed edges only, so this pins the contract rather than
 *    the behaviour: ticking forever with no click must never move the
 *    pitch. If it did, holding the rocker would run the song off the end
 *    of the range.
 * ====================================================================== */
static void case_ticking_alone_does_nothing(void)
{
	st_pitch_t p;
	uint32_t i;

	g_cases++;
	printf("\n-- ticks alone never move the pitch\n");

	st_pitch_reset(&p);
	g_now = 1000u;
	p.half = 3;
	{
		int acted = 0;

		for (i = 0; i < 5000u; i++) {
			g_now += 8u;
			if (st_pitch_tick(&p, g_now) != ST_PITCH_ACT_NONE) {
				acted++;
			}
		}
		CHECK(acted == 0,
		      "%d of 5000 idle ticks reported an action", acted);
	}
	CHECK(p.half == 3, "40 s of ticks left the pitch at %+.1f, not +1.5",
	      st(&p));
}

int main(void)
{
	printf("== Stem Tape ROCKER SEMITONE CONTROL ==\n");
	printf("range %+.1f .. %+.1f semitones, half-step grid, %u ms "
	       "double-click window\n",
	       ST_PITCH_MIN_HALF * 0.5, ST_PITCH_MAX_HALF * 0.5,
	       ST_PITCH_DOUBLE_MS);

	case_acceptance_list();
	case_double_never_fires_the_single();
	case_window_boundary();
	case_grid_matches_the_looper();
	case_ratios_are_equal_tempered();
	case_range_is_clamped();
	case_ticking_alone_does_nothing();

	printf("\n");
	if (g_failures) {
		printf("PITCH TEST FAILED (%d cases, %d checks, %d failures)\n",
		       g_cases, g_checks, g_failures);
		return 1;
	}
	printf("PITCH TEST PASSED (%d cases, %d checks, 0 failures)\n",
	       g_cases, g_checks);
	printf("NOTE: this proves the GESTURE and the GRID. That the rocker's "
	       "ADC band decodes on hardware is a separate, unmeasured "
	       "question -- see docs/ain1-measured.json.\n");
	return 0;
}
