/*
 * test_readcost.c -- the arithmetic a sector-format change would rest on.
 *
 * The decision this gates is expensive and hard to reverse: a stem-planar
 * layout re-encodes every stored song. So the model is exercised against BOTH
 * hypotheses about where a read's time goes, and is required to reach OPPOSITE
 * conclusions about per-track reverse, because a model that says "feasible"
 * either way would be telling us nothing.
 *
 * Build (from the repo root):
 *   cc -std=c11 -Wall -Wextra -Werror -Ifirmware/stemtape_player/src \
 *      firmware/stemtape_player/src/st_readcost.c \
 *      firmware/stemtape_player/tests/test_readcost.c \
 *      -o test_readcost && ./test_readcost
 */

#include <stdio.h>
#include <string.h>

#include "st_latency.h"
#include "st_readcost.h"

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

/* The two headers must agree about how much audio a sector holds, or the duty
 * cycle this file computes is against a different budget than the streamer's. */
_Static_assert(ST_RC_SECTOR_US == ST_LAT_SECTOR_US,
	       "st_readcost.h and st_latency.h disagree on sector duration");

/* ---- the recorded capture, from docs/stem-tape-playback-physical-test.md -- */
#define CAP_DMA_US   2056u
#define CAP_HUNT_US  1763u
#define CAP_CRC_US   1104u
#define CAP_CMD_US    150u

/* Synthesise a sweep under a hypothesis about which phases scale. */
static void synth(uint32_t *blk, uint32_t *us, uint32_t n, bool hunt_per_block)
{
	static const uint32_t sizes[] = { 1u, 2u, 4u, 8u, 16u };
	const uint32_t scaling = CAP_DMA_US + CAP_CRC_US +
				 (hunt_per_block ? CAP_HUNT_US : 0u);
	const uint32_t fixed   = CAP_CMD_US +
				 (hunt_per_block ? 0u : CAP_HUNT_US);
	uint32_t i;

	for (i = 0; i < n; i++) {
		blk[i] = sizes[i % 5u];
		us[i]  = fixed + (scaling * blk[i]) / ST_RC_SECTOR_BLOCKS;
	}
}

/* ======================================================================
 * 1. THE FIT REPRODUCES THE CAPTURE. Whatever else it does, a 16-block read
 *    must come back out at the 5073 us that was actually measured.
 * ====================================================================== */
static void case_reproduces_the_capture(void)
{
	uint32_t blk[5], us[5];
	st_readcost_t rc;

	g_cases++;
	printf("\n-- the fit reproduces the measured 16-block read\n");

	synth(blk, us, 5u, true);
	CHECK(st_readcost_fit(blk, us, 5u, &rc), "the fit refused clean input");
	printf("     fixed %.1f us, per block %.1f us\n",
	       rc.fixed_us_q8 / 256.0, rc.per_block_us_q8 / 256.0);
	CHECK(st_readcost_predict_us(&rc, ST_RC_SECTOR_BLOCKS) ==
	      ST_LAT_READ_TYP_US,
	      "a full sector predicts %u us, not the measured %u",
	      st_readcost_predict_us(&rc, ST_RC_SECTOR_BLOCKS),
	      ST_LAT_READ_TYP_US);
}

/* ======================================================================
 * 2. THE TWO HYPOTHESES REACH OPPOSITE VERDICTS.
 *
 *    This is the case that gives the whole exercise its point. If the model
 *    said "feasible" under both, the sweep would be theatre.
 * ====================================================================== */
static void case_the_hypotheses_diverge(void)
{
	uint32_t blk[5], us[5];
	st_readcost_t a, b;
	uint32_t duty_a, duty_b;

	g_cases++;
	printf("\n-- the two hypotheses must disagree, or the sweep proves nothing\n");

	synth(blk, us, 5u, true);
	CHECK(st_readcost_fit(blk, us, 5u, &a), "hypothesis A failed to fit");
	synth(blk, us, 5u, false);
	CHECK(st_readcost_fit(blk, us, 5u, &b), "hypothesis B failed to fit");

	duty_a = st_readcost_planar_duty_ppm(&a, ST_RC_STEMS);
	duty_b = st_readcost_planar_duty_ppm(&b, ST_RC_STEMS);

	printf("     A (hunt per block): %u-block read %u us, four reversed "
	       "%.1f%% duty\n", ST_RC_PLANE_BLOCKS,
	       st_readcost_predict_us(&a, ST_RC_PLANE_BLOCKS), duty_a / 10000.0);
	printf("     B (hunt per read):  %u-block read %u us, four reversed "
	       "%.1f%% duty\n", ST_RC_PLANE_BLOCKS,
	       st_readcost_predict_us(&b, ST_RC_PLANE_BLOCKS), duty_b / 10000.0);

	CHECK(duty_a < 1000000u,
	      "under A four reversed tracks must fit (got %.1f%%)",
	      duty_a / 10000.0);
	CHECK(duty_b > 1000000u,
	      "under B four reversed tracks must NOT fit (got %.1f%%)",
	      duty_b / 10000.0);
	CHECK(st_readcost_fits(&a, ST_RC_STEMS, 100000u),
	      "under A four reversed should still leave 10 points spare");
	/*
	 * UNDER B, ONE REVERSED TRACK "FITS" AT 98.6% -- AND THAT IS A REFUSAL.
	 *
	 * Bare arithmetic says 98.6% is under 100%, so an earlier version of
	 * this check asserted it would not fit at all, and was simply wrong.
	 * The real statement is about HEADROOM. Every duty figure here assumes
	 * the streamer gets the whole wall clock, which it never does: it runs
	 * against the audio thread, the MIDI thread and the control loop, and
	 * docs/stem-tape-playback-physical-test.md records what happens when
	 * its share drops -- reads stretched from 5073 us to ~12500 us and the
	 * song played slow and crushed. A design left with 1.4 points spare is
	 * one scheduling hiccup from that. Ten points is the least that could
	 * honestly be called viable, and under B one reversed track does not
	 * have it.
	 */
	printf("     B, one reversed: %.1f%% duty -- fits on paper, no headroom\n",
	       st_readcost_planar_duty_ppm(&b, 1u) / 10000.0);
	CHECK(!st_readcost_fits(&b, 1u, 100000u),
	      "under B one reversed track must be refused for lack of headroom "
	      "(%.1f%% duty)", st_readcost_planar_duty_ppm(&b, 1u) / 10000.0);
	CHECK(st_readcost_fits(&a, 1u, 200000u),
	      "under A one reversed track should leave 20 points spare");
}

/* ======================================================================
 * 3. ZERO REVERSED TRACKS COSTS EXACTLY WHAT PLAYBACK COSTS TODAY.
 *
 *    The layout change must not tax ordinary playback. With nothing
 *    diverging the four planes are contiguous and are one read, so the duty
 *    must equal today's 5073/7083 and not a fraction more.
 * ====================================================================== */
static void case_all_forward_is_free(void)
{
	uint32_t blk[5], us[5];
	st_readcost_t rc;
	uint32_t today_ppm, planar_ppm;

	g_cases++;
	printf("\n-- with nothing reversed the layout change costs nothing\n");

	synth(blk, us, 5u, true);
	(void)st_readcost_fit(blk, us, 5u, &rc);

	today_ppm  = (uint32_t)(((uint64_t)ST_LAT_READ_TYP_US * 1000000u) /
				 ST_LAT_SECTOR_US);
	planar_ppm = st_readcost_planar_duty_ppm(&rc, 0u);

	printf("     today %.1f%%, stem-planar all-forward %.1f%%\n",
	       today_ppm / 10000.0, planar_ppm / 10000.0);
	CHECK(planar_ppm <= today_ppm + 2000u,
	      "all-forward planar duty %.2f%% exceeds today's %.2f%%",
	      planar_ppm / 10000.0, today_ppm / 10000.0);
}

/* ======================================================================
 * 4. THE COST RISES WITH DIVERGENCE, MONOTONICALLY. Each extra reversed
 *    track adds one read, and nothing else.
 * ====================================================================== */
static void case_cost_rises_with_divergence(void)
{
	uint32_t blk[5], us[5];
	st_readcost_t rc;
	uint32_t prev = 0u, n;

	g_cases++;
	printf("\n-- each diverging track adds one read's fixed cost\n");

	synth(blk, us, 5u, true);
	(void)st_readcost_fit(blk, us, 5u, &rc);

	printf("     reversed  duty\n");
	for (n = 0; n <= ST_RC_STEMS; n++) {
		const uint32_t d = st_readcost_planar_duty_ppm(&rc, n);

		printf("        %u     %5.1f%%\n", n, d / 10000.0);
		CHECK(d >= prev,
		      "%u reversed cost less than %u reversed", n, n - 1u);
		prev = d;
	}
	CHECK(st_readcost_planar_duty_ppm(&rc, ST_RC_STEMS) < 1000000u,
	      "the worst case (all four reversed) must still fit");
}

/* ======================================================================
 * 5. THE FIT REFUSES INPUT IT CANNOT HONESTLY FIT.
 *
 *    A gate that returns a confident number from garbage is worse than no
 *    gate, because the number is what a format change would be justified by.
 * ====================================================================== */
static void case_refuses_bad_input(void)
{
	st_readcost_t rc;
	uint32_t one_blk[2] = { 4u, 4u };
	uint32_t one_us[2]  = { 1381u, 1390u };
	uint32_t neg_blk[3] = { 1u, 4u, 16u };
	uint32_t neg_us[3]  = { 5000u, 3000u, 1000u };   /* bigger reads cheaper */

	g_cases++;
	printf("\n-- the fit refuses input it cannot honestly fit\n");

	CHECK(!st_readcost_fit(one_blk, one_us, 1u, &rc),
	      "a single sample must not produce a fit");
	CHECK(!st_readcost_fit(one_blk, one_us, 2u, &rc),
	      "two samples at the SAME size determine no slope");
	CHECK(!st_readcost_fit(neg_blk, neg_us, 3u, &rc),
	      "a negative slope is a broken measurement and must be refused");
	CHECK(!rc.valid, "a refused fit must not be left marked valid");
	CHECK(st_readcost_predict_us(&rc, 4u) == 0u,
	      "an invalid fit must predict nothing");
	CHECK(!st_readcost_fits(&rc, 1u, 0u),
	      "an invalid fit must never report the feature as affordable");
}

/* ======================================================================
 * 6. NOISE DOES NOT FLIP THE VERDICT. Real reads jitter; a fit that only
 *    survives perfect input would not survive the bench.
 * ====================================================================== */
static void case_tolerates_noise(void)
{
	uint32_t blk[15], us[15];
	st_readcost_t rc;
	uint32_t i, duty;

	g_cases++;
	printf("\n-- a noisy sweep still reaches the right verdict\n");

	synth(blk, us, 15u, true);
	/* +/- 8% deterministic wobble, alternating so it cannot cancel into a
	 * clean average by luck. */
	for (i = 0; i < 15u; i++) {
		const uint32_t d = us[i] / 12u;

		us[i] = (i & 1u) ? us[i] + d : us[i] - d;
	}
	CHECK(st_readcost_fit(blk, us, 15u, &rc), "the fit refused noisy input");
	duty = st_readcost_planar_duty_ppm(&rc, ST_RC_STEMS);
	printf("     with +/-8%% jitter: four reversed %.1f%% duty\n",
	       duty / 10000.0);
	CHECK(duty < 1000000u,
	      "8%% jitter flipped the verdict to infeasible (%.1f%%)",
	      duty / 10000.0);
}

int main(void)
{
	printf("== Stem Tape READ-COST MODEL (per-track reverse feasibility) ==\n");
	printf("sector %u us of audio, %u blocks; a stem plane is %u blocks\n",
	       ST_RC_SECTOR_US, ST_RC_SECTOR_BLOCKS, ST_RC_PLANE_BLOCKS);

	case_reproduces_the_capture();
	case_the_hypotheses_diverge();
	case_all_forward_is_free();
	case_cost_rises_with_divergence();
	case_refuses_bad_input();
	case_tolerates_noise();

	printf("\n");
	if (g_failures) {
		printf("READCOST TEST FAILED (%d cases, %d checks, %d failures)\n",
		       g_cases, g_checks, g_failures);
		return 1;
	}
	printf("READCOST TEST PASSED (%d cases, %d checks, 0 failures)\n",
	       g_cases, g_checks);
	printf("NOTE: this proves the ARITHMETIC and that the two hypotheses "
	       "diverge. WHICH hypothesis holds is a hardware measurement -- "
	       "run the 'M' sweep and feed its numbers in before changing any "
	       "sector layout.\n");
	return 0;
}
