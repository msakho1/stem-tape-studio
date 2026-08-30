/*
 * test_vol_ladder.c -- the AIN1 Volume ladder decode, host-tested against the
 * PHYSICAL MEASUREMENT rather than against a model.
 *
 * Every stimulus below is a real number captured from the user's SP-1 with
 * build st20-VOLCAL (docs/ain1-measured.json). This exercises the SAME
 * st_vol_decode() the firmware's control loop calls -- the function lives in
 * a header precisely so that is possible -- so a hand-edited threshold fails
 * the build instead of silently breaking FX entry on hardware.
 *
 * WHY THIS FILE EXISTS. Before the measurement, decode's top band was
 * "anything >= 1500", so Volume+ (1821..1830) and the two-button chord
 * (2019..2029) both returned VOL_UP and were indistinguishable. That single
 * ambiguity was the entire blocker on FX mode. These assertions are what keep
 * it from coming back.
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -Werror -I../src test_vol_ladder.c \
 *      -o test_vol_ladder && ./test_vol_ladder
 */

#include <stdio.h>

#include "st_vol_ladder.h"

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
 * THE MEASUREMENT. docs/ain1-measured.json, transcribed once.
 * lo/hi are the extreme SETTLED values observed while holding that state.
 * ---------------------------------------------------------------------- */
typedef struct {
	int lo, hi;
	enum vol_btn expect;
	const char *name;
} meas_t;

static const meas_t meas[] = {
	{    1,   10, VOL_NONE, "idle"          },
	{  732,  742, VOL_DOWN, "Volume-"       },
	{ 1821, 1830, VOL_UP,   "Volume+"       },
	{ 2019, 2029, VOL_BOTH, "BOTH together" },
};
#define NMEAS ((int)(sizeof(meas) / sizeof(meas[0])))

static const char *nm(enum vol_btn b)
{
	switch (b) {
	case VOL_NONE:       return "VOL_NONE";
	case VOL_TEMPO_DOWN: return "VOL_TEMPO_DOWN";
	case VOL_DOWN:       return "VOL_DOWN";
	case VOL_TEMPO_UP:   return "VOL_TEMPO_UP";
	case VOL_UP:         return "VOL_UP";
	case VOL_BOTH:       return "VOL_BOTH";
	}
	return "?";
}

/* 1. EVERY measured value decodes to the state that produced it. Not the
 *    centre or the endpoints -- every integer in each observed span. */
static void t_measured_spans(void)
{
	printf("measured spans decode correctly\n");
	for (int i = 0; i < NMEAS; i++) {
		for (int v = meas[i].lo; v <= meas[i].hi; v++) {
			enum vol_btn got = st_vol_decode(v);
			CHECK(got == meas[i].expect,
			      "%s: raw=%d decoded %s, expected %s",
			      meas[i].name, v, nm(got), nm(meas[i].expect));
		}
	}
}

/* 2. THE DEFECT THIS MEASUREMENT FIXED. Volume+ and the chord must decode
 *    differently. This is the assertion that would have failed before the
 *    capture, when both returned VOL_UP. */
static void t_chord_distinct_from_volume_plus(void)
{
	printf("Volume+ and the chord are distinguishable\n");
	enum vol_btn plus  = st_vol_decode(1825); /* measured centre */
	enum vol_btn chord = st_vol_decode(2024); /* measured centre */
	CHECK(plus != chord, "Volume+ and the chord both decode as %s", nm(plus));
	CHECK(plus == VOL_UP, "Volume+ centre decoded %s", nm(plus));
	CHECK(chord == VOL_BOTH, "chord centre decoded %s", nm(chord));
}

/* 3. THE GUARD GAP. Nothing was ever observed between 1830 and 2019. The
 *    threshold sits at its midpoint, so the whole gap must resolve cleanly to
 *    one side or the other with the split exactly where the header claims --
 *    and both margins must actually be the 95 counts documented. */
static void t_guard_gap(void)
{
	printf("the 1830..2019 guard gap splits at ST_VOL_CHORD_MIN\n");
	for (int v = 1831; v < ST_VOL_CHORD_MIN; v++)
		CHECK(st_vol_decode(v) == VOL_UP,
		      "gap raw=%d below the split decoded %s",
		      v, nm(st_vol_decode(v)));
	for (int v = ST_VOL_CHORD_MIN; v <= 2018; v++)
		CHECK(st_vol_decode(v) == VOL_BOTH,
		      "gap raw=%d at/above the split decoded %s",
		      v, nm(st_vol_decode(v)));

	CHECK(ST_VOL_CHORD_MIN - 1830 == 95,
	      "margin above Volume+ is %d, expected 95", ST_VOL_CHORD_MIN - 1830);
	CHECK(2019 - ST_VOL_CHORD_MIN + 1 == 95,
	      "margin below the chord is %d, expected 95",
	      2019 - ST_VOL_CHORD_MIN + 1);
	CHECK(ST_VOL_CHORD_RAW >= 2019 && ST_VOL_CHORD_RAW <= 2029,
	      "ST_VOL_CHORD_RAW %d is outside the measured chord plateau",
	      ST_VOL_CHORD_RAW);
}

/* 4. THE TWO ENTRY POINTS AGREE EVERYWHERE. The FX overlay asks
 *    st_vol_is_chord(); everything else reads the enum. If they ever
 *    disagreed, FX entry and volume handling would see different worlds.
 *    Checked across the entire 12-bit range, not at sampled points. */
static void t_predicate_agrees_with_decode(void)
{
	printf("st_vol_is_chord() agrees with st_vol_decode() on all 4096 inputs\n");
	int mismatches = 0;
	for (int v = 0; v < 4096; v++)
		if (st_vol_is_chord(v) != (st_vol_decode(v) == VOL_BOTH))
			mismatches++;
	CHECK(mismatches == 0,
	      "%d raw values where the predicate and the decode disagree",
	      mismatches);
}

/* 5. NO MEASURED STATE COLLIDES WITH ANOTHER, and the ordering the
 *    parallel-resistor model predicts actually holds: the chord reads higher
 *    than either single button. That is the physical claim the whole
 *    calibration rested on. */
static void t_states_ordered_and_disjoint(void)
{
	printf("measured states are disjoint and ordered as the ladder predicts\n");
	for (int i = 1; i < NMEAS; i++)
		CHECK(meas[i].lo > meas[i - 1].hi,
		      "%s (lo=%d) overlaps %s (hi=%d)",
		      meas[i].name, meas[i].lo,
		      meas[i - 1].name, meas[i - 1].hi);

	/* chord above Volume+ above Volume- above idle */
	CHECK(meas[3].lo > meas[2].hi, "chord does not read above Volume+");
	CHECK(meas[2].lo > meas[1].hi, "Volume+ does not read above Volume-");
}

/* 6. THE UNMEASURED BANDS ARE NOT ASSERTED AS CORRECT -- only that they do
 *    not swallow anything that WAS measured. FWD/RWD were never pressed, so
 *    claiming their centres are right would be inventing evidence. What can
 *    honestly be checked is that no measured plateau lands inside them. */
static void t_unverified_tempo_bands_do_not_shadow_measurements(void)
{
	printf("unverified tempo bands do not swallow any measured state\n");
	for (int i = 0; i < NMEAS; i++) {
		for (int v = meas[i].lo; v <= meas[i].hi; v++) {
			enum vol_btn got = st_vol_decode(v);
			CHECK(got != VOL_TEMPO_DOWN && got != VOL_TEMPO_UP,
			      "%s raw=%d fell into an unverified tempo band (%s)",
			      meas[i].name, v, nm(got));
		}
	}
}

int main(void)
{
	printf("=== AIN1 volume ladder (measured, st20-VOLCAL) ===\n");
	t_measured_spans();
	t_chord_distinct_from_volume_plus();
	t_guard_gap();
	t_predicate_agrees_with_decode();
	t_states_ordered_and_disjoint();
	t_unverified_tempo_bands_do_not_shadow_measurements();

	printf("\n%d checks, %d failures\n", checks, failures);
	if (failures) {
		printf("FAILED\n");
		return 1;
	}
	printf("OK\n");
	return 0;
}
