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


/* ======================================================================
 * PART THREE -- the transport primitive
 * ====================================================================== */

/* One control pass at the real ~8 ms cadence. */
#define PASS_US 8000u

static double rate_of(const st_scratch_t *s) { return st_scratch_rate_q16(s) / 65536.0; }

/* Hold a drive for `ms` and return the rate reached. */
static double hold(st_scratch_t *s, int32_t drive, uint32_t ms)
{
	uint32_t t;

	for (t = 0; t < ms * 1000u; t += PASS_US) {
		st_scratch_set_drive(s, drive);
		st_scratch_tick(s, PASS_US);
	}
	return rate_of(s);
}

static void test_a_sustained_hold_shuttles_to_the_clamp(void)
{
	st_scratch_t s;
	double r;

	st_scratch_begin(&s, 0, ST_SCRATCH_MAX_RATE_MASTER_Q16);
	r = hold(&s, ST_SCRATCH_DRIVE_FULL, 400u);

	CHECK(fabs(r - ST_SCRATCH_MAX_RATE_MASTER_Q16 / 65536.0) < 0.01,
	      "holding forward reaches the clamp and stays there: %.3fx", r);

	/* And it STAYS. A shuttle that crept past its clamp, or drifted back
	 * off it, would be the transport arguing with the player. */
	r = hold(&s, ST_SCRATCH_DRIVE_FULL, 400u);
	CHECK(fabs(r - ST_SCRATCH_MAX_RATE_MASTER_Q16 / 65536.0) < 0.01,
	      "and another 400 ms of hold does not push past it: %.3fx", r);

	r = hold(&s, -ST_SCRATCH_DRIVE_FULL, 400u);
	CHECK(fabs(r + ST_SCRATCH_MAX_RATE_MASTER_Q16 / 65536.0) < 0.01,
	      "holding backward shuttles in reverse at the clamp: %.3fx", r);
}

static void test_short_presses_scratch_without_a_mode(void)
{
	/*
	 * THE CENTRAL CLAIM. Nothing in st_scratch.c measures press duration or
	 * names a mode, so short alternating presses must produce oscillation
	 * and a long press must produce travel -- purely as a consequence of the
	 * same integrator. If a seam ever appears between the two, it will show
	 * up here as one of these two cases failing while the other passes.
	 */
	st_scratch_t s;
	double peak_fwd = 0.0, peak_rev = 0.0;
	int crossings = 0;
	int prev_sign = 0;
	int cycle;

	st_scratch_begin(&s, 0, ST_SCRATCH_MAX_RATE_MASTER_Q16);

	/* 8 alternating 60 ms presses -- a scratch rhythm, roughly 8 Hz. */
	for (cycle = 0; cycle < 8; cycle++) {
		const int32_t d = (cycle & 1) ? -ST_SCRATCH_DRIVE_FULL : ST_SCRATCH_DRIVE_FULL;
		uint32_t t;

		for (t = 0; t < 60u * 1000u; t += PASS_US) {
			int sign;

			st_scratch_set_drive(&s, d);
			st_scratch_tick(&s, PASS_US);

			if (rate_of(&s) > peak_fwd) { peak_fwd = rate_of(&s); }
			if (rate_of(&s) < peak_rev) { peak_rev = rate_of(&s); }

			sign = (st_scratch_rate_q16(&s) > 0) ? 1 :
			       (st_scratch_rate_q16(&s) < 0) ? -1 : 0;
			if (sign != 0 && prev_sign != 0 && sign != prev_sign) {
				crossings++;
			}
			if (sign != 0) { prev_sign = sign; }
		}
	}

	printf("       8 presses of 60 ms: peak +%.2fx / %.2fx, %d zero crossings\n",
	       peak_fwd, peak_rev, crossings);

	CHECK(peak_fwd > 0.5, "short presses reach a real forward velocity (%.2fx), not a wobble",
	      peak_fwd);
	CHECK(peak_rev < -0.5, "and a real reverse one (%.2fx)", peak_rev);
	CHECK(crossings >= 6,
	      "and the head genuinely reverses direction on each press (%d crossings) -- "
	      "that is scratching, produced by nothing but press timing", crossings);
}

static void test_the_zero_crossing_is_continuous(void)
{
	/*
	 * Forward -> slow -> zero -> reverse must be a walk, not a jump. The
	 * largest single-tick change anywhere across a full reversal bounds how
	 * big a discontinuity the resampler can ever be handed.
	 */
	st_scratch_t s;
	double max_step = 0.0;
	double closest_to_zero = 1e9;
	double prev;
	uint32_t t;
	uint32_t ticks_to_cross = 0u;
	bool crossed = false;

	st_scratch_begin(&s, 0, ST_SCRATCH_MAX_RATE_MASTER_Q16);
	(void)hold(&s, ST_SCRATCH_DRIVE_FULL, 400u);   /* settled forward */

	prev = rate_of(&s);
	for (t = 0; t < 400u * 1000u; t += PASS_US) {
		double now;

		st_scratch_set_drive(&s, -ST_SCRATCH_DRIVE_FULL);
		st_scratch_tick(&s, PASS_US);
		now = rate_of(&s);
		if (fabs(now - prev) > max_step) { max_step = fabs(now - prev); }
		if (fabs(now) < closest_to_zero) { closest_to_zero = fabs(now); }
		if (!crossed) {
			ticks_to_cross++;
			if (now < 0.0) { crossed = true; }
		}
		prev = now;
	}

	/*
	 * "PASSES THROUGH ZERO" MEANS BOUNDED, NOT PAUSED.
	 *
	 * The first version of this case asked the rate to land within +/-0.05 of
	 * zero at some tick. It does not, and should not have been expected to:
	 * one legal decel step is 0.425, so the crossing tick steps +0.106 ->
	 * -0.319 and simply passes over that band. Asking for a dwell at zero is
	 * asking for a granularity the ramp does not have and the audio does not
	 * need -- what the resampler is handed is a rate CHANGE, not a sample
	 * discontinuity.
	 *
	 * The real property is that the sign change costs one bounded step and
	 * that getting there took many ticks, so a reversal can never be a flip.
	 */
	CHECK(closest_to_zero <=
	      (ST_SCRATCH_MAX_RATE_MASTER_Q16 / 65536.0) *
	      ((double)PASS_US / 1000.0) / (double)ST_SCRATCH_DECEL_MS,
	      "the rate comes within one ramp step of zero (%.4fx) before changing sign -- "
	      "it passes through rather than over", closest_to_zero);
	CHECK(ticks_to_cross >= 6u,
	      "and reaching the far side took %u ticks, so a reversal is a walk and can "
	      "never be a sign flip", ticks_to_cross);
	/*
	 * BOUND IT BY THE RAMP, not by a number that looked small. One tick may
	 * legally move the rate by (clamp * dt / ramp_ms); anything larger would
	 * mean the walk jumped. Asserting the real bound is what makes this case
	 * survive a change to either constant.
	 */
	{
		const double legal =
			(ST_SCRATCH_MAX_RATE_MASTER_Q16 / 65536.0) *
			((double)PASS_US / 1000.0) / (double)ST_SCRATCH_DECEL_MS;

		CHECK(max_step <= legal + 1e-6,
		      "and the largest single-tick change across the crossing is %.4fx, within "
		      "the %.4fx one ramp step allows -- a walk, not a sign flip",
		      max_step, legal);
	}
	CHECK(rate_of(&s) < -1.0, "and it ends up genuinely reversed (%.2fx)", rate_of(&s));
}

static void test_releasing_the_push_stops_the_tape(void)
{
	st_scratch_t s;
	double r;

	st_scratch_begin(&s, 0, ST_SCRATCH_MAX_RATE_MASTER_Q16);
	(void)hold(&s, ST_SCRATCH_DRIVE_FULL, 400u);

	r = hold(&s, 0, 200u);
	CHECK(fabs(r) < 0.001,
	      "drive back to zero brings the head to a genuine standstill (%.4fx) -- "
	      "the hand resting on the record, which is a thing a player asks for", r);

	/* And the decel is FASTER than the accel, which is what makes it feel
	 * like a hand rather than a motor. */
	{
		st_scratch_t a, b;
		uint32_t t, accel_us = 0, decel_us = 0;

		st_scratch_begin(&a, 0, ST_SCRATCH_MAX_RATE_MASTER_Q16);
		for (t = 0; t < 1000u * 1000u; t += PASS_US) {
			st_scratch_set_drive(&a, ST_SCRATCH_DRIVE_FULL);
			st_scratch_tick(&a, PASS_US);
			accel_us += PASS_US;
			if (st_scratch_rate_q16(&a) >= a.max_rate_q16) { break; }
		}
		b = a;
		for (t = 0; t < 1000u * 1000u; t += PASS_US) {
			st_scratch_set_drive(&b, 0);
			st_scratch_tick(&b, PASS_US);
			decel_us += PASS_US;
			if (st_scratch_rate_q16(&b) == 0) { break; }
		}
		printf("       accel %u us, decel %u us\n", accel_us, decel_us);
		CHECK(decel_us < accel_us,
		      "stopping (%u us) is quicker than starting (%u us)", decel_us, accel_us);
	}
}

static void test_grabbing_a_moving_tape_starts_from_its_motion(void)
{
	st_scratch_t s;

	/* Unity playback, then FUNCTION goes down. */
	st_scratch_begin(&s, 65536, ST_SCRATCH_MAX_RATE_MASTER_Q16);
	CHECK(st_scratch_rate_q16(&s) == 65536,
	      "grabbing a tape running at 1x starts the gesture AT 1x, not at a standstill -- "
	      "a hand landing on a spinning record does not stop it dead");

	/* A rate the new target cannot afford is clamped on entry rather than
	 * refused, so the grab always succeeds and the bound still holds. */
	st_scratch_begin(&s, 8 * 65536, ST_SCRATCH_MAX_RATE_MASTER_Q16);
	CHECK(st_scratch_rate_q16(&s) == (int32_t)ST_SCRATCH_MAX_RATE_MASTER_Q16,
	      "and grabbing one running faster than this target may go clamps on entry (%.2fx)",
	      rate_of(&s));

	/* Release hands the rate back and moves nothing. */
	{
		int32_t r = st_scratch_release(&s);

		CHECK(r == (int32_t)ST_SCRATCH_MAX_RATE_MASTER_Q16,
		      "release hands the signed rate back for st_scrub's ramp to take over");
		CHECK(!s.engaged, "and the gesture is no longer live");
	}
}

static void test_the_clamp_is_never_exceeded(void)
{
	/*
	 * The whole starvation argument rests on this. Drive is saturated, so no
	 * caller can ask for more than full deflection -- and even a caller that
	 * tries must not get it.
	 */
	st_scratch_t s;
	uint32_t t;
	int32_t worst = 0;

	st_scratch_begin(&s, 0, ST_SCRATCH_MAX_RATE_STEM_Q16);
	for (t = 0; t < 2000u * 1000u; t += PASS_US) {
		st_scratch_set_drive(&s, 100 * ST_SCRATCH_DRIVE_FULL);  /* absurd */
		st_scratch_tick(&s, PASS_US);
		if (st_scratch_rate_q16(&s) > worst) { worst = st_scratch_rate_q16(&s); }
	}
	CHECK(worst == (int32_t)ST_SCRATCH_MAX_RATE_STEM_Q16,
	      "a drive 100x past full deflection still tops out exactly at the clamp (%.3fx)",
	      worst / 65536.0);

	st_scratch_begin(&s, 0, ST_SCRATCH_MAX_RATE_STEM_Q16);
	worst = 0;
	for (t = 0; t < 2000u * 1000u; t += PASS_US) {
		st_scratch_set_drive(&s, -100 * ST_SCRATCH_DRIVE_FULL);
		st_scratch_tick(&s, PASS_US);
		if (st_scratch_rate_q16(&s) < worst) { worst = st_scratch_rate_q16(&s); }
	}
	CHECK(worst == -(int32_t)ST_SCRATCH_MAX_RATE_STEM_Q16,
	      "and symmetrically in reverse (%.3fx)", worst / 65536.0);
}

static void test_master_heads_cannot_drift(void)
{
	/*
	 * Master scratch must keep the four stems sample-locked. They are locked
	 * because they are driven by ONE rate, so the property to prove is that
	 * the same gesture through independent instances yields bit-identical
	 * rates -- if it did not, sharing one instance would be hiding a
	 * divergence rather than preventing one.
	 */
	st_scratch_t a, b;
	uint32_t t;
	int diverged = 0;

	st_scratch_begin(&a, 0, ST_SCRATCH_MAX_RATE_MASTER_Q16);
	st_scratch_begin(&b, 0, ST_SCRATCH_MAX_RATE_MASTER_Q16);

	for (t = 0; t < 3000u * 1000u; t += PASS_US) {
		const int32_t d = ((t / 47000u) & 1u) ? -ST_SCRATCH_DRIVE_FULL
						       : ST_SCRATCH_DRIVE_FULL;

		st_scratch_set_drive(&a, d);
		st_scratch_tick(&a, PASS_US);
		st_scratch_set_drive(&b, d);
		st_scratch_tick(&b, PASS_US);
		if (st_scratch_rate_q16(&a) != st_scratch_rate_q16(&b)) { diverged++; }
	}
	CHECK(diverged == 0,
	      "3 s of irregular scratching leaves two independently-ticked heads "
	      "bit-identical -- the integrator is deterministic, so locked heads stay locked");
}

static void test_the_fader_maps_movement_not_position(void)
{
	/* Standing still asks for nothing, wherever the fader physically is. */
	CHECK(st_scratch_drive_from_fader(0, PASS_US) == 0,
	      "a stationary fader asks for zero drive, at any position");

	/* Noise-sized movement is rejected. A few counts at 125 Hz is a few
	 * hundred counts per second, which is a resting finger, not a hand. */
	CHECK(st_scratch_drive_from_fader(1, PASS_US) == 0,
	      "one count per pass (125 counts/s) is ADC noise and produces no drive");

	/* A brisk sweep saturates; a slow one does not. */
	{
		const int32_t fast = st_scratch_drive_from_fader(200, PASS_US);
		const int32_t slow = st_scratch_drive_from_fader(6, PASS_US);

		printf("       fast sweep -> %d, slow -> %d (full is %d)\n",
		       fast, slow, ST_SCRATCH_DRIVE_FULL);
		CHECK(fast == ST_SCRATCH_DRIVE_FULL, "a fast sweep saturates the drive");
		CHECK(slow > 0 && slow < ST_SCRATCH_DRIVE_FULL / 4,
		      "and a slow movement asks for a correspondingly small one (%d)", slow);
	}

	/* Direction follows the movement's sign, and the map is symmetric. */
	CHECK(st_scratch_drive_from_fader(-200, PASS_US) == -ST_SCRATCH_DRIVE_FULL,
	      "moving the other way drives the other way, by the same amount");

	/* The deadband is SUBTRACTED, not stepped over: drive must rise from
	 * zero continuously as movement crosses the threshold, or every slow
	 * scratch would begin with a lurch. */
	{
		int32_t at_edge = 0;
		int32_t d;

		for (d = 1; d < 40; d++) {
			int32_t v = st_scratch_drive_from_fader(d, PASS_US);

			if (v > 0) { at_edge = v; break; }
		}
		CHECK(at_edge > 0 && at_edge < ST_SCRATCH_DRIVE_FULL / 20,
		      "the first movement past the deadband asks for a TINY drive (%d), "
		      "not a step up to the deadband's worth", at_edge);
	}
}

static void test_the_rocker_reports_direction_only(void)
{
	CHECK(st_scratch_drive_from_rocker(1) == ST_SCRATCH_DRIVE_FULL,
	      "the rocker held forward drives at full deflection");
	CHECK(st_scratch_drive_from_rocker(-1) == -ST_SCRATCH_DRIVE_FULL,
	      "held backward, full deflection the other way");
	CHECK(st_scratch_drive_from_rocker(0) == 0,
	      "released, nothing -- it is a switch and reports which way, never how far, "
	      "which is why press DURATION is what spans scratching and shuttling");
}

int main(void)
{
	printf("== Stem Tape SIGNED-HEAD TRANSPORT + VELOCITY CLAMP ==\n");
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

	RUN(test_a_sustained_hold_shuttles_to_the_clamp);
	RUN(test_short_presses_scratch_without_a_mode);
	RUN(test_the_zero_crossing_is_continuous);
	RUN(test_releasing_the_push_stops_the_tape);
	RUN(test_grabbing_a_moving_tape_starts_from_its_motion);
	RUN(test_the_clamp_is_never_exceeded);
	RUN(test_master_heads_cannot_drift);
	RUN(test_the_fader_maps_movement_not_position);
	RUN(test_the_rocker_reports_direction_only);

	printf("\n%d distinct test cases, %d assertion checks\n", g_cases, g_checks);
	if (g_failures) {
		printf("SCRATCH CLAMP TEST FAILED (%d/%d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("SCRATCH CLAMP TEST PASSED (%d test cases, %d checks)\n", g_cases, g_checks);
	return 0;
}
