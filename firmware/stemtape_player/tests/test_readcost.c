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
	 * THE TWO HYPOTHESES MUST STILL REACH OPPOSITE CONCLUSIONS SOMEWHERE.
	 *
	 * That is this file's whole design, stated in its own header: a model
	 * that says "feasible" under both hypotheses is telling us nothing.
	 * It used to be pinned at ONE reversed track, where B gave 98.6% duty
	 * -- under 100% on bare arithmetic, but with 1.4 points of headroom
	 * against a streamer that never gets the whole wall clock, which is a
	 * refusal.
	 *
	 * v1.3 changed the units underneath that. A 16-bit sector carries 510
	 * frames instead of 340, so a sector lasts 10,625 us instead of 7,083
	 * and every duty figure falls by a third: under B one reversed track is
	 * now 65.8%, genuinely affordable, and the old assertion started
	 * demanding that a comfortable design be refused.
	 *
	 * So the case finds the divergence point instead of naming it. It is
	 * still the same claim -- the model discriminates -- and it no longer
	 * encodes a snapshot of which track count it discriminates at.
	 *
	 * (The hunt question the two hypotheses represent is now settled by
	 * measurement: tools/sp1-readcost-sweep.py fitted us = 650 + 159*blocks
	 * on hardware, so the hunt is per BLOCK at 5.6 us and hypothesis A is
	 * the real one. This case is kept because it proves the MODEL can tell
	 * them apart, which is what makes that measurement worth trusting.)
	 */
	{
		uint32_t n, diverge = 0u;

		for (n = 1u; n <= ST_RC_STEMS; n++) {
			if (st_readcost_fits(&a, n, 100000u) &&
			    !st_readcost_fits(&b, n, 100000u)) {
				diverge = n;
				break;
			}
		}
		printf("     hypotheses diverge at %u reversed track(s): "
		       "A %.1f%%, B %.1f%%\n", diverge,
		       diverge ? st_readcost_planar_duty_ppm(&a, diverge) / 10000.0 : 0.0,
		       diverge ? st_readcost_planar_duty_ppm(&b, diverge) / 10000.0 : 0.0);
		CHECK(diverge != 0u,
		      "the two hypotheses must disagree about SOME number of "
		      "reversed tracks, or the model cannot gate the format "
		      "decision it exists for");
	}
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

/* ======================================================================
 * 7. THE REAL MEASUREMENT, from hardware.
 *
 *    st32's 'M' sweep on the actual card, 24 reads per size. This is the
 *    case that settles the question; everything above only proves the
 *    arithmetic could tell the two answers apart.
 *
 *    TWO THINGS LANDED DIFFERENTLY FROM THE PREDICTION, and both are
 *    recorded here rather than quietly absorbed:
 *
 *    THE READS ARE MUCH FASTER than st_latency.h's ST_LAT_READ_TYP_US.
 *    A full sector measured 3152 us uncontended, not 5073. That figure came
 *    from a boot capture WITH contention and is not wrong -- it is a
 *    different quantity. Nothing here overwrites it: the read-ahead depth is
 *    sized from contended worst cases and must stay that way.
 *
 *    THE FIXED COST IS 649 us, NOT 150. The phase breakdown suggested only
 *    the CMD18/CMD12 handshake was per-read, so 3% fixed was predicted; the
 *    truth is 21%. The feature is still affordable, but for a different
 *    reason than predicted -- not because the fixed cost is negligible, but
 *    because the whole read is fast enough to pay it four times over.
 * ====================================================================== */
static void case_the_real_measurement(void)
{
	/* blocks, avg_us, hunt_us -- STEMRC, firmware st32 */
	static const uint32_t blk[5]  = { 1u, 2u, 4u, 8u, 16u };
	static const uint32_t avg[5]  = { 675u, 1040u, 1340u, 1945u, 3152u };
	static const uint32_t hunt[5] = { 11u, 17u, 31u, 56u, 109u };
	st_readcost_t rc;
	uint32_t i, duty4, duty0;

	g_cases++;
	printf("\n-- THE HARDWARE ANSWER (st32 sweep, 24 reads per size)\n");

	/*
	 * THE DECIDING PROPERTY, checked directly rather than through the fit:
	 * the start-bit hunt must scale with the read. Per block it roughly
	 * quadruples from 4 blocks to 16; per read it would not move at all.
	 */
	printf("     hunt: ");
	for (i = 0; i < 5u; i++) {
		printf("%u blk=%u us  ", blk[i], hunt[i]);
	}
	printf("\n     hunt(16)/hunt(4) = %.2f\n", (double)hunt[4] / hunt[2]);
	CHECK((double)hunt[4] / hunt[2] > 2.5,
	      "the start-bit hunt did not scale with the read (%.2f) -- that is "
	      "hypothesis B and per-track reverse is impossible",
	      (double)hunt[4] / hunt[2]);
	/* And it is not scaling super-linearly either, which would mean bigger
	 * reads are disproportionately punished and the planar split helps even
	 * more than claimed. Stated so the number is bounded on both sides. */
	CHECK((double)hunt[4] / hunt[2] < 5.0,
	      "hunt scaled %.2f across a 4x size step -- steeper than linear",
	      (double)hunt[4] / hunt[2]);

	CHECK(st_readcost_fit(blk, avg, 5u, &rc),
	      "the real sweep failed to fit");
	printf("     fit: %.0f us fixed + %.1f us per block\n",
	       rc.fixed_us_q8 / 256.0, rc.per_block_us_q8 / 256.0);

	duty0 = st_readcost_planar_duty_ppm(&rc, 0u);
	duty4 = st_readcost_planar_duty_ppm(&rc, ST_RC_STEMS);
	printf("     duty: all forward %.1f%%, all four reversed %.1f%%\n",
	       duty0 / 10000.0, duty4 / 10000.0);

	CHECK(duty4 < 1000000u,
	      "all four reversed needs %.1f%% of the read engine", duty4 / 10000.0);
	/* Twenty points spare is the least that could honestly be called
	 * viable -- see the hypothesis-B note above about headroom. */
	CHECK(st_readcost_fits(&rc, ST_RC_STEMS, 200000u),
	      "all four reversed leaves under 20 points of headroom (%.1f%%)",
	      duty4 / 10000.0);
	CHECK(duty0 < duty4,
	      "reversing tracks must cost more than not reversing them");
}

/* ======================================================================
 * 8. THE PLANAR READ PLAN COVERS THE SECTOR EXACTLY ONCE.
 *
 *    The simulation reads the same sector in four pieces. A gap would leave
 *    stale bytes in the buffer and an overlap would leave one quarter
 *    missing -- either way the audio is wrong, and wrong in a way that would
 *    be blamed on the read pattern rather than on the plan. So the coverage
 *    is proved byte by byte rather than eyeballed.
 * ====================================================================== */
static void case_planar_plan_covers_the_sector(void)
{
	st_rc_read_t plan[ST_RC_PLAN_MAX];
	uint8_t seen[ST_RC_SECTOR_BLOCKS];
	uint32_t nrev, n, i, b, gaps = 0, dups = 0;

	g_cases++;
	printf("\n-- every divergence level covers the sector exactly once\n");

	for (nrev = 0; nrev <= ST_RC_STEMS; nrev++) {
		memset(seen, 0, sizeof(seen));
		n = st_readcost_plan_planar(plan, nrev);

		printf("     %u reversed -> %u read%s:", nrev, n,
		       n == 1u ? " " : "s");
		for (i = 0; i < n; i++) {
			printf(" blk%u+%u", plan[i].block_off, plan[i].blocks);
			CHECK(plan[i].buf_off == plan[i].block_off * 512u,
			      "read %u writes byte %u for block %u -- mismatched",
			      i, plan[i].buf_off, plan[i].block_off);
			for (b = 0; b < plan[i].blocks; b++) {
				const uint32_t blk = plan[i].block_off + b;

				if (blk >= ST_RC_SECTOR_BLOCKS) {
					gaps++;
					continue;
				}
				if (seen[blk]) {
					dups++;
				}
				seen[blk] = 1u;
			}
		}
		printf("\n");
		for (b = 0; b < ST_RC_SECTOR_BLOCKS; b++) {
			if (!seen[b]) {
				gaps++;
			}
		}
		CHECK(n <= ST_RC_PLAN_MAX,
		      "%u reversed needs %u reads, more than the plan holds",
		      nrev, n);
		/* One read per diverging track, plus ONE for whatever stays
		 * contiguous. This is the cost model the duty table is built
		 * on, so the plan has to match it exactly. */
		CHECK(n == nrev + (nrev < ST_RC_STEMS ? 1u : 0u),
		      "%u reversed produced %u reads, expected %u", nrev, n,
		      nrev + (nrev < ST_RC_STEMS ? 1u : 0u));
	}
	CHECK(gaps == 0, "%u blocks were never read across the levels", gaps);
	CHECK(dups == 0, "%u blocks were read twice across the levels", dups);

	/* Level 0 must be the single full-sector read playback already does,
	 * or arming the gate at zero would itself change the shipped path. */
	n = st_readcost_plan_planar(plan, 0u);
	CHECK(n == 1u && plan[0].block_off == 0u &&
	      plan[0].blocks == ST_RC_SECTOR_BLOCKS,
	      "level 0 is not the single full-sector read");

	/* And divergence still reads back to front. */
	n = st_readcost_plan_planar(plan, ST_RC_STEMS);
	CHECK(plan[0].block_off > plan[n - 1u].block_off,
	      "the plan reads forwards (%u then %u); sequential read-ahead "
	      "would flatter the measurement",
	      plan[0].block_off, plan[n - 1u].block_off);
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
	case_the_real_measurement();
	case_planar_plan_covers_the_sector();

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
