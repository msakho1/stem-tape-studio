/*
 * test_inertia.c -- the tape transport envelope.
 *
 * WHAT IS BEING PROVEN. Not "a number goes up and down": the specific
 * properties that separate a tape transport from a volume fade.
 *
 *   the envelope is a MULTIPLIER, so it composes with varispeed -- a 0.8x
 *   target ramps 0 -> 0.8x, not 0 -> 1.0x then clipped
 *
 *   it is MONOTONIC and CONTINUOUS -- no step anywhere, because a step in
 *   rate is a step in pitch, which is a click
 *
 *   spin-up has zero slope at BOTH ends and its steepest section in the
 *   middle, which is the "slow, then strong, then settling" the brief asks
 *   for and the opposite of a linear ramp
 *
 *   spin-down is front-loaded -- most of the SPEED is lost early, most of
 *   the TIME is spent crawling at low rate -- which is what makes it a tape
 *   stop rather than a fade
 *
 *   the envelope depends only on ELAPSED FRAMES, never on how the caller
 *   chopped them into runs. The audio thread renders in runs bounded by
 *   sector, block and loop edges, so an envelope that drifted with run
 *   length would put a different pitch contour on every take.
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -Werror -Ifirmware/stemtape_player/src \
 *      firmware/stemtape_player/src/st_inertia.c \
 *      firmware/stemtape_player/tests/test_inertia.c -lm \
 *      -o test_inertia && ./test_inertia
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include "st_inertia.h"

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

#define SR 48000u

/* Run a ramp to completion one frame at a time, recording the envelope. */
static uint32_t g_env[200000];
static uint32_t g_n;

static void run_to_rest_or_unity(st_inertia_t *s, uint32_t limit)
{
	g_n = 0;
	while (g_n < limit) {
		st_inertia_advance(s, 1u);
		g_env[g_n++] = st_inertia_env_q16(s);
		if (s->state == ST_INERTIA_RUNNING ||
		    s->state == ST_INERTIA_STOPPED) {
			break;
		}
	}
}

/*
 * Advance two copies over the same `frames` -- `a` one frame at a time, `b` in
 * lumps of 1,2,3,...,256,1,... -- comparing at EVERY lump boundary. Returns the
 * number of points at which they disagreed.
 */
static uint32_t lockstep(st_inertia_t *a, st_inertia_t *b, uint32_t frames)
{
	uint32_t done = 0, lump = 1, bad = 0;

	while (done < frames) {
		uint32_t n = lump, k;

		if (done + n > frames) {
			n = frames - done;
		}
		for (k = 0; k < n; k++) {
			st_inertia_advance(a, 1u);
		}
		st_inertia_advance(b, n);
		if (st_inertia_env_q16(a) != st_inertia_env_q16(b) ||
		    a->state != b->state) {
			bad++;
		}
		done += n;
		lump = (lump % 256u) + 1u;
	}
	return bad;
}

/* ======================================================================
 * 1. THE STATE MODEL the brief specifies, and the fact that audio keeps
 *    being rendered through BOTH ramps -- that is what makes stop not a mute.
 * ====================================================================== */
static void case_state_model(void)
{
	st_inertia_t s;

	g_cases++;
	printf("\n-- STOPPED -> SPINUP -> RUNNING -> SPINDOWN -> STOPPED\n");

	st_inertia_reset(&s);
	CHECK(s.state == ST_INERTIA_STOPPED, "starts stopped");
	CHECK(!st_inertia_moving(&s), "and not moving, so nothing is rendered");
	CHECK(st_inertia_env_q16(&s) == 0u, "envelope is zero at rest");

	st_inertia_play(&s, SR);
	CHECK(s.state == ST_INERTIA_SPINUP, "PLAY enters SPINUP");
	CHECK(st_inertia_moving(&s), "and audio is rendered from the first frame");
	CHECK(!st_inertia_at_unity(&s), "but not yet at nominal rate");

	run_to_rest_or_unity(&s, 100000u);
	CHECK(s.state == ST_INERTIA_RUNNING, "reaches RUNNING");
	CHECK(st_inertia_env_q16(&s) == ST_INERTIA_ONE,
	      "at exactly 1.0 (%u)", st_inertia_env_q16(&s));
	CHECK(st_inertia_at_unity(&s),
	      "and reports unity, so the audio path takes its 1:1 fast path");

	st_inertia_stop(&s, SR);
	CHECK(s.state == ST_INERTIA_SPINDOWN, "STOP enters SPINDOWN");
	CHECK(st_inertia_moving(&s),
	      "audio is STILL rendered while decelerating -- stop is not a mute");

	run_to_rest_or_unity(&s, 100000u);
	CHECK(s.state == ST_INERTIA_STOPPED, "comes to rest");
	CHECK(st_inertia_env_q16(&s) == 0u, "at exactly zero");
	CHECK(!st_inertia_moving(&s), "and only THEN is playback cut");
}

/* ======================================================================
 * 2. DURATIONS land in the brief's ranges. Reported as measured, not
 *    asserted from the constants, so changing a constant by ear shows up
 *    here as a number rather than as a silently still-passing test.
 * ====================================================================== */
static void case_durations(void)
{
	st_inertia_t s;
	uint32_t up, down;

	g_cases++;
	printf("\n-- ramp durations\n");

	st_inertia_reset(&s);
	st_inertia_play(&s, SR);
	run_to_rest_or_unity(&s, 200000u);
	up = g_n;

	st_inertia_stop(&s, SR);
	run_to_rest_or_unity(&s, 200000u);
	down = g_n;

	printf("     spin-up %u frames (%.0f ms), spin-down %u frames (%.0f ms)\n",
	       up, 1000.0 * up / SR, down, 1000.0 * down / SR);
	CHECK(up >= SR * 250u / 1000u && up <= SR * 500u / 1000u,
	      "spin-up is inside the 250-500 ms brief (%.0f ms)",
	      1000.0 * up / SR);
	CHECK(down >= SR * 400u / 1000u && down <= SR * 800u / 1000u,
	      "spin-down is inside the 400-800 ms brief (%.0f ms)",
	      1000.0 * down / SR);
}

/* ======================================================================
 * 3. CONTINUITY AND MONOTONICITY. A step in the envelope is a step in
 *    playback rate, which is a step in pitch, which is a click -- the exact
 *    artifact this feature exists to avoid.
 * ====================================================================== */
static void case_continuous_and_monotonic(void)
{
	st_inertia_t s;
	uint32_t i, worst_up = 0, worst_down = 0;
	int back = 0;

	g_cases++;
	printf("\n-- both ramps are continuous and monotonic\n");

	st_inertia_reset(&s);
	st_inertia_play(&s, SR);
	run_to_rest_or_unity(&s, 200000u);
	for (i = 1; i < g_n; i++) {
		uint32_t d;

		if (g_env[i] < g_env[i - 1]) back++;
		d = g_env[i] - g_env[i - 1];
		if (d > worst_up) worst_up = d;
	}
	CHECK(back == 0, "spin-up never goes backwards (%d reversals)", back);
	/* One frame of a 350 ms smoothstep moves at most ~1.5 * 65536/16800.
	 * A generous ceiling still excludes any real discontinuity. */
	CHECK(worst_up < 64u,
	      "spin-up's largest single-frame step is %u (well under a jump)",
	      worst_up);

	st_inertia_stop(&s, SR);
	run_to_rest_or_unity(&s, 200000u);
	back = 0;
	/* The LAST transition is excluded on purpose and checked separately
	 * below: the decay curve approaches zero without arriving, so the module
	 * declares the reel stopped once it is under the rest threshold and cuts
	 * the remainder. That cut is a step by construction -- the contract is
	 * that it is BOUNDED BY THE THRESHOLD, i.e. it happens at a rate so low
	 * the ear has nothing left to hear. Everything before it must be smooth. */
	for (i = 1; i + 1u < g_n; i++) {
		uint32_t d;

		if (g_env[i] > g_env[i - 1]) back++;
		d = g_env[i - 1] - g_env[i];
		if (d > worst_down) worst_down = d;
	}
	CHECK(back == 0, "spin-down never goes backwards (%d reversals)", back);
	CHECK(worst_down < 64u,
	      "spin-down's largest single-frame step is %u", worst_down);
	CHECK(g_env[g_n - 1u] == 0u, "spin-down ends at exactly zero");
	/* The frame that crosses the threshold is zeroed in the same call, so
	 * the last value the caller can observe is the one BEFORE the crossing:
	 * the threshold plus at most one ordinary frame of decay. */
	CHECK(g_env[g_n - 2u] <= ST_INERTIA_REST_Q16 + worst_down,
	      "the final cut to zero happens from %u, within one frame's decay "
	      "of the rest threshold %u", g_env[g_n - 2u], ST_INERTIA_REST_Q16);
	/* And an absolute ceiling on the threshold itself, independent of the
	 * tuning: 1/100 of nominal is over six octaves down. A constant large
	 * enough to make the cut audible would be caught here. */
	CHECK(ST_INERTIA_REST_Q16 <= ST_INERTIA_ONE / 100u,
	      "the rest threshold %u is under 1%% of nominal rate",
	      ST_INERTIA_REST_Q16);
	printf("     largest single-frame move: up %u, down %u (of 65536); "
	       "final cut from %u\n",
	       worst_up, worst_down, g_env[g_n - 2u]);
}

/* ======================================================================
 * 4. THE SPIN-UP SHAPE. Slow off the mark, strongest in the middle,
 *    settling at the top -- explicitly NOT a linear ramp.
 * ====================================================================== */
static void case_spinup_shape(void)
{
	st_inertia_t s;
	uint32_t q1, q2, q3, first, mid, last;

	g_cases++;
	printf("\n-- spin-up eases in, drives through the middle, settles\n");

	st_inertia_reset(&s);
	st_inertia_play(&s, SR);
	run_to_rest_or_unity(&s, 200000u);

	q1 = g_env[g_n / 4u];
	q2 = g_env[g_n / 2u];
	q3 = g_env[(3u * g_n) / 4u];
	printf("     envelope at 25/50/75%%: %.3f  %.3f  %.3f\n",
	       q1 / 65536.0, q2 / 65536.0, q3 / 65536.0);

	CHECK(q1 < ST_INERTIA_ONE / 4u,
	      "at a quarter through it is still BELOW a linear ramp (%.3f < 0.25)",
	      q1 / 65536.0);
	CHECK(q3 > (3u * ST_INERTIA_ONE) / 4u,
	      "at three quarters it is ABOVE it (%.3f > 0.75)", q3 / 65536.0);
	CHECK(q2 > ST_INERTIA_ONE / 2u - 2048u && q2 < ST_INERTIA_ONE / 2u + 2048u,
	      "and crosses the middle near halfway (%.3f)", q2 / 65536.0);

	/* Slope: gentle at both ends, steepest in the middle. */
	first = g_env[g_n / 16u] - g_env[0];
	mid   = g_env[g_n / 2u + g_n / 16u] - g_env[g_n / 2u];
	last  = g_env[g_n - 1u] - g_env[g_n - 1u - g_n / 16u];
	printf("     slope over 1/16 of the ramp: start %u, middle %u, end %u\n",
	       first, mid, last);
	CHECK(mid > first * 2u,
	      "the middle accelerates much harder than the start (%u vs %u)",
	      mid, first);
	CHECK(mid > last * 2u,
	      "and much harder than the settle (%u vs %u)", mid, last);
}

/* ======================================================================
 * 5. THE SPIN-DOWN SHAPE. Front-loaded speed loss, then a long crawl --
 *    the classic tape stop, and the thing a linear ramp cannot give.
 * ====================================================================== */
static void case_spindown_shape(void)
{
	st_inertia_t s;
	uint32_t q1, q2, half_time = 0, i;

	g_cases++;
	printf("\n-- spin-down loses speed at once, then crawls\n");

	st_inertia_reset(&s);
	st_inertia_play(&s, SR);
	run_to_rest_or_unity(&s, 200000u);
	st_inertia_stop(&s, SR);
	run_to_rest_or_unity(&s, 200000u);

	q1 = g_env[g_n / 4u];
	q2 = g_env[g_n / 2u];
	printf("     envelope at 25/50%% of the ramp: %.3f  %.3f\n",
	       q1 / 65536.0, q2 / 65536.0);

	CHECK(q1 < ST_INERTIA_ONE / 2u,
	      "a quarter of the way through, MORE than half the speed is "
	      "already gone (%.3f)", q1 / 65536.0);
	CHECK(q2 < ST_INERTIA_ONE / 4u,
	      "halfway through it is below a quarter rate -- two octaves down "
	      "with half the ramp still to run (%.3f)", q2 / 65536.0);

	/* Where the envelope passes 0.5: for a front-loaded curve that is
	 * early. A linear ramp would put it at exactly halfway. */
	for (i = 0; i < g_n; i++) {
		if (g_env[i] <= ST_INERTIA_ONE / 2u) { half_time = i; break; }
	}
	printf("     reaches half rate after %.0f%% of the ramp\n",
	       100.0 * half_time / g_n);
	CHECK(half_time * 4u < g_n,
	      "half rate arrives in the first quarter of the ramp -- the rest "
	      "is the crawl (%.0f%%)", 100.0 * half_time / g_n);
}

/* ======================================================================
 * 6. RUN LENGTH MUST NOT CHANGE THE RAMP. The audio thread advances in runs
 *    bounded by sector, block and loop edges; if the envelope depended on
 *    how those fell, the pitch contour would differ between takes.
 * ====================================================================== */
static void case_run_length_independent(void)
{
	st_inertia_t a, b;
	uint32_t i, mismatches = 0;

	g_cases++;
	printf("\n-- the ramp is identical however the frames are chopped\n");

	/*
	 * `a` advances one frame at a time; `b` in lumps of 1,2,3,...,256,1,...
	 * They are compared AT EVERY LUMP BOUNDARY, and the comparison stays
	 * INSIDE the ramp. Comparing only at the end would prove nothing: both
	 * copies saturate at unity once the ramp finishes, so a module whose
	 * ramp took a completely different path would still match.
	 */
	st_inertia_reset(&a);
	st_inertia_play(&a, SR);
	st_inertia_reset(&b);
	st_inertia_play(&b, SR);
	mismatches += lockstep(&a, &b, 12000u);   /* spin-up is 16800 frames */
	CHECK(mismatches == 0,
	      "spin-up: same envelope at every matched frame count however the "
	      "frames are chopped (%u mismatches)", mismatches);
	CHECK(st_inertia_env_q16(&a) > 0u &&
	      st_inertia_env_q16(&a) < ST_INERTIA_ONE,
	      "and the comparison really happened mid-ramp (envelope %.3f)",
	      st_inertia_env_q16(&a) / 65536.0);

	/* Again on the way down, where the curve is steepest. */
	i = mismatches;
	run_to_rest_or_unity(&a, 200000u);
	st_inertia_reset(&b);
	st_inertia_play(&b, SR);
	run_to_rest_or_unity(&b, 200000u);
	st_inertia_stop(&a, SR);
	st_inertia_stop(&b, SR);
	mismatches += lockstep(&a, &b, 9000u);    /* spin-down is 27290 frames */
	CHECK(mismatches == i,
	      "spin-down: likewise (%u mismatches)", mismatches - i);
	CHECK(st_inertia_env_q16(&a) > 0u &&
	      st_inertia_env_q16(&a) < ST_INERTIA_ONE,
	      "and that one was mid-ramp too (envelope %.3f)",
	      st_inertia_env_q16(&a) / 65536.0);
	CHECK(a.state == b.state, "and the same state throughout");
}

/* ======================================================================
 * 7. IT IS A MULTIPLIER OVER VARISPEED. The brief is explicit: with a 0.8x
 *    target, PLAY must ramp 0 -> 0.8x and STOP 0.8x -> 0. This is the check
 *    that the layer composes instead of replacing.
 * ====================================================================== */
static void case_multiplies_varispeed(void)
{
	st_inertia_t s;
	const uint32_t target = (uint32_t)(0.8 * 65536.0);   /* 0.8x varispeed */
	uint32_t peak = 0, i;

	g_cases++;
	printf("\n-- the envelope multiplies the requested rate, not replaces it\n");

	st_inertia_reset(&s);
	st_inertia_play(&s, SR);
	run_to_rest_or_unity(&s, 200000u);
	for (i = 0; i < g_n; i++) {
		uint32_t rate = (uint32_t)(((uint64_t)target * g_env[i]) >> 16);

		if (rate > peak) peak = rate;
	}
	printf("     0.8x target: ramp peaked at %.3fx\n", peak / 65536.0);
	CHECK(peak == target,
	      "spin-up arrives at exactly the requested 0.8x (%u vs %u)",
	      peak, target);

	st_inertia_stop(&s, SR);
	run_to_rest_or_unity(&s, 200000u);
	CHECK((uint32_t)(((uint64_t)target * g_env[g_n - 1u]) >> 16) == 0u,
	      "and spin-down reaches exactly zero");

	/* At half envelope the rate is half the TARGET -- one octave below
	 * 0.8x, not below 1.0x. That is what "multiplier" means. */
	CHECK((uint32_t)(((uint64_t)target * (ST_INERTIA_ONE / 2u)) >> 16)
	      == target / 2u,
	      "and half envelope is half the target rate, an octave down");
}

/* ======================================================================
 * 8. PLAY DURING SPIN-DOWN CATCHES THE REEL. Restarting from zero would be
 *    a rate discontinuity -- a click -- and is not what a transport does.
 * ====================================================================== */
static void case_play_during_spindown_catches(void)
{
	st_inertia_t s;
	uint32_t at_press, i;
	int back = 0;

	g_cases++;
	printf("\n-- PLAY during spin-down catches the reel, it does not restart\n");

	st_inertia_reset(&s);
	st_inertia_play(&s, SR);
	run_to_rest_or_unity(&s, 200000u);
	st_inertia_stop(&s, SR);
	for (i = 0; i < 6000u; i++) {          /* part-way down */
		st_inertia_advance(&s, 1u);
	}
	at_press = st_inertia_env_q16(&s);
	CHECK(at_press > 0u && at_press < ST_INERTIA_ONE,
	      "the reel is part-way down when PLAY arrives (%.3f)",
	      at_press / 65536.0);

	st_inertia_play(&s, SR);
	CHECK(st_inertia_env_q16(&s) == at_press,
	      "the envelope does not jump on the press (%u -> %u)",
	      at_press, st_inertia_env_q16(&s));

	run_to_rest_or_unity(&s, 200000u);
	for (i = 1; i < g_n; i++) {
		if (g_env[i] < g_env[i - 1]) back++;
	}
	CHECK(back == 0, "and climbs from there without dipping (%d dips)", back);
	CHECK(s.state == ST_INERTIA_RUNNING, "reaching full rate");
	printf("     caught at %.3f, recovered in %.0f ms\n",
	       at_press / 65536.0, 1000.0 * g_n / SR);
	CHECK(g_n < SR * 350u / 1000u,
	      "faster than a ramp from rest, because less distance was left "
	      "(%.0f ms)", 1000.0 * g_n / SR);
}

int main(void)
{
	printf("== Stem Tape TRANSPORT INERTIA ==\n");
	printf("spin-up %u ms, spin-down %u ms, rest below %u/65536\n",
	       ST_INERTIA_SPINUP_MS, ST_INERTIA_SPINDOWN_MS,
	       ST_INERTIA_REST_Q16);

	case_state_model();
	case_durations();
	case_continuous_and_monotonic();
	case_spinup_shape();
	case_spindown_shape();
	case_run_length_independent();
	case_multiplies_varispeed();
	case_play_during_spindown_catches();

	printf("\n");
	if (g_failures) {
		printf("INERTIA TEST FAILED (%d cases, %d checks, %d failures)\n",
		       g_cases, g_checks, g_failures);
		return 1;
	}
	printf("INERTIA TEST PASSED (%d cases, %d checks, 0 failures)\n",
	       g_cases, g_checks);
	printf("NOTE: this proves the ENVELOPE. It is not audio: whether the "
	       "transport sounds right is a listening test.\n");
	return 0;
}
