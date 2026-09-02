/*
 * test_scratch.c -- the eMMC velocity clamp for the signed-head transport.
 *
 * Two kinds of case, and the difference matters.
 *
 * The first kind RE-DERIVES st_scratch.h's clamp from the measured read-cost
 * fit by a different arithmetic route (floating point, from first principles)
 * and requires the two to agree. That catches a hand-edited constant drifting
 * away from the measurement it is supposed to express -- the same discipline
 * tests/test_ladder.c applies to the ladder bands.
 *
 * The second kind does not model anything. It drives a synthetic scratch
 * gesture through the REAL st_stem_stream state machine and COUNTS the sectors
 * actually demanded, so the claim that oscillating inside the resident ring is
 * free is measured against the production ring rather than asserted. A model
 * that says scratching is affordable and a simulation that says it is not
 * would be exactly the disagreement worth finding before the audio thread ever
 * sees a signed rate.
 *
 *     cc -std=c11 -Wall -Wextra -I../src \
 *        ../src/st_stem_stream.c test_scratch.c -o test_scratch
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "st_scratch.h"
#include "st_stem_stream.h"

static int g_cases, g_checks, g_failures;

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
#define RUN(fn) do { g_cases++; printf("\n-- %s\n", #fn); fn(); } while (0)

/* A song long enough that a gesture can travel without ever reaching an end
 * and having the clamp confused with a clamp at frame 0. 600 sectors is about
 * 6.4 seconds of audio per stem. */
#define TEST_SECTORS 600u
#define TEST_FRAMES  (TEST_SECTORS * ST11_FRAMES_PER_SECTOR)
#define TEST_BLOCKS  (TEST_SECTORS * ST11_BLOCKS_PER_SECTOR)

/* ======================================================================
 * PART ONE -- the clamp, re-derived
 * ====================================================================== */

/* The measured fit, in floating point, exactly as tools/sp1-readcost-sweep.py
 * reports it. Deliberately NOT the header's Q8 integer form: if both used the
 * same arithmetic this case could only ever agree with itself. */
static double batch_us_fp(void)
{
	return 649.0 + 158.4 * (double)(ST_LAT_REFILL_GROUPS * ST_PL_GROUP_BLOCKS);
}

static double max_rate_fp(unsigned moving)
{
	const double cover  = (double)ST_LAT_REFILL_GROUPS * (double)ST_LAT_SECTOR_US;
	const double budget = cover * (double)ST_SCRATCH_BUDGET_PCT / 100.0;
	const double still  = (double)(ST_PL_STEMS - moving) * batch_us_fp();

	return (budget - still) / ((double)moving * batch_us_fp());
}

static void test_clamp_matches_the_measured_fit(void)
{
	unsigned m;

	CHECK(fabs((double)ST_SCRATCH_BATCH_US - batch_us_fp()) <= 1.0,
	      "one refill batch costs %u us; the measured fit says %.1f us",
	      ST_SCRATCH_BATCH_US, batch_us_fp());

	/* The Q8 carry must round the cost UP, never down: understating a read
	 * cost is the unsafe direction for a starvation clamp. */
	CHECK((double)ST_SCRATCH_BATCH_US >= batch_us_fp(),
	      "and it rounds UP, so the clamp can only ever be conservative");

	for (m = 1; m <= ST_PL_STEMS; m++) {
		const double want = max_rate_fp(m);
		const double got  = (double)ST_SCRATCH_MAX_RATE_Q16(m) / 65536.0;

		CHECK(fabs(got - want) < 0.01,
		      "%u head(s) moving: clamp %.3fx, re-derived %.3fx", m, got, want);
		CHECK(got <= want + 1e-9,
		      "and the integer form never exceeds the real-valued one");
	}
}

static void test_clamp_is_ordered_and_usable(void)
{
	const double master = (double)ST_SCRATCH_MAX_RATE_MASTER_Q16 / 65536.0;
	const double stem   = (double)ST_SCRATCH_MAX_RATE_STEM_Q16 / 65536.0;
	unsigned m;

	CHECK(stem > master,
	      "moving ONE head (%.2fx) is cheaper than moving four (%.2fx) -- which is "
	      "why an isolated stem scratch can be faster than a master one", stem, master);

	for (m = 2; m <= ST_PL_STEMS; m++) {
		CHECK(ST_SCRATCH_MAX_RATE_Q16(m) < ST_SCRATCH_MAX_RATE_Q16(m - 1u),
		      "every additional moving head lowers the clamp (%u -> %u)", m - 1u, m);
	}

	/* A clamp below unity would mean the shuttle could not even reach normal
	 * speed, which would make the whole gesture pointless rather than merely
	 * limited. */
	CHECK(master > 1.0, "the master clamp is above unity, so a shuttle can run");
	CHECK(stem > 2.0,
	      "and a single stem clears 2x, the region where reversed audio reads as a "
	      "scratch rather than as slow reverse playback");
}

static void test_the_budget_leaves_the_still_heads_room(void)
{
	/* Three heads at unity plus the moving one must fit. If the still heads
	 * alone exhausted the budget there would be no isolated scratch at all,
	 * and the clamp would come out negative rather than small -- a failure
	 * mode worth naming, because in unsigned arithmetic it wraps. */
	CHECK(ST_SCRATCH_STILL_US(1u) < ST_SCRATCH_BUDGET_US,
	      "three still heads cost %u us of a %u us budget, leaving room to move one",
	      ST_SCRATCH_STILL_US(1u), ST_SCRATCH_BUDGET_US);
	CHECK(ST_SCRATCH_MAX_RATE_Q16(1u) < 0x7FFFFFFFu,
	      "and the clamp did not wrap");
}

/* ======================================================================
 * PART TWO -- the free window, MEASURED through the real state machine
 * ====================================================================== */

/*
 * Drive a head through one oscillation of +/- `amplitude_frames` and count how
 * many DISTINCT sectors it demanded. The stream is told every sector is ready,
 * because what is under test is which sectors a gesture needs -- not whether a
 * read completed.
 *
 * `ring_slots` sectors are resident at any moment, centred on where the gesture
 * started, which is what the production ring holds. A demand inside that set is
 * free; a demand outside it is a read the eMMC has to service.
 */
static uint32_t sweep_and_count_reads(uint32_t start_frame, uint32_t amplitude_frames,
				       uint32_t cycles, uint32_t step_frames)
{
	st_stream_t st;
	uint32_t resident_lo, resident_hi;
	uint32_t reads = 0u;
	uint32_t c;

	if (!st_stream_init(&st, 0u, TEST_BLOCKS, TEST_FRAMES, TEST_SECTORS, false)) {
		printf("FATAL: test song geometry rejected\n");
		return 0xFFFFFFFFu;
	}
	st_stream_play(&st);
	(void)st_stream_seek(&st, start_frame);

	/* The resident set the ring holds around the starting sector. */
	{
		const uint32_t s = st_stream_required_sector(&st);
		const uint32_t half = (ST_LAT_RING_SLOTS - 1u) / 2u;

		resident_lo = (s > half) ? (s - half) : 0u;
		resident_hi = resident_lo + ST_LAT_RING_SLOTS - 1u;
	}

	for (c = 0; c < cycles; c++) {
		uint32_t dir;

		for (dir = 0; dir < 2u; dir++) {
			uint32_t moved = 0u;

			st_stream_set_reverse(&st, dir == 1u);
			while (moved < amplitude_frames) {
				uint32_t n = step_frames;
				uint32_t s;

				if (n > amplitude_frames - moved) {
					n = amplitude_frames - moved;
				}
				(void)st_stream_advance_frames(&st, n);
				moved += n;

				s = st_stream_required_sector(&st);
				if (s < resident_lo || s > resident_hi) {
					/* left the ring: one batch, and the ring
					 * re-centres on where it landed */
					reads++;
					resident_lo = (s > (ST_LAT_RING_SLOTS - 1u) / 2u) ?
						       (s - (ST_LAT_RING_SLOTS - 1u) / 2u) : 0u;
					resident_hi = resident_lo + ST_LAT_RING_SLOTS - 1u;
				}
				st_stream_sector_ready(&st, s);
			}
		}
	}
	return reads;
}

static void test_oscillating_inside_the_ring_costs_nothing(void)
{
	/*
	 * The free window is (G-1) sectors of audio. A gesture whose whole
	 * excursion fits inside it must never leave residency -- that is the
	 * claim st_scratch.h makes about why scratching is affordable at all,
	 * and here it is measured rather than argued.
	 *
	 * Half the window either side of the start, less one sector of slack so
	 * the case is testing the claim rather than its exact boundary.
	 */
	const uint32_t half_window = ((ST_LAT_RING_SLOTS - 1u) / 2u) * ST11_FRAMES_PER_SECTOR;
	const uint32_t amplitude   = half_window - ST11_FRAMES_PER_SECTOR;
	const uint32_t start       = 300u * ST11_FRAMES_PER_SECTOR;
	uint32_t reads;

	printf("       free window %u us of audio; testing +/-%u frames, %u cycles\n",
	       ST_SCRATCH_FREE_WINDOW_US, amplitude, 20u);

	reads = sweep_and_count_reads(start, amplitude, 20u, 64u);

	CHECK(reads == 0u,
	      "20 full oscillations of +/-%u frames demanded %u reads -- a scratch that "
	      "stays inside the ring is free, at any velocity", amplitude, reads);
}

static void test_travelling_beyond_the_ring_costs_reads(void)
{
	/*
	 * The complementary case, and the reason the one above is not vacuous:
	 * a gesture that DOES travel must cost reads. A simulation in which
	 * nothing ever leaves residency would pass the free-window case for the
	 * uninteresting reason that the counter is broken.
	 */
	const uint32_t amplitude = 40u * ST11_FRAMES_PER_SECTOR;   /* ~425 ms of audio */
	const uint32_t start     = 300u * ST11_FRAMES_PER_SECTOR;
	uint32_t reads = sweep_and_count_reads(start, amplitude, 2u, 64u);

	CHECK(reads > 0u,
	      "a +/-%u frame shuttle DOES leave the ring and demands reads (%u) -- so the "
	      "free-window case above is measuring something real", amplitude, reads);

	/*
	 * And it costs them at roughly the rate the clamp's model assumes: one
	 * batch per ST_LAT_REFILL_GROUPS sectors travelled. Two cycles of
	 * amplitude A cover 4A frames of travel.
	 */
	{
		const double sectors_travelled =
			4.0 * (double)amplitude / (double)ST11_FRAMES_PER_SECTOR;
		const double batches_ideal = sectors_travelled / (double)ST_LAT_REFILL_GROUPS;

		printf("       travelled %.0f sectors; %u reads, ideal batching would be %.0f\n",
		       sectors_travelled, reads, batches_ideal);
		CHECK((double)reads <= sectors_travelled + 1.0,
		      "and never more than one read per sector travelled, which is the "
		      "worst case the clamp would have to survive");
	}
}

static void test_the_free_window_is_stated_honestly(void)
{
	/* (G-1) sectors, in microseconds. Stated as its own check because the
	 * number appears in the header's prose and prose does not fail. */
	const uint32_t want = (ST_LAT_RING_SLOTS - 1u) * ST_LAT_SECTOR_US;

	CHECK(ST_SCRATCH_FREE_WINDOW_US == want,
	      "the free window is (%u-1) sectors = %u us = %.1f ms of audio",
	      ST_LAT_RING_SLOTS, want, want / 1000.0);
	CHECK(ST_SCRATCH_FREE_WINDOW_US >= 40000u,
	      "and it is at least 40 ms, or a hand movement would leave it instantly");
}

int main(void)
{
	printf("== Stem Tape SIGNED-HEAD VELOCITY CLAMP ==\n");
	printf("read cost: %u us + %.1f us/block   batch %u blocks = %u us\n",
	       ST_SCRATCH_RC_FIXED_US, ST_SCRATCH_RC_PER_BLOCK_Q8 / 256.0,
	       ST_SCRATCH_BATCH_BLOCKS, ST_SCRATCH_BATCH_US);
	printf("budget %u%% of %u us; ring %u slots, free window %.1f ms\n",
	       ST_SCRATCH_BUDGET_PCT, ST_SCRATCH_BATCH_COVER_US,
	       ST_LAT_RING_SLOTS, ST_SCRATCH_FREE_WINDOW_US / 1000.0);
	printf("CLAMP: master %.3fx, single stem %.3fx\n",
	       ST_SCRATCH_MAX_RATE_MASTER_Q16 / 65536.0,
	       ST_SCRATCH_MAX_RATE_STEM_Q16 / 65536.0);

	RUN(test_clamp_matches_the_measured_fit);
	RUN(test_clamp_is_ordered_and_usable);
	RUN(test_the_budget_leaves_the_still_heads_room);
	RUN(test_the_free_window_is_stated_honestly);
	RUN(test_oscillating_inside_the_ring_costs_nothing);
	RUN(test_travelling_beyond_the_ring_costs_reads);

	printf("\n%d distinct test cases, %d assertion checks\n", g_cases, g_checks);
	if (g_failures) {
		printf("SCRATCH CLAMP TEST FAILED (%d/%d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("SCRATCH CLAMP TEST PASSED (%d test cases, %d checks)\n", g_cases, g_checks);
	return 0;
}
