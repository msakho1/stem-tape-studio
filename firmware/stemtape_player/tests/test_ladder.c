/*
 * test_ladder.c — the shared AIN0 ladder classifier, host-tested against the
 * PHYSICAL MEASUREMENT, not against a model.
 *
 * Every stimulus below is a real number captured from the user's SP-1 with
 * build st16-cal (docs/ladder-measured.json). The measured centres are the
 * only constants this file declares; the band table is not transcribed here,
 * it is RE-DERIVED from those centres by the documented rule and compared to
 * the firmware's own st_ladder_bands[]. A hand-edited band therefore fails
 * the build rather than silently diverging from the hardware.
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -Werror -I../src ../src/st_ladder.c \
 *      test_ladder.c -o test_ladder && ./test_ladder
 */

#include <stdio.h>
#include <string.h>

#include "st_ladder.h"

static int checks, failures;

#define CHECK(cond, fmt, ...)                                                  \
	do {                                                                   \
		checks++;                                                      \
		if (!(cond)) {                                                 \
			failures++;                                            \
			printf("  FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,  \
			       ##__VA_ARGS__);                                 \
		}                                                              \
	} while (0)

/* ---------------------------------------------------------------------- *
 * THE MEASUREMENT. docs/ladder-measured.json, transcribed once.
 * Index == Track mask for rows 1..15; index 16 is PLAY.
 * ---------------------------------------------------------------------- */
#define NROWS 16

typedef struct {
	int centre;
	int lo;      /* lowest RAW observed while holding it   */
	int hi;      /* highest RAW observed while holding it  */
	const char *name;
} meas_t;

static const meas_t meas[NROWS] = {
	{  205,  199,  211, "T1"          },
	{  400,  396,  405, "T2"          },
	{  570,  565,  574, "T1+T2"       },
	{  727,  722,  733, "T3"          },
	{  868,  866,  871, "T1+T3"       },
	{  993,  989,  998, "T2+T3"       },
	{ 1104, 1102, 1107, "T1+T2+T3"    },
	{ 1213, 1209, 1216, "T4"          },
	{ 1309, 1307, 1310, "T1+T4"       },
	{ 1396, 1396, 1396, "T2+T4"       },
	{ 1480, 1480, 1480, "T1+T2+T4"    },
	{ 1559, 1559, 1559, "T3+T4"       },
	{ 1628, 1628, 1628, "T1+T3+T4"    },
	{ 1695, 1693, 1698, "T2+T3+T4"    },
	{ 1755, 1749, 1761, "all four"    },
	{ 1813, 1808, 1819, "PLAY"        },
};

/* The rule from docs/ladder-measured.json: half = min(25, 40% of the nearest
 * neighbour gap). Below the first row the neighbour is the idle ceiling. */
static int half_width(int i)
{
	int below = (i == 0) ? (meas[0].centre - ST_LADDER_IDLE_MAX)
			     : (meas[i].centre - meas[i - 1].centre);
	int above = (i == NROWS - 1) ? below
				     : (meas[i + 1].centre - meas[i].centre);
	int gap = (below < above) ? below : above;
	int h = (gap * 40) / 100;

	return (h > 25) ? 25 : h;
}

static void expect(st_ladder_t *l, uint8_t mask, bool play, const char *what)
{
	CHECK(st_ladder_mask(l) == mask && st_ladder_play(l) == play,
	      "%s: expected mask=%X play=%d, got mask=%X play=%d", what, mask,
	      (int)play, st_ladder_mask(l), (int)st_ladder_play(l));
}

/* Feed `raw` `n` times. */
static void feed(st_ladder_t *l, int raw, int n)
{
	for (int k = 0; k < n; k++) {
		st_ladder_update(l, raw);
	}
}

/* ---------------------------------------------------------------------- *
 * 1. The firmware table IS the measurement, derived by the stated rule.
 * ---------------------------------------------------------------------- */
static void case_table_matches_measurement(void)
{
	printf("case 1: st_ladder_bands[] re-derived from the physical capture\n");

	CHECK(ST_LADDER_ROWS == NROWS, "row count %u != %d", ST_LADDER_ROWS, NROWS);

	for (int i = 0; i < NROWS; i++) {
		int h = half_width(i);
		int want_lo = meas[i].centre - h;
		int want_hi = meas[i].centre + h;
		const st_ladder_band_t *b = &st_ladder_bands[i];

		if (i == NROWS - 1) {
			/* PLAY is open-ended upward: nothing reads higher. */
			CHECK(b->play == 1u, "row %d should be the PLAY row", i);
			CHECK(b->lo == (uint16_t)want_lo,
			      "PLAY lo: want %d, table has %u", want_lo, b->lo);
			CHECK(b->hi == 4095u,
			      "PLAY hi: want 4095, table has %u", b->hi);
			continue;
		}
		CHECK(b->play == 0u, "row %d (%s) must not be a PLAY row", i,
		      meas[i].name);
		/* THE ORDERING PROPERTY: on this hardware voltage order and
		 * mask order coincide, so row index == mask. */
		CHECK(b->mask == (uint8_t)(i + 1),
		      "row %d (%s): mask should be %d, table has %u", i,
		      meas[i].name, i + 1, b->mask);
		CHECK(b->lo == (uint16_t)want_lo && b->hi == (uint16_t)want_hi,
		      "%s: rule gives %d..%d, table has %u..%u", meas[i].name,
		      want_lo, want_hi, b->lo, b->hi);
	}
}

/* ---------------------------------------------------------------------- *
 * 2. No band aliases a neighbour, and every measured extreme is in band.
 * ---------------------------------------------------------------------- */
static void case_no_aliasing(void)
{
	printf("case 2: bands are disjoint, contain their own measured extremes,\n"
	       "        and never reach a neighbour's centre\n");

	for (int i = 0; i + 1 < NROWS; i++) {
		CHECK(st_ladder_bands[i].hi < st_ladder_bands[i + 1].lo,
		      "%s hi %u overlaps %s lo %u", meas[i].name,
		      st_ladder_bands[i].hi, meas[i + 1].name,
		      st_ladder_bands[i + 1].lo);
		CHECK((int)st_ladder_bands[i].hi < meas[i + 1].centre,
		      "%s reaches %s's centre", meas[i].name, meas[i + 1].name);
		CHECK((int)st_ladder_bands[i + 1].lo > meas[i].centre,
		      "%s reaches %s's centre", meas[i + 1].name, meas[i].name);
	}
	/* Exactly one row is settled at a time, so only one row is ever widened.
	 * The invariant that matters is therefore: a WIDENED row must not reach
	 * its unwidened neighbour's band, in either direction. */
	for (int i = 0; i + 1 < NROWS; i++) {
		CHECK((int)st_ladder_bands[i].hi + ST_LADDER_HYSTERESIS <
		      (int)st_ladder_bands[i + 1].lo,
		      "widened %s reaches %s", meas[i].name, meas[i + 1].name);
		CHECK((int)st_ladder_bands[i + 1].lo - ST_LADDER_HYSTERESIS >
		      (int)st_ladder_bands[i].hi,
		      "widened %s reaches %s", meas[i + 1].name, meas[i].name);
	}
	for (int i = 0; i < NROWS; i++) {
		CHECK(meas[i].lo >= (int)st_ladder_bands[i].lo &&
		      meas[i].hi <= (int)st_ladder_bands[i].hi,
		      "%s: observed %d..%d is outside band %u..%u",
		      meas[i].name, meas[i].lo, meas[i].hi,
		      st_ladder_bands[i].lo, st_ladder_bands[i].hi);
	}
	CHECK(st_ladder_bands[0].lo > ST_LADDER_IDLE_MAX,
	      "T1's band reaches down into idle");
}

/* ---------------------------------------------------------------------- *
 * 3. Every measured state settles -- all 15 masks AND play.
 * ---------------------------------------------------------------------- */
static void case_every_measured_state_settles(void)
{
	printf("case 3: all 15 Track masks and PLAY settle from the measured\n"
	       "        centre and from both measured extremes\n");

	for (int i = 0; i < NROWS; i++) {
		bool is_play = (i == NROWS - 1);
		uint8_t want = is_play ? 0u : (uint8_t)(i + 1);
		int stim[3] = { meas[i].centre, meas[i].lo, meas[i].hi };

		for (int s = 0; s < 3; s++) {
			st_ladder_t l;

			st_ladder_reset(&l);
			feed(&l, stim[s], ST_LADDER_SETTLE_READS);
			expect(&l, want, is_play, meas[i].name);
		}
	}
}

/* ---------------------------------------------------------------------- *
 * 4. THE st15 REGRESSION. A chord formed from idle in one step must settle.
 * ---------------------------------------------------------------------- */
static void case_no_slew_deadlock(void)
{
	printf("case 4: st15 regression -- a large single-step jump must still\n"
	       "        settle (the removed slew guard made this impossible)\n");

	/* Idle -> T1+T2 is a 570-count step; idle -> all four is 1755. Both are
	 * far beyond the 40-count threshold st15 refused to commit through. */
	for (int i = 0; i < NROWS - 1; i++) {
		st_ladder_t l;

		st_ladder_reset(&l);
		feed(&l, 0, (int)ST_LADDER_RELEASE_READS);
		feed(&l, meas[i].centre, ST_LADDER_SETTLE_READS);
		expect(&l, (uint8_t)(i + 1), false, meas[i].name);
	}

	/* And chord-to-chord, which is the same size of step: T1 -> T2+T3+T4. */
	{
		st_ladder_t l;

		st_ladder_reset(&l);
		feed(&l, meas[0].centre, ST_LADDER_SETTLE_READS);
		expect(&l, 0x1u, false, "T1 before the jump");
		feed(&l, meas[13].centre, ST_LADDER_SETTLE_READS);
		expect(&l, 0xEu, false, "T1 -> T2+T3+T4");
	}
}

/* ---------------------------------------------------------------------- *
 * 5. Noise: guard-zone samples hold, never latch, and never invent a mask.
 * ---------------------------------------------------------------------- */
static void case_noise_holds_and_still_settles(void)
{
	printf("case 5: guard-zone noise holds the settled state, cannot latch\n"
	       "        the decoder off, and never invents a mask\n");

	/* A settled chord survives a burst of guard-zone samples. */
	{
		st_ladder_t l;
		int guard = (int)st_ladder_bands[2].hi + ST_LADDER_HYSTERESIS + 5;

		st_ladder_reset(&l);
		feed(&l, meas[2].centre, ST_LADDER_SETTLE_READS);
		expect(&l, 0x3u, false, "T1+T2 settled");
		feed(&l, guard, 50);
		expect(&l, 0x3u, false, "T1+T2 held through 50 guard samples");
	}

	/* A chord still settles when every other sample is guard-zone noise --
	 * the case st15's slew guard turned into a permanent stall. */
	{
		st_ladder_t l;
		int guard = (int)st_ladder_bands[5].lo - ST_LADDER_HYSTERESIS - 5;

		st_ladder_reset(&l);
		for (int k = 0; k < 40; k++) {
			st_ladder_update(&l, meas[5].centre);
			st_ladder_update(&l, guard);
		}
		expect(&l, 0x6u, false, "T2+T3 through alternating noise");
	}

	/* Pure guard-zone noise, forever, never proposes anything. */
	{
		st_ladder_t l;

		st_ladder_reset(&l);
		for (int k = 0; k < 200; k++) {
			st_ladder_update(&l, (int)st_ladder_bands[8].lo - 12);
			st_ladder_update(&l, (int)st_ladder_bands[9].hi + 12);
		}
		expect(&l, 0u, false, "sustained guard-zone noise");
	}
}

/* ---------------------------------------------------------------------- *
 * 6. ADC error holds; idle releases in one read.
 * ---------------------------------------------------------------------- */
static void case_adc_error_and_release(void)
{
	printf("case 6: an ADC error (-1) HOLDS a chord; a CONFIRMED idle releases\n");

	st_ladder_t l;

	st_ladder_reset(&l);
	feed(&l, meas[6].centre, ST_LADDER_SETTLE_READS);
	expect(&l, 0x7u, false, "T1+T2+T3 settled");

	feed(&l, -1, 30);
	expect(&l, 0x7u, false, "T1+T2+T3 through 30 ADC errors");

	st_ladder_update(&l, 0);
	expect(&l, 0x7u, false,
	       "ONE idle read does NOT drop the chord -- a coupled dip is not a "
	       "release");
	st_ladder_update(&l, 0);
	expect(&l, 0u, false, "the CONFIRMED release clears it");

	/* PLAY is held through errors too. */
	st_ladder_reset(&l);
	feed(&l, meas[15].centre, ST_LADDER_SETTLE_READS);
	expect(&l, 0u, true, "PLAY settled");
	feed(&l, -1, 10);
	expect(&l, 0u, true, "PLAY through ADC errors");
	feed(&l, ST_LADDER_IDLE_MAX, (int)ST_LADDER_RELEASE_READS);
	expect(&l, 0u, false, "PLAY released");
}

/* ---------------------------------------------------------------------- *
 * 7. PLAY and the Track masks never impersonate each other.
 * ---------------------------------------------------------------------- */
static void case_play_never_aliases_a_chord(void)
{
	printf("case 7: PLAY never decodes as a chord and no chord -- including\n"
	       "        all four -- ever decodes as PLAY\n");

	for (int i = 0; i < NROWS - 1; i++) {
		for (int v = meas[i].lo; v <= meas[i].hi; v++) {
			st_ladder_read_t r = st_ladder_classify(v, 0u, false);

			CHECK(r.cls == ST_LADDER_TRACKS &&
			      r.mask == (uint8_t)(i + 1),
			      "%s at raw %d classified as cls=%d mask=%X",
			      meas[i].name, v, (int)r.cls, r.mask);
		}
	}
	for (int v = meas[15].lo; v <= meas[15].hi; v++) {
		st_ladder_read_t r = st_ladder_classify(v, 0u, false);

		CHECK(r.cls == ST_LADDER_PLAY, "PLAY at raw %d classified %d", v,
		      (int)r.cls);
	}
	/* The specific st15 false claim: all four is NOT PLAY. */
	{
		st_ladder_t l;

		st_ladder_reset(&l);
		feed(&l, meas[14].centre, ST_LADDER_SETTLE_READS);
		expect(&l, 0xFu, false, "all four is a chord, not PLAY");
	}
	/* Even fully widened, PLAY cannot be reached from all-four. */
	{
		st_ladder_read_t r =
			st_ladder_classify(meas[14].hi, 0xFu, false);

		CHECK(r.cls == ST_LADDER_TRACKS && r.mask == 0xFu,
		      "widened all-four leaked into PLAY");
	}
}

/* ---------------------------------------------------------------------- *
 * 8. Hysteresis widens only the settled row.
 * ---------------------------------------------------------------------- */
static void case_hysteresis(void)
{
	printf("case 8: hysteresis widens the settled row, and only that row\n");

	for (int i = 0; i < NROWS - 1; i++) {
		st_ladder_t l;
		int just_out_hi = (int)st_ladder_bands[i].hi + ST_LADDER_HYSTERESIS;
		int just_out_lo = (int)st_ladder_bands[i].lo - ST_LADDER_HYSTERESIS;

		st_ladder_reset(&l);
		feed(&l, meas[i].centre, ST_LADDER_SETTLE_READS);
		feed(&l, just_out_hi, 5);
		expect(&l, (uint8_t)(i + 1), false, meas[i].name);
		feed(&l, just_out_lo, 5);
		expect(&l, (uint8_t)(i + 1), false, meas[i].name);

		/* The same voltage, with a DIFFERENT row settled, is unclaimed. */
		{
			st_ladder_read_t r = st_ladder_classify(
				just_out_hi, 0u, false);

			CHECK(r.cls == ST_LADDER_UNKNOWN,
			      "%s+hyst claimed while unsettled (cls=%d)",
			      meas[i].name, (int)r.cls);
		}
	}
}

/* ---------------------------------------------------------------------- *
 * 9. A new state must earn exactly SETTLE_READS consecutive votes.
 * ---------------------------------------------------------------------- */
static void case_settle_count(void)
{
	printf("case 9: a new state needs exactly %u consecutive in-band reads\n",
	       ST_LADDER_SETTLE_READS);

	st_ladder_t l;

	st_ladder_reset(&l);
	feed(&l, meas[0].centre, ST_LADDER_SETTLE_READS);
	expect(&l, 0x1u, false, "T1");

	for (uint32_t n = 1u; n < ST_LADDER_SETTLE_READS; n++) {
		st_ladder_reset(&l);
		feed(&l, meas[0].centre, ST_LADDER_SETTLE_READS);
		feed(&l, meas[2].centre, (int)n);
		expect(&l, 0x1u, false, "T1 held: too few votes for T1+T2");
	}
	feed(&l, meas[2].centre, 1);
	expect(&l, 0x3u, false, "T1+T2 on the final vote");
}

int main(void)
{
	printf("== st_ladder: the shared AIN0 classifier ==\n");
	printf("stimuli are PHYSICAL MEASUREMENTS from docs/ladder-measured.json\n\n");

	case_table_matches_measurement();
	case_no_aliasing();
	case_every_measured_state_settles();
	case_no_slew_deadlock();
	case_noise_holds_and_still_settles();
	case_adc_error_and_release();
	case_play_never_aliases_a_chord();
	case_hysteresis();
	case_settle_count();

	printf("\n%d checks, %d failures\n", checks, failures);
	if (failures) {
		printf("LADDER SELFTEST: FAIL\n");
		return 1;
	}
	printf("LADDER SELFTEST: PASS\n");
	return 0;
}
