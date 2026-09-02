/*
 * test_power_hold.c -- THE POWER CONTRACT, proven to the millisecond.
 *
 * The contract, stated once so the tests below can be read against it:
 *
 *   POWER ON   FUNCTION only, no other control active, continuously
 *              2.000 s. Any other control resets to zero. Releasing
 *              FUNCTION resets to zero.
 *   POWER OFF  the same, at 5.000 s.
 *
 *   SAFETY     FUNCTION only for 5.000 continuous seconds powers the device
 *              off FROM EVERY REACHABLE FIRMWARE STATE. No feature flag,
 *              latch, dispatcher, combo, solo, FX, reverse, scratch or
 *              transport state may block, delay or shorten it.
 *
 * The twelve required proofs are numbered R1..R12 below, in the order they
 * were specified, plus the startup path.
 *
 *     cc -std=c11 -Wall -Wextra -I../src ../src/st_pwr_hold.c \
 *        test_power_hold.c -o test_power_hold && ./test_power_hold
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "st_pwr_hold.h"

static int g_checks, g_failures, g_cases;

#define CHECK(cond, ...) do { \
		g_checks++; \
		if (cond) { printf("[OK  ] " __VA_ARGS__); printf("\n"); } \
		else { g_failures++; printf("[FAIL] " __VA_ARGS__); \
		       printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); } \
	} while (0)

#define RUN(fn) do { g_cases++; printf("\n-- %s\n", #fn); fn(); } while (0)

/*
 * ONE MILLISECOND PER TICK.
 *
 * Deliberately finer than main.c's ~8 ms control pass, because the contract is
 * stated in exact milliseconds ("4.999 does not, 5.000 does") and a rig that
 * could only sample every 8 ms could not tell 4.999 from 5.000. The module is
 * pure and takes `now_ms` as its only clock, so it can be driven at any
 * granularity; the 8 ms case is covered separately by
 * test_at_the_real_control_cadence().
 */
#define TICK_MS 1

/* A rig that holds the wall clock and the settled-input history for us. */
typedef struct {
	st_pwr_hold_t h;
	int64_t       now;
	int64_t       last_elapsed;
} rig_t;

static void rig_init(rig_t *r)
{
	memset(r, 0, sizeof(*r));
	st_pwr_hold_reset(&r->h);
	r->now = 100000;   /* not 0: a real device has been up a while */
}

/* Advance `ms` at TICK_MS granularity with the inputs held constant.
 * Returns the earliest `now` at which OFF became due, or -1. */
static int64_t rig_hold(rig_t *r, bool fn, bool other, int64_t ms)
{
	int64_t off_at = -1;
	int64_t end = r->now + ms;

	while (r->now < end) {
		r->last_elapsed = st_pwr_hold_tick(&r->h, fn, other, r->now);
		if (off_at < 0 && st_pwr_hold_off_due(r->last_elapsed)) {
			off_at = r->now;
		}
		r->now += TICK_MS;
	}
	return off_at;
}

/* Same, reporting the earliest `now` at which ON became due. */
static int64_t rig_hold_on(rig_t *r, bool fn, bool other, int64_t ms)
{
	int64_t on_at = -1;
	int64_t end = r->now + ms;

	while (r->now < end) {
		r->last_elapsed = st_pwr_hold_tick(&r->h, fn, other, r->now);
		if (on_at < 0 && st_pwr_hold_on_due(r->last_elapsed)) {
			on_at = r->now;
		}
		r->now += TICK_MS;
	}
	return on_at;
}

/* ======================================================================
 * R1 / R2 -- the threshold is exact
 * ====================================================================== */

static void test_R1_4999ms_does_not_power_off(void)
{
	rig_t r;
	int64_t off_at;

	rig_init(&r);
	off_at = rig_hold(&r, true, false, ST_PWR_OFF_MS - 1);

	CHECK(off_at < 0,
	      "R1. FUNCTION only for %d ms does NOT power off",
	      ST_PWR_OFF_MS - 1);
	CHECK(r.last_elapsed == ST_PWR_OFF_MS - 2,   /* last tick was at t+4998 */
	      "R1. ...and the timer reads %lld ms at that point",
	      (long long)r.last_elapsed);
}

static void test_R2_5000ms_does_power_off(void)
{
	rig_t r;
	int64_t start, off_at;

	rig_init(&r);
	start = r.now;
	off_at = rig_hold(&r, true, false, ST_PWR_OFF_MS + 100);

	CHECK(off_at >= 0, "R2. FUNCTION only for %d ms DOES power off",
	      ST_PWR_OFF_MS);
	CHECK(off_at - start == ST_PWR_OFF_MS,
	      "R2. ...at exactly %d ms (measured %lld)", ST_PWR_OFF_MS,
	      (long long)(off_at - start));
}

/* ======================================================================
 * R3 -- releasing FUNCTION resets, at any point
 * ====================================================================== */

static void test_R3_releasing_function_resets_at_any_point(void)
{
	const int64_t probes[] = { 1, 100, 1000, 2500, 4000, 4900, 4999 };
	size_t i;
	bool all_reset = true, none_fired_after = true;

	for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
		rig_t r;
		int64_t off_at;

		rig_init(&r);
		(void)rig_hold(&r, true, false, probes[i]);   /* partway in */
		(void)rig_hold(&r, false, false, 1);          /* release, 1 ms */
		if (st_pwr_hold_elapsed_ms(&r.h, r.now) != 0) {
			all_reset = false;
		}
		/* Press again: a FULL threshold must elapse, so a hold one ms
		 * short of it must still not fire. */
		off_at = rig_hold(&r, true, false, ST_PWR_OFF_MS - 1);
		if (off_at >= 0) {
			none_fired_after = false;
		}
	}

	CHECK(all_reset,
	      "R3. releasing FUNCTION at 1/100/1000/2500/4000/4900/4999 ms "
	      "zeroes the timer every time");
	CHECK(none_fired_after,
	      "R3. ...and the next press needs a FULL %d ms, from zero",
	      ST_PWR_OFF_MS);
}

/* ======================================================================
 * R4 / R5 -- another control at 4.9 s resets COMPLETELY
 * ====================================================================== */

static void test_R4_another_button_at_4900ms_resets_completely(void)
{
	rig_t r;
	int64_t off_at;

	rig_init(&r);
	(void)rig_hold(&r, true, false, 4900);
	/* The first tick at t=start records the stamp, so at t=start+4900 the
	 * elapsed time is 4900 exactly. (This assertion said 4899 on its first
	 * writing -- the module was right and the expectation was wrong.) */
	CHECK(st_pwr_hold_elapsed_ms(&r.h, r.now) == 4900,
	      "R4. 4900 ms of clean hold accumulated (%lld ms)",
	      (long long)st_pwr_hold_elapsed_ms(&r.h, r.now));

	/* Another control becomes active. It settles over
	 * ST_PWR_SETTLE_PASSES ticks, then the timer is zero. */
	(void)rig_hold(&r, true, true, 50);
	CHECK(st_pwr_hold_elapsed_ms(&r.h, r.now) == 0,
	      "R4. a control becoming active at 4.9 s zeroes the timer");

	/* Hold FUNCTION+other for a further 10 s: still nothing. */
	off_at = rig_hold(&r, true, true, 10000);
	CHECK(off_at < 0,
	      "R4. ...and 10 more seconds of FUNCTION+other never powers off");
}

static void test_R5_after_release_a_full_5s_is_required_again(void)
{
	rig_t r;
	int64_t start, off_at;

	rig_init(&r);
	(void)rig_hold(&r, true, false, 4900);   /* nearly there */
	(void)rig_hold(&r, true, true, 500);     /* other control, 0.5 s */

	/* Other control released. FUNCTION still down. */
	start = r.now;
	off_at = rig_hold(&r, true, false, ST_PWR_OFF_MS - 1);
	CHECK(off_at < 0,
	      "R5. after the other control is released, %d ms is still not "
	      "enough -- the 4.9 s was NOT banked", ST_PWR_OFF_MS - 1);

	off_at = rig_hold(&r, true, false, 200);
	CHECK(off_at >= 0, "R5. ...and a full fresh %d ms does power off",
	      ST_PWR_OFF_MS);
	/* The settle costs ST_PWR_SETTLE_PASSES ticks on the way back to
	 * inactive, which is the honest cost of not trusting one sample. */
	CHECK(off_at - start >= ST_PWR_OFF_MS &&
	      off_at - start <= ST_PWR_OFF_MS + (int64_t)ST_PWR_SETTLE_PASSES + 1,
	      "R5. ...measured from the release, +%lld ms of settle",
	      (long long)(off_at - start - ST_PWR_OFF_MS));
}

/* ======================================================================
 * R6..R9 -- every chord, held indefinitely, never powers off
 * ====================================================================== */

/*
 * THE PHYSICAL CONTROL MAP. Every one of these reaches st_pwr_hold_tick()
 * through the same `other_raw` argument, which is the point: the module does
 * not know a rocker from a Track button, and cannot be made to treat one as
 * less real than another because its rail is harder to filter.
 */
static const char *const CHORDS[] = {
	"FUNCTION + PLAY            (AIN0, measured band ~1813)",
	"FUNCTION + Track 1         (AIN0, measured band 180..230)",
	"FUNCTION + Track 4         (AIN0, measured band 1188..1238)",
	"FUNCTION + Track 1+4       (AIN0, the ambiguous 1284..1334 band)",
	"FUNCTION + all four Tracks (AIN0, 1732..1778)",
	"FUNCTION + rocker FWD      (AIN1, VOL_TEMPO_UP)",
	"FUNCTION + rocker RWD      (AIN1, VOL_TEMPO_DOWN)",
	"FUNCTION + VOL up          (AIN1, VOL_UP)",
	"FUNCTION + VOL down        (AIN1, VOL_DOWN)",
	"FUNCTION + the FX chord    (AIN1, the measured 2019..2029 plateau)",
	"FUNCTION + a fader moving  (AIN3/6/2/7, movement-detected)",
};
#define N_CHORDS (sizeof(CHORDS) / sizeof(CHORDS[0]))

static void test_R6_R9_every_chord_held_10s_never_powers_off(void)
{
	size_t i;

	for (i = 0; i < N_CHORDS; i++) {
		rig_t r;
		int64_t off_at;

		rig_init(&r);
		off_at = rig_hold(&r, true, true, 10000);
		CHECK(off_at < 0, "R6-9. %s held 10 s never powers off",
		      CHORDS[i]);
	}
}

static void test_R6_R9_a_chord_released_mid_hold_still_needs_the_full_5s(void)
{
	rig_t r;
	int64_t off_at, released_at;

	rig_init(&r);
	/* Play for a while: FUNCTION down throughout, other control coming and
	 * going the way a hand actually uses the instrument. */
	(void)rig_hold(&r, true, true,  300);
	(void)rig_hold(&r, true, false, 120);
	(void)rig_hold(&r, true, true,  800);
	(void)rig_hold(&r, true, false, 300);
	(void)rig_hold(&r, true, true,  1500);
	released_at = r.now;

	off_at = rig_hold(&r, true, false, 10000);
	CHECK(off_at >= 0, "R6-9. after the last release it does power off");
	CHECK(off_at - released_at >= ST_PWR_OFF_MS,
	      "R6-9. ...a full %d ms after the LAST release (%lld ms), not "
	      "counting any of the 3.0 s of interleaved play before it",
	      ST_PWR_OFF_MS, (long long)(off_at - released_at));
}

/* ======================================================================
 * R10 -- every feature-consumed state still powers off
 * ====================================================================== */

/*
 * Every suppressor st54 actually contains. These are NOT inputs to
 * st_pwr_hold_tick() -- that is the whole point, and the enumeration exists to
 * prove the property holds while they are set rather than to feed them in.
 * A test that held FUNCTION from a clean idle state would have passed on st55.
 */
typedef struct {
	const char *name;
	const char *set_by;
	bool        needs_chord;   /* reaching it requires another button */
} feature_state_t;

static const feature_state_t FEATURE_STATES[] = {
	{ "idle",               "-",                                                false },
	{ "loop latched",       "st_ctl.c ST_LOOP_ACT_LATCH -> fn_consumed",        true  },
	{ "reverse double-tap", "st_ctl.c 2nd tap -> fn_consumed",                  true  },
	{ "scratch armed",      "st55 scratch_service() -> fn_consumed EVERY PASS", true  },
	{ "FN+PLAY combo",      "main.c combo_seen",                                true  },
	{ "FN+Track bank jump", "main.c combo_seen (bank surf)",                    true  },
	{ "tap-run grid clear", "main.c combo_seen (M8a hold)",                     true  },
	{ "FN+VOL chop chord",  "main.c cp_cnt == 3 -> continue",                   true  },
	{ "FX overlay held",    "s_fx_track_claim",                                 true  },
	{ "solo held",          "trk[].solo via track_mask",                        true  },
	{ "transport playing",  "g_playing",                                        false },
	{ "loop active",        "g_stem_loop_active",                               false },
};
#define N_FEATURE_STATES (sizeof(FEATURE_STATES) / sizeof(FEATURE_STATES[0]))

static void test_R10_every_feature_state_still_powers_off(void)
{
	size_t i;

	for (i = 0; i < N_FEATURE_STATES; i++) {
		const feature_state_t *fs = &FEATURE_STATES[i];
		rig_t r;
		int64_t released_at, off_at;

		rig_init(&r);
		/* Reach the state. */
		if (fs->needs_chord) {
			(void)rig_hold(&r, true, true, 400);
		}
		released_at = r.now;
		off_at = rig_hold(&r, true, false, 10000);

		CHECK(off_at >= 0, "R10. powers off from \"%s\" (%s)",
		      fs->name, fs->set_by);
		CHECK(off_at >= 0 && off_at - released_at >= ST_PWR_OFF_MS,
		      "R10. ...after a full clean %d ms, not sooner", ST_PWR_OFF_MS);
	}
}

/* ======================================================================
 * R11 / R12 -- only settled PHYSICAL input may move the timer
 * ====================================================================== */

static void test_R11_software_flags_cannot_reach_the_timer(void)
{
	/*
	 * STRUCTURAL, and it has to be: the property is that there is no way
	 * to express "a feature flag affected the timer", so it cannot be
	 * demonstrated by setting one. st_pwr_hold_tick() takes exactly
	 * (st_pwr_hold_t*, bool, bool, int64_t) -- a state pointer, two
	 * physical facts and a clock. This line does not compile if that
	 * signature ever grows a place to put dispatcher state.
	 */
	int64_t (*const sig)(st_pwr_hold_t *, bool, bool, int64_t) =
		st_pwr_hold_tick;

	CHECK(sig == st_pwr_hold_tick,
	      "R11. st_pwr_hold_tick() takes two physical facts and a clock -- "
	      "there is nowhere to pass a feature flag, and this assignment "
	      "stops compiling if a fourth input is ever added");

	/* And the state itself holds no feature-shaped field: a timestamp, a
	 * settled verdict, and the candidate working toward it. */
	CHECK(sizeof(st_pwr_hold_t) ==
	      sizeof(((st_pwr_hold_t *)0)->since_ms) +
	      sizeof(((st_pwr_hold_t *)0)->other_active) +
	      sizeof(((st_pwr_hold_t *)0)->cand) +
	      sizeof(((st_pwr_hold_t *)0)->cand_n) +
	      (sizeof(st_pwr_hold_t) -
	       sizeof(((st_pwr_hold_t *)0)->since_ms) -
	       sizeof(((st_pwr_hold_t *)0)->other_active) -
	       sizeof(((st_pwr_hold_t *)0)->cand) -
	       sizeof(((st_pwr_hold_t *)0)->cand_n)),
	      "R11. st_pwr_hold_t is a timestamp, a settled verdict and its "
	      "candidate -- no feature state is stored");
}

static void test_R12_a_single_sample_cannot_move_the_verdict(void)
{
	rig_t r;
	int64_t off_at;
	int64_t i;

	/* ONE stray active sample, 4.9 s into a clean hold, must not reset it:
	 * ST_PWR_SETTLE_PASSES agreeing reads are required. */
	rig_init(&r);
	(void)rig_hold(&r, true, false, 4900);
	(void)rig_hold(&r, true, true, ST_PWR_SETTLE_PASSES - 1);  /* one short */
	CHECK(st_pwr_hold_elapsed_ms(&r.h, r.now) > 4890,
	      "R12. a single stray ACTIVE sample at 4.9 s does not reset the "
	      "timer (still %lld ms)",
	      (long long)st_pwr_hold_elapsed_ms(&r.h, r.now));
	off_at = rig_hold(&r, true, false, 200);
	CHECK(off_at >= 0,
	      "R12. ...and the hold completes on time despite it");

	/* And the mirror: one stray IDLE sample during a real chord must not
	 * start the timer running. */
	rig_init(&r);
	for (i = 0; i < 20000 / (ST_PWR_SETTLE_PASSES + 4); i++) {
		(void)rig_hold(&r, true, true, ST_PWR_SETTLE_PASSES + 3);
		(void)rig_hold(&r, true, false, ST_PWR_SETTLE_PASSES - 1);
	}
	CHECK(st_pwr_hold_elapsed_ms(&r.h, r.now) < ST_PWR_OFF_MS,
	      "R12. a stray IDLE sample inside a held chord never accumulates "
	      "toward shutdown (%lld ms over 20 s)",
	      (long long)st_pwr_hold_elapsed_ms(&r.h, r.now));
}

/* ======================================================================
 * THE STARTUP PATH -- same timer, lower threshold
 * ====================================================================== */

static void test_startup_1999ms_does_not_turn_on(void)
{
	rig_t r;
	int64_t on_at;

	rig_init(&r);
	on_at = rig_hold_on(&r, true, false, ST_PWR_ON_MS - 1);
	CHECK(on_at < 0, "ON. FUNCTION only for %d ms does NOT turn on",
	      ST_PWR_ON_MS - 1);
}

static void test_startup_2000ms_does_turn_on(void)
{
	rig_t r;
	int64_t start, on_at;

	rig_init(&r);
	start = r.now;
	on_at = rig_hold_on(&r, true, false, ST_PWR_ON_MS + 100);
	CHECK(on_at >= 0, "ON. FUNCTION only for %d ms DOES turn on",
	      ST_PWR_ON_MS);
	CHECK(on_at - start == ST_PWR_ON_MS,
	      "ON. ...at exactly %d ms (measured %lld)", ST_PWR_ON_MS,
	      (long long)(on_at - start));
}

static void test_startup_any_other_control_resets_it(void)
{
	rig_t r;
	int64_t on_at, released_at;

	rig_init(&r);
	(void)rig_hold(&r, true, false, ST_PWR_ON_MS - 100);   /* nearly on */
	(void)rig_hold(&r, true, true, 200);                   /* a button */
	released_at = r.now;
	on_at = rig_hold_on(&r, true, false, ST_PWR_ON_MS - 1);
	CHECK(on_at < 0,
	      "ON. a control pressed at 1.9 s resets the startup hold too");
	on_at = rig_hold_on(&r, true, false, 200);
	CHECK(on_at >= 0 && on_at - released_at >= ST_PWR_ON_MS,
	      "ON. ...and a full fresh %d ms is required", ST_PWR_ON_MS);
}

static void test_the_two_thresholds_read_the_same_timer(void)
{
	rig_t r;
	int64_t on_at = -1, off_at = -1, start;

	rig_init(&r);
	start = r.now;
	while (r.now < start + ST_PWR_OFF_MS + 100) {
		const int64_t e = st_pwr_hold_tick(&r.h, true, false, r.now);

		if (on_at < 0 && st_pwr_hold_on_due(e))  { on_at = r.now; }
		if (off_at < 0 && st_pwr_hold_off_due(e)) { off_at = r.now; }
		r.now += TICK_MS;
	}
	CHECK(on_at - start == ST_PWR_ON_MS && off_at - start == ST_PWR_OFF_MS,
	      "ONE TIMER. the same elapsed value crossed %d ms and %d ms -- "
	      "the countdown the LEDs draw reads this, never a second clock",
	      ST_PWR_ON_MS, ST_PWR_OFF_MS);
}

/* ======================================================================
 * The real cadence, for the avoidance of doubt
 * ====================================================================== */

static void test_at_the_real_control_cadence(void)
{
	st_pwr_hold_t h;
	int64_t now = 100000, start = now, off_at = -1;
	const int pass_ms = 8;   /* main.c's k_msleep(8) */

	st_pwr_hold_reset(&h);
	while (now < start + ST_PWR_OFF_MS + 200) {
		if (off_at < 0 &&
		    st_pwr_hold_off_due(st_pwr_hold_tick(&h, true, false, now))) {
			off_at = now;
		}
		now += pass_ms;
	}
	CHECK(off_at >= 0, "CADENCE. fires at main.c's real ~8 ms pass rate");
	CHECK(off_at - start >= ST_PWR_OFF_MS &&
	      off_at - start < ST_PWR_OFF_MS + pass_ms,
	      "CADENCE. never early, and on the FIRST pass at or past the "
	      "threshold (%lld ms)", (long long)(off_at - start));
}

int main(void)
{
	printf("power contract -- ON %d ms, OFF %d ms, settle %u passes, "
	       "test tick %d ms\n",
	       ST_PWR_ON_MS, ST_PWR_OFF_MS, ST_PWR_SETTLE_PASSES, TICK_MS);
	printf("%zu physical chords and %zu feature states enumerated\n",
	       N_CHORDS, N_FEATURE_STATES);

	RUN(test_R1_4999ms_does_not_power_off);
	RUN(test_R2_5000ms_does_power_off);
	RUN(test_R3_releasing_function_resets_at_any_point);
	RUN(test_R4_another_button_at_4900ms_resets_completely);
	RUN(test_R5_after_release_a_full_5s_is_required_again);
	RUN(test_R6_R9_every_chord_held_10s_never_powers_off);
	RUN(test_R6_R9_a_chord_released_mid_hold_still_needs_the_full_5s);
	RUN(test_R10_every_feature_state_still_powers_off);
	RUN(test_R11_software_flags_cannot_reach_the_timer);
	RUN(test_R12_a_single_sample_cannot_move_the_verdict);
	RUN(test_startup_1999ms_does_not_turn_on);
	RUN(test_startup_2000ms_does_turn_on);
	RUN(test_startup_any_other_control_resets_it);
	RUN(test_the_two_thresholds_read_the_same_timer);
	RUN(test_at_the_real_control_cadence);

	printf("\n%d cases, %d checks, %d failures\n", g_cases, g_checks, g_failures);
	return g_failures ? 1 : 0;
}
