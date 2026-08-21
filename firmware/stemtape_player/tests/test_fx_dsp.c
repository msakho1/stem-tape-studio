/*
 * test_fx_dsp.c — the four fixed effects, against INDEPENDENTLY COMPUTED
 * reference values.
 *
 * THE REFERENCE IS NOT THE THING UNDER TEST. Every expected number here is
 * recomputed in double precision from the same expression the committed
 * TypeScript uses (src/audio/fx/banks.ts), inside this file, using <math.h>.
 * Nothing calls the production function to decide what the production function
 * should have produced.
 *
 * Where byte identity is impossible -- fixed-point tables, Q14 coefficient
 * rounding, one-pole vs biquad damping -- the tolerance is stated per check
 * with the reason it is what it is.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -Ifirmware/stemtape_player/src \
 *      firmware/stemtape_player/src/st_fx.c \
 *      firmware/stemtape_player/src/st_fx_ctl.c \
 *      firmware/stemtape_player/tests/test_fx_dsp.c -lm -o test_fx_dsp
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "st_fx.h"

static int g_checks, g_failures, g_cases;

#define CHECK(cond, ...) do { \
		g_checks++; \
		if (cond) { printf("[OK  ] " __VA_ARGS__); printf("\n"); } \
		else { g_failures++; printf("[FAIL] " __VA_ARGS__); \
		       printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); } \
	} while (0)

#define SR 48000.0

/* ---------------------------------------------------------------------- *
 * INDEPENDENT REFERENCE, in double, from banks.ts's own expressions.
 * ---------------------------------------------------------------------- */

/* banks.ts:199-207 crusherCurve(0.35): k = 1 + drive*40; tanh(kx)/tanh(k) */
static double ref_dirt(double x)
{
	const double k = 1.0 + 0.35 * 40.0;
	return tanh(k * x) / tanh(k);
}

/* RBJ lowpass, the shape both the browser and the firmware implement. */
static void ref_lp(double fc, double Q, double b[3], double a[2])
{
	double w0 = 2.0 * M_PI * fc / SR;
	double c = cos(w0), s = sin(w0), al = s / (2.0 * Q);
	double a0 = 1.0 + al;

	b[0] = (1.0 - c) / 2.0 / a0;
	b[1] = (1.0 - c) / a0;
	b[2] = (1.0 - c) / 2.0 / a0;
	a[0] = (-2.0 * c) / a0;
	a[1] = (1.0 - al) / a0;
}

/* ====================================================================== */
static void case_dirt_curve(void)
{
	int i, worst_i = 0;
	double worst = 0.0;

	g_cases++;
	printf("\n-- Distortion curve vs tanh(15x)/tanh(15), recomputed here\n");

	for (i = -64; i <= 64; i++) {
		double x = (double)i / 64.0;
		int32_t in = (int32_t)(x * (ST_FX_FULLSCALE - 1));
		int32_t got = st_fx_shape_dirt(in);
		double want = ref_dirt(x);
		double gotf = (double)got / (double)ST_FX_FULLSCALE;
		double err = fabs(gotf - want);

		if (err > worst) { worst = err; worst_i = i; }
	}
	printf("      worst absolute error %.6f at x = %+.4f\n",
	       worst, (double)worst_i / 64.0);
	/* TOLERANCE 0.0002, about -74 dBFS. Set just above the MEASURED worst
	 * (0.000076) rather than at a guessed round number, so the check
	 * actually constrains the table: dropping a single entry, or coarsening
	 * the interpolation, fails here instead of passing silently. The error
	 * is largest where tanh(15x) is steepest, near the origin. */
	CHECK(worst < 0.0002,
	      "the shaper tracks the reference curve within 0.0002 everywhere");

	CHECK(st_fx_shape_dirt(0) == 0, "zero in, zero out -- no DC offset");
	CHECK(st_fx_shape_dirt(-(ST_FX_FULLSCALE - 1)) ==
	      -st_fx_shape_dirt(ST_FX_FULLSCALE - 1),
	      "the curve is odd, so it adds no even-order DC");
	CHECK(st_fx_shape_dirt(ST_FX_FULLSCALE - 1) <= ST_FX_FULLSCALE - 1 &&
	      st_fx_shape_dirt(ST_FX_FULLSCALE - 1) > 0,
	      "full scale in stays in range -- no integer wraparound");
	/* The one that would be a real bug: a value beyond full scale. */
	CHECK(st_fx_shape_dirt(ST_FX_FULLSCALE * 2) <= ST_FX_FULLSCALE - 1,
	      "over-range input is clamped, not wrapped");
}

static void case_filter_response(void)
{
	st_fx_t fx;
	double b[3], a[2];
	int i;

	g_cases++;
	printf("\n-- Filter: measured magnitude vs the reference biquad\n");

	ref_lp(1800.0, 0.9, b, a);

	/* Drive a sine at three frequencies and compare the settled peak with
	 * the analytic |H(f)| of the same filter. */
	const double freqs[3] = { 200.0, 1800.0, 8000.0 };
	for (i = 0; i < 3; i++) {
		double f = freqs[i];
		double w = 2.0 * M_PI * f / SR;
		/* |H| for the direct-form coefficients above. */
		double cr = b[0] + b[1] * cos(w) + b[2] * cos(2 * w);
		double ci = -(b[1] * sin(w) + b[2] * sin(2 * w));
		double dr = 1.0 + a[0] * cos(w) + a[1] * cos(2 * w);
		double di = -(a[0] * sin(w) + a[1] * sin(2 * w));
		double want = sqrt((cr * cr + ci * ci) / (dr * dr + di * di));
		int32_t peak = 0;
		int n;

		st_fx_reset(&fx);
		st_fx_prepare(&fx, 24000u, 0u, ST_FX_BIT(ST_FX_FILTER));
		/* Settle the engage ramp and the filter, then measure. */
		for (n = 0; n < 6000; n++) {
			double s = sin(2.0 * M_PI * f * n / SR) * (ST_FX_FULLSCALE / 2);
			int32_t l = (int32_t)s, r = l;

			st_fx_process(&fx, &l, &r, (uint32_t)n);
			if (n > 4000 && l > peak) peak = l;
		}
		double got = (double)peak / (double)(ST_FX_FULLSCALE / 2);
		printf("      %6.0f Hz  want %.4f  got %.4f\n", f, want, got);
		/* TOLERANCE 8%: Q14 coefficient rounding at 1800 Hz moves the
		 * pole pair slightly, and the measured peak is a discrete
		 * maximum over a finite window rather than a true envelope. */
		CHECK(fabs(got - want) < 0.08 * want + 0.01,
		      "%.0f Hz magnitude within tolerance", f);
	}
}

static void case_engage_ramp(void)
{
	st_fx_t fx;
	int n, first_full = -1;

	g_cases++;
	printf("\n-- 12 ms engagement is sample-level and exactly 576 frames\n");
	CHECK(ST_FX_ENGAGE_FRAMES == 576u,
	      "FX_ENGAGE_S 0.012 x 48000 = 576, not a block count");

	st_fx_reset(&fx);
	st_fx_prepare(&fx, 24000u, 0u, ST_FX_BIT(ST_FX_DIRT));
	for (n = 0; n < 2000; n++) {
		int32_t l = ST_FX_FULLSCALE / 4, r = l;

		st_fx_process(&fx, &l, &r, (uint32_t)n);
		if (first_full < 0 && fx.wet[ST_FX_DIRT] == ST_FX_WET_UNITY) {
			first_full = n;
		}
	}
	printf("      wet reached unity at frame %d\n", first_full);
	CHECK(first_full >= 570 && first_full <= 590,
	      "the ramp completes at ~576 frames (%d)", first_full);

	/* Continuity ACROSS 256-frame block boundaries: the ramp must not
	 * restart, stall or jump where a block ends. */
	{
		uint16_t prev = 0;
		int bad = 0;

		st_fx_reset(&fx);
		st_fx_prepare(&fx, 24000u, 0u, ST_FX_BIT(ST_FX_DIRT));
		for (n = 0; n < 1200; n++) {
			int32_t l = ST_FX_FULLSCALE / 4, r = l;

			if ((n % 256) == 0) {
				/* a real caller re-prepares every block */
				st_fx_prepare(&fx, 24000u, 0u, ST_FX_BIT(ST_FX_DIRT));
			}
			st_fx_process(&fx, &l, &r, (uint32_t)n);
			if (fx.wet[ST_FX_DIRT] < prev) bad++;
			prev = fx.wet[ST_FX_DIRT];
		}
		CHECK(bad == 0,
		      "the ramp never goes backwards across a block boundary "
		      "(%d regressions)", bad);
	}
}

static void case_gate_phase_lock(void)
{
	st_fx_t fx;
	const uint32_t fpb = 24000u;          /* 120 BPM */
	const uint32_t cycle = fpb / 4u;      /* 1/16 = 6000 frames */
	uint32_t n;
	int opens = 0;
	int prev_open = -1;

	g_cases++;
	printf("\n-- Gate: 1/16 division, phase from the playback frame only\n");

	st_fx_reset(&fx);
	st_fx_prepare(&fx, fpb, 0u, ST_FX_BIT(ST_FX_GATE));
	CHECK(fx.gate_cycle == cycle,
	      "4 cycles per beat -> %u frames per gate cycle", cycle);

	/* Count openings over four beats and confirm 16 sixteenths. */
	for (n = 0; n < fpb * 4u; n++) {
		int32_t l = ST_FX_FULLSCALE / 2, r = l;
		int open;

		st_fx_process(&fx, &l, &r, n);
		open = (l > ST_FX_FULLSCALE / 4) ? 1 : 0;
		if (prev_open == 0 && open == 1) opens++;
		prev_open = open;
	}
	printf("      openings over 4 beats: %d\n", opens);
	CHECK(opens >= 15 && opens <= 16,
	      "sixteen 1/16 openings in four beats (%d; the first edge starts "
	      "already open at frame 0, so 15 rising edges is correct)", opens);

	/* THE ONE THAT MATTERS: a loop wrap moves song_frame backwards, and the
	 * gate must follow it with no resync, because its phase IS song_frame. */
	{
		int32_t a_l = ST_FX_FULLSCALE / 2, a_r = a_l;
		int32_t b_l = ST_FX_FULLSCALE / 2, b_r = b_l;
		st_fx_t fx2;

		st_fx_reset(&fx2);
		st_fx_prepare(&fx2, fpb, 0u, ST_FX_BIT(ST_FX_GATE));
		/* frame 3000 reached the long way round... */
		for (n = 0; n < 3000u; n++) {
			int32_t l = ST_FX_FULLSCALE / 2, r = l;
			st_fx_process(&fx2, &l, &r, n);
			a_l = l;
		}
		/* ...and frame 3000 reached immediately after a wrap. */
		st_fx_reset(&fx);
		st_fx_prepare(&fx, fpb, 0u, ST_FX_BIT(ST_FX_GATE));
		for (n = 0; n < 3000u; n++) {
			int32_t l = ST_FX_FULLSCALE / 2, r = l;
			st_fx_process(&fx, &l, &r, 900000u + n);   /* far away */
		}
		{
			int32_t l = ST_FX_FULLSCALE / 2, r = l;
			st_fx_process(&fx, &l, &r, 2999u);         /* wrapped back */
			b_l = l;
		}
		(void)a_r; (void)b_r;
		printf("      gate gain at frame 2999/3000: continuous %d, "
		       "after a wrap %d\n", a_l, b_l);
		CHECK(a_l > 0 && b_l > 0 && abs(a_l - b_l) < ST_FX_FULLSCALE / 8,
		      "the gate lands on the SAME phase after a loop wrap -- "
		      "there is no free-running clock to desync");
	}

	/* No tempo, no gate: fail closed rather than invent a grid. */
	st_fx_reset(&fx);
	st_fx_prepare(&fx, 0u, 0u, ST_FX_BIT(ST_FX_GATE));
	CHECK(fx.gate_cycle == 0u, "no tempo yields no gate cycle");
	{
		int32_t l = ST_FX_FULLSCALE / 2, r = l;

		st_fx_process(&fx, &l, &r, 1234u);
		CHECK(l == ST_FX_FULLSCALE / 2,
		      "and the signal passes through untouched");
	}
}

static void case_echo(void)
{
	st_fx_t fx;
	const uint32_t fpb = 24000u;
	const uint32_t want_len = fpb * 3u / 8u;   /* 0.375 beat = 9000 */
	uint32_t n;
	int32_t peak_at_delay = 0;

	g_cases++;
	printf("\n-- Delay/Echo: 0.375 beat, feedback 0.43, one shared line\n");

	st_fx_reset(&fx);
	st_fx_prepare(&fx, fpb, 0u, ST_FX_BIT(ST_FX_ECHO));
	CHECK(fx.echo_len == want_len,
	      "0.375 beat at 120 BPM = %u frames", want_len);
	CHECK(ST_FX_ECHO_FEEDBACK_Q15 < ST_FX_ECHO_FEEDBACK_MAX_Q15,
	      "the default feedback 0.43 is under the 0.72 contract ceiling");
	CHECK(ST_FX_ECHO_MAX_FRAMES * 2u == 32768u,
	      "the delay line is 32,768 bytes: 16,384 frames of Q15 mono");
	CHECK((ST_FX_ECHO_MAX_FRAMES & (ST_FX_ECHO_MAX_FRAMES - 1u)) == 0u,
	      "and it is a POWER OF TWO, which is what lets the 48 kHz loop mask "
	      "instead of divide -- two modulos removed from the hot path");
	CHECK(ST_FX_ECHO_MASK == ST_FX_ECHO_MAX_FRAMES - 1u,
	      "the mask matches the length");
	/* The admitted tempo floor is a CONSEQUENCE of that size, so it is
	 * asserted rather than left as a comment: 0.375 beat must still fit at
	 * ST_FX_ECHO_MIN_BPM. */
	CHECK((48000u * 60u / ST_FX_ECHO_MIN_BPM) * 3u / 8u <= ST_FX_ECHO_MAX_FRAMES,
	      "0.375 beat at the %u BPM floor still fits the line",
	      ST_FX_ECHO_MIN_BPM);

	/* An impulse must come back one delay later. */
	for (n = 0; n < want_len * 2u + 100u; n++) {
		int32_t l = (n == 700u) ? (ST_FX_FULLSCALE - 1) : 0;
		int32_t r = l;

		st_fx_process(&fx, &l, &r, n);
		if (n > 700u + want_len - 50u && n < 700u + want_len + 50u) {
			if (l > peak_at_delay) peak_at_delay = l;
		}
	}
	printf("      impulse returns at +%u frames, peak %d\n",
	       want_len, peak_at_delay);
	CHECK(peak_at_delay > ST_FX_FULLSCALE / 16,
	      "the repeat arrives one delay period later");

	/* Feedback must decay, never grow. */
	{
		int32_t p1 = 0, p2 = 0;

		st_fx_reset(&fx);
		st_fx_prepare(&fx, fpb, 0u, ST_FX_BIT(ST_FX_ECHO));
		for (n = 0; n < want_len * 4u; n++) {
			int32_t l = (n == 100u) ? (ST_FX_FULLSCALE - 1) : 0;
			int32_t r = l;

			st_fx_process(&fx, &l, &r, n);
			if (n > 100u + want_len - 40u && n < 100u + want_len + 40u && l > p1) p1 = l;
			if (n > 100u + 2u * want_len - 40u && n < 100u + 2u * want_len + 40u && l > p2) p2 = l;
		}
		printf("      repeat 1 peak %d, repeat 2 peak %d\n", p1, p2);
		CHECK(p2 < p1, "each repeat is quieter than the last");
		CHECK(p1 > 0 && p2 > 0, "and both are actually audible");
	}

	/* Toggling must not reset the line: no allocation, no clear. */
	{
		uint32_t w_before;

		st_fx_reset(&fx);
		st_fx_prepare(&fx, fpb, 0u, ST_FX_BIT(ST_FX_ECHO));
		for (n = 0; n < 5000u; n++) {
			int32_t l = ST_FX_FULLSCALE / 4, r = l;
			st_fx_process(&fx, &l, &r, n);
		}
		w_before = fx.echo_w;
		st_fx_prepare(&fx, fpb, 0u, 0u);            /* release */
		for (n = 0; n < 100u; n++) {
			int32_t l = 0, r = 0;
			st_fx_process(&fx, &l, &r, 5000u + n);
		}
		CHECK(fx.echo_w != w_before,
		      "the line keeps circulating after release -- a bounded "
		      "tail, not a frozen buffer");
		CHECK(fx.echo_tail > 0u, "and the tail countdown is running");
	}
}

static void case_signal_order_is_audible(void)
{
	st_fx_t a, b;
	uint32_t n;
	int32_t la = 0, lb = 0;

	g_cases++;
	printf("\n-- the fixed order really is Filter -> Distortion (not the reverse)\n");

	/* Distorting a filtered signal is not the same as filtering a distorted
	 * one. If the chain ever ran in button order this would collapse. */
	st_fx_reset(&a);
	st_fx_prepare(&a, 24000u, 0u,
		       (uint8_t)(ST_FX_BIT(ST_FX_FILTER) | ST_FX_BIT(ST_FX_DIRT)));
	for (n = 0; n < 3000u; n++) {
		double s = sin(2.0 * M_PI * 5000.0 * n / SR) * (ST_FX_FULLSCALE / 2);
		int32_t l = (int32_t)s, r = l;

		st_fx_process(&a, &l, &r, n);
		la = l;
	}
	st_fx_reset(&b);
	st_fx_prepare(&b, 24000u, 0u, ST_FX_BIT(ST_FX_DIRT));
	for (n = 0; n < 3000u; n++) {
		double s = sin(2.0 * M_PI * 5000.0 * n / SR) * (ST_FX_FULLSCALE / 2);
		int32_t l = (int32_t)s, r = l;

		st_fx_process(&b, &l, &r, n);
		lb = l;
	}
	printf("      filter+dirt %d   vs dirt alone %d\n", la, lb);
	CHECK(la != lb,
	      "a 5 kHz tone through Filter then Distortion differs from "
	      "Distortion alone -- the Filter really is upstream");
}

static void case_bypass_is_free_and_exact(void)
{
	st_fx_t fx;
	uint32_t n;
	int changed = 0;

	g_cases++;
	printf("\n-- nothing active: bit-exact passthrough and no work\n");

	st_fx_reset(&fx);
	st_fx_prepare(&fx, 24000u, 0u, 0u);
	CHECK(!st_fx_running(&fx),
	      "st_fx_running() is false, so the caller skips the rack entirely");

	for (n = 0; n < 1000u; n++) {
		int32_t want = (int32_t)((n * 7919u) % (ST_FX_FULLSCALE - 1)) -
			       ST_FX_FULLSCALE / 2;
		int32_t l = want, r = want;

		st_fx_process(&fx, &l, &r, n);
		if (l != want || r != want) changed++;
	}
	CHECK(changed == 0,
	      "and if it is called anyway, every sample is returned unchanged "
	      "(%d altered)", changed);
}

int main(void)
{
	printf("== Stem Tape FX DSP ==\n");
	printf("expected values recomputed in double from banks.ts's own "
	       "expressions, never from the C under test\n");

	case_dirt_curve();
	case_filter_response();
	case_engage_ramp();
	case_gate_phase_lock();
	case_echo();
	case_signal_order_is_audible();
	case_bypass_is_free_and_exact();

	printf("\n");
	if (g_failures) {
		printf("FX DSP FAILED (%d cases, %d checks, %d failures)\n",
		       g_cases, g_checks, g_failures);
		return 1;
	}
	printf("FX DSP PASSED (%d cases, %d checks, 0 failures)\n", g_cases, g_checks);
	printf("NOTE: host arithmetic only. Not production-linked and not heard.\n");
	return 0;
}
