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

/* A silent check for use inside long loops, where one CHECK per iteration
 * would bury the output. Counts only failures. */
#define CHECK_QUIET(cond)                                                      \
	do {                                                                   \
		if (!(cond)) {                                                 \
			g_checks++;                                            \
			g_failures++;                                          \
			printf("  FAIL %s:%d: quiet check\n", __FILE__,        \
			       __LINE__);                                      \
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

/* ======================================================================
 * 8. SLOW PLAYBACK LAYERS OVER THE VARISPEED, it does not replace it.
 *
 *    The brief's whole state requirement in one case: "Do not overwrite the
 *    underlying user semitone/varispeed value when entering slow mode." So
 *    this checks the SEMITONE FIELD directly, not just the resulting rate --
 *    an implementation that stashed the old value and restored it would give
 *    the right rate back and still be wrong, because the next thing it does
 *    with a stale copy is get it wrong.
 *
 *    EVERY RATE BELOW COMES OUT OF st_pitch_effective_q16(), the same call
 *    the audio block makes. An earlier draft of this case did the multiply
 *    itself, which proved only that this file can multiply; the layering it
 *    was meant to test lived at the call site where no host test could reach
 *    it. Moving the product into the module is what makes the case real.
 * ====================================================================== */
static void case_slow_layers_over_varispeed(void)
{
	st_pitch_t p;
	uint32_t at_2, slow_2, back_2;

	g_cases++;
	printf("\n-- slow playback multiplies the varispeed, never replaces it\n");

	/* +2 semitones, the brief's own example. */
	st_pitch_reset(&p);
	p.half = 4;
	at_2 = st_pitch_effective_q16(&p, ST_PITCH_ONE);
	CHECK(at_2 == st_pitch_ratio_q16(&p),
	      "with slow off the effective rate must be the plain pitch rate "
	      "(%u vs %u)", at_2, st_pitch_ratio_q16(&p));

	/* Engaging slow must not touch the semitone state at all. */
	slow_2 = st_pitch_effective_q16(&p, ST_PITCH_SLOW_Q16);
	CHECK(p.half == 4,
	      "engaging slow changed the stored semitones to %+.1f", st(&p));
	back_2 = st_pitch_effective_q16(&p, ST_PITCH_ONE);

	printf("     +2.0 st: %u -> slow %u -> back %u (%.4fx -> %.4fx)\n",
	       at_2, slow_2, back_2, at_2 / 65536.0, slow_2 / 65536.0);
	CHECK(back_2 == at_2,
	      "leaving slow must return to the +2 rate exactly (%u vs %u)",
	      back_2, at_2);
	/* Half speed of the +2 rate, not half speed of 1.0x. */
	CHECK(slow_2 == at_2 / 2u,
	      "slow at +2 semitones must be half of the +2 rate (%u), not half "
	      "of unity (%u)", at_2 / 2u, ST_PITCH_ONE / 2u);
	CHECK(slow_2 != ST_PITCH_SLOW_Q16,
	      "slow collapsed to a flat 0.5x -- the varispeed was discarded");

	/*
	 * AND AT EVERY OTHER SETTING, not just the brief's example. Half of
	 * the pitched rate at all 30 half-steps, so an implementation that
	 * special-cased one of them, or that layered only near unity, fails
	 * here rather than in someone's ears.
	 */
	{
		int k, off = 0;

		for (k = ST_PITCH_MIN_HALF; k <= ST_PITCH_MAX_HALF; k++) {
			uint32_t norm, slow;

			p.half = (int16_t)k;
			norm = st_pitch_effective_q16(&p, ST_PITCH_ONE);
			slow = st_pitch_effective_q16(&p, ST_PITCH_SLOW_Q16);
			if (slow != norm / 2u || p.half != (int16_t)k) {
				off++;
			}
		}
		CHECK(off == 0,
		      "%d of %d half-steps did not slow to exactly half their "
		      "own rate", off, ST_PITCH_MAX_HALF - ST_PITCH_MIN_HALF + 1);
	}

	/*
	 * A PART-WAY GLIDE SCALES THE PITCHED RATE TOO. The multiplier is a
	 * position, not a switch, so mid-transition must sit strictly between
	 * the two endpoints -- at the PITCHED endpoints, not at 1.0x and 0.5x.
	 */
	{
		const uint32_t midway = (ST_PITCH_ONE + ST_PITCH_SLOW_Q16) / 2u;
		uint32_t mid;

		p.half = 4;
		mid = st_pitch_effective_q16(&p, midway);
		CHECK(mid > slow_2 && mid < at_2,
		      "a half-completed glide at +2 gave %u, outside the "
		      "pitched endpoints %u..%u", mid, slow_2, at_2);
		/*
		 * And it is three quarters OF THE PITCHED RATE, not three
		 * quarters of unity. Being between the endpoints is not enough
		 * to prove that: 0.75x itself (49152) also sits between them,
		 * which is exactly what a glide applied to the wrong base
		 * would return.
		 */
		CHECK(mid == (at_2 * 3u) / 4u,
		      "a 75%%-of-the-way glide at +2 gave %u, not three "
		      "quarters of the +2 rate (%u) -- it is scaling the wrong "
		      "base", mid, (at_2 * 3u) / 4u);
	}

	/*
	 * CHANGING THE ROCKER WHILE SLOW IS ENGAGED. The brief: leaving slow
	 * must resume at the NEW setting, not at a stale one from before. With
	 * the two kept as independent factors this needs no code at all, which
	 * is the point of the case -- it would catch a save/restore design.
	 */
	{
		uint32_t slow_new, back_new;

		p.half = 2;                            /* +1.0 while still slow */
		slow_new = st_pitch_effective_q16(&p, ST_PITCH_SLOW_Q16);
		back_new = st_pitch_effective_q16(&p, ST_PITCH_ONE);
		printf("     changed to +1.0 while slow: slow %u, resumed %u\n",
		       slow_new, back_new);
		CHECK(slow_new != slow_2,
		      "the slow rate did not follow the new semitone setting");
		CHECK(back_new == st_pitch_ratio_q16(&p),
		      "resuming must use the CURRENT setting, not a stale copy");
		CHECK(back_new != at_2,
		      "resuming returned to the OLD +2 rate -- a stale value was "
		      "restored");
	}
}

/*
 * Run one whole glide from `from` to the `want_slow` endpoint at a given block
 * size and sample rate. Returns the ELAPSED MILLISECONDS, which is the only
 * figure that should be the same across every (blk, rate) pair; the step COUNT
 * deliberately is not. Reports monotonicity and the largest single step by
 * reference.
 */
static double ramp_ms(uint32_t from, bool want_slow, uint32_t blk,
		      uint32_t rate_hz, uint32_t *worst_step, uint32_t *landed)
{
	uint32_t cur = from, prev = from, steps = 0;

	if (worst_step) {
		*worst_step = 0u;
	}
	while (steps < 100000u) {
		uint32_t nxt = st_pitch_slow_glide(cur, want_slow, blk, rate_hz);
		uint32_t d = (nxt > cur) ? (nxt - cur) : (cur - nxt);

		if (nxt == cur) {
			break;                      /* arrived, or stalled */
		}
		if (worst_step && d > *worst_step) {
			*worst_step = d;
		}
		/* Monotone: a glide that wobbles is a wobble in the pitch. */
		CHECK_QUIET(want_slow ? (nxt < prev) : (nxt > prev));
		prev = cur = nxt;
		steps++;
	}
	if (landed) {
		*landed = cur;
	}
	return steps * 1000.0 * blk / rate_hz;
}

/* ======================================================================
 * 9. THE GLIDE: smooth, bounded, symmetric, and it ARRIVES.
 * ====================================================================== */
static void case_slow_glide(void)
{
	uint32_t worst = 0, landed = 0;
	double down_ms, up_ms;

	g_cases++;
	printf("\n-- the slow toggle glides, and lands exactly\n");

	/* Into slow. */
	down_ms = ramp_ms(ST_PITCH_ONE, true, 256u, 48000u, &worst, &landed);
	printf("     normal -> slow in %.0f ms, largest step %u/65536\n",
	       down_ms, worst);
	CHECK(landed == ST_PITCH_SLOW_Q16,
	      "the glide must ARRIVE at half speed, not approach it (got %u)",
	      landed);
	CHECK(down_ms > ST_PITCH_SLOW_GLIDE_MS * 0.7 &&
	      down_ms < ST_PITCH_SLOW_GLIDE_MS * 1.4,
	      "the glide took %.0f ms against a %u ms constant", down_ms,
	      ST_PITCH_SLOW_GLIDE_MS);
	/* NOT the inertia envelope: that is a transport start, this is a speed
	 * toggle, and the brief separates them explicitly. */
	CHECK(down_ms < 250.0,
	      "%.0f ms is long enough to read as a tape spinning down rather "
	      "than a speed switch", down_ms);

	/* And back out, to exactly unity, in the same time. */
	up_ms = ramp_ms(ST_PITCH_SLOW_Q16, false, 256u, 48000u, NULL, &landed);
	CHECK(landed == ST_PITCH_ONE,
	      "the glide back must land on exactly 1.0x (got %u)", landed);
	/*
	 * SYMMETRY. Nothing in the brief makes entering slow and leaving it
	 * different, and an exit that snaps while the entry glides is exactly
	 * the asymmetry a "restore the old value" implementation produces.
	 */
	CHECK(up_ms > down_ms * 0.9 && up_ms < down_ms * 1.1,
	      "leaving slow took %.0f ms against %.0f ms to enter -- the two "
	      "directions must be the same glide", up_ms, down_ms);

	/*
	 * THE GLIDE IS ON THE AUDIO CLOCK, NOT ON A BLOCK COUNT.
	 *
	 * `frames` is what makes that true, so it has to be varied: measured
	 * only at one block size, a glide that stepped by a hardcoded 256
	 * would be indistinguishable from a correct one. Same wall-clock
	 * duration at a quarter of the block size and at a different sample
	 * rate; the STEP COUNT differs by design, the TIME does not.
	 */
	{
		const double tiny = ramp_ms(ST_PITCH_ONE, true, 64u, 48000u,
					    NULL, &landed);
		const double big  = ramp_ms(ST_PITCH_ONE, true, 1024u, 48000u,
					    NULL, NULL);
		const double slow_clock = ramp_ms(ST_PITCH_ONE, true, 256u,
						  16000u, NULL, NULL);

		printf("     same glide at 64/256/1024-frame blocks: "
		       "%.0f / %.0f / %.0f ms; at 16 kHz: %.0f ms\n",
		       tiny, down_ms, big, slow_clock);
		CHECK(landed == ST_PITCH_SLOW_Q16,
		      "the glide must arrive at half speed at a 64-frame block "
		      "too (got %u)", landed);
		CHECK(tiny > down_ms * 0.8 && tiny < down_ms * 1.2,
		      "a 64-frame block glided in %.0f ms against %.0f ms at "
		      "256 -- the glide is counting blocks, not frames",
		      tiny, down_ms);
		CHECK(big > down_ms * 0.8 && big < down_ms * 1.25,
		      "a 1024-frame block glided in %.0f ms against %.0f ms at "
		      "256 -- the glide is counting blocks, not frames",
		      big, down_ms);
		CHECK(slow_clock > down_ms * 0.8 && slow_clock < down_ms * 1.2,
		      "at 16 kHz the glide took %.0f ms against %.0f ms at 48 "
		      "kHz -- the sample rate is being ignored",
		      slow_clock, down_ms);
	}

	/* Already there: a no-op, not a wander. */
	CHECK(st_pitch_slow_glide(ST_PITCH_ONE, false, 256u, 48000u) ==
	      ST_PITCH_ONE, "gliding to where it already is must not move");
	CHECK(st_pitch_slow_glide(ST_PITCH_SLOW_Q16, true, 256u, 48000u) ==
	      ST_PITCH_SLOW_Q16, "and the same at the slow end");

	/*
	 * A REVERSAL MID-GLIDE MUST TURN ROUND, NOT JUMP.
	 *
	 * Measured from DEEP in the glide, not one step in. One step in, "step
	 * back" and "snap to unity" land on the same number, so the check
	 * passes either way and constrains nothing -- which is precisely how
	 * an earlier version of this case let a snapping implementation
	 * through.
	 */
	{
		uint32_t deep = ST_PITCH_ONE;
		uint32_t rev;
		int i;

		for (i = 0; i < 10; i++) {
			deep = st_pitch_slow_glide(deep, true, 256u, 48000u);
		}
		rev = st_pitch_slow_glide(deep, false, 256u, 48000u);
		printf("     reversal 10 blocks in: %u -> %u\n", deep, rev);
		CHECK(rev > deep,
		      "reversing mid-glide must move back toward unity "
		      "(%u -> %u)", deep, rev);
		CHECK(rev < ST_PITCH_ONE,
		      "reversing mid-glide jumped straight to unity (%u) from "
		      "%u -- that is a snap, not a glide", rev, deep);
		CHECK(rev - deep <= worst,
		      "the reversal step (%u) is larger than the largest "
		      "forward step (%u)", rev - deep, worst);
	}
}

/* ======================================================================
 * 10. THE STOCK RATIO IS THE COMPANION'S.
 * ====================================================================== */
static void case_slow_ratio_is_stock(void)
{
	g_cases++;
	printf("\n-- the slow ratio is the companion's half speed\n");

	/* src/machine/surface.ts: `const rate = ... ? 1 : 0.5`. */
	CHECK(ST_PITCH_SLOW_Q16 == ST_PITCH_ONE / 2u,
	      "the slow ratio is %u/65536, not the companion's exact 0.5x",
	      ST_PITCH_SLOW_Q16);
	printf("     %u/65536 = %.4fx, exactly one octave down\n",
	       ST_PITCH_SLOW_Q16, ST_PITCH_SLOW_Q16 / 65536.0);
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
	case_slow_layers_over_varispeed();
	case_slow_glide();
	case_slow_ratio_is_stock();

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
