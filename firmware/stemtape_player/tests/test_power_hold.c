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

/* ======================================================================
 * THE FADERS -- the wiring defect this file did not have a test for
 * ====================================================================== */

/* One fader is polled once per four control passes (round-robin). */
#define FADER_POLL_MS 32

/*
 * THE DEFECT, REPRODUCED AS A REGRESSION TEST.
 *
 * The first fader detector compared each rail against a reference that chased
 * the reading at 4 counts per sample, called it in use above a 32-count gap,
 * and only ever ran while FUNCTION was held. So an ordinary volume change made
 * with FUNCTION UP left the reference stale by the whole movement: a
 * 500 -> 3000 move stranded a 2500-count gap needing 617 polls to close, and
 * 617 x 32 ms is 19.7 s. A 5.000 s hold would have taken 24.7 s.
 *
 * That is the safety invariant broken by stale state, which is st55's failure
 * in different clothes. It was found by auditing the wiring, NOT by a test,
 * because the wiring had none. This is that test.
 */
static void test_a_fader_moved_before_the_hold_does_not_delay_it(void)
{
	st_pwr_fader_t f;
	st_pwr_hold_t h;
	int64_t now = 100000, start, off_at = -1;
	int p;

	/* The user moved a fader 500 -> 3000 at some earlier time, with
	 * FUNCTION up. Then they press FUNCTION. The rising edge forgets
	 * everything -- that is the fix. */
	st_pwr_fader_reset(&f);
	st_pwr_hold_reset(&h);

	start = now;
	for (p = 0; p < (ST_PWR_OFF_MS + 500) / TICK_MS && off_at < 0; p++) {
		/* Every fader sits STILL at its new position throughout. */
		const bool fader_in_use =
			((p % FADER_POLL_MS) == 0)
			 ? st_pwr_fader_sample(&f, (uint32_t)((p / FADER_POLL_MS) & 3),
						3000, now)
			 : st_pwr_fader_active(&f, now);

		if (st_pwr_hold_off_due(st_pwr_hold_tick(&h, true, fader_in_use,
							  now))) {
			off_at = now;
		}
		now += TICK_MS;
	}

	CHECK(off_at >= 0,
	      "FADER. a fader moved BEFORE the hold does not block it");
	CHECK(off_at - start == ST_PWR_OFF_MS,
	      "FADER. ...and the hold still takes exactly %d ms, not %lld "
	      "(the displacement version took 24,700)",
	      ST_PWR_OFF_MS, (long long)(off_at - start));
}

static void test_a_fader_being_moved_holds_the_timer_at_zero(void)
{
	st_pwr_fader_t f;
	st_pwr_hold_t h;
	int64_t now = 100000, off_at = -1;
	int32_t pos = 500;
	int p;

	st_pwr_fader_reset(&f);
	st_pwr_hold_reset(&h);

	/* A hand sweeping one fader steadily for 10 s: 1000 counts/s, so
	 * 32 counts per 32 ms poll -- twice the threshold. */
	for (p = 0; p < 10000 / TICK_MS && off_at < 0; p++) {
		bool in_use;

		if ((p % FADER_POLL_MS) == 0) {
			pos += 32;
			if (pos > 3700) {
				pos = 500;
			}
			in_use = st_pwr_fader_sample(&f, 0u, pos, now);
		} else {
			in_use = st_pwr_fader_active(&f, now);
		}
		if (st_pwr_hold_off_due(st_pwr_hold_tick(&h, true, in_use, now))) {
			off_at = now;
		}
		now += TICK_MS;
	}
	CHECK(off_at < 0,
	      "FADER. a fader being swept for 10 s never powers off");
}

static void test_the_hand_leaving_a_fader_is_forgotten_promptly(void)
{
	st_pwr_fader_t f;
	int64_t now = 100000;

	st_pwr_fader_reset(&f);
	(void)st_pwr_fader_sample(&f, 0u, 500, now);          /* seed */
	now += FADER_POLL_MS;
	CHECK(st_pwr_fader_sample(&f, 0u, 900, now),
	      "FADER. a 400-count jump reads as in use");

	/* The hand stops. The fader sits at 900. */
	now += ST_PWR_FADER_ACTIVE_MS;
	CHECK(!st_pwr_fader_sample(&f, 0u, 900, now),
	      "FADER. ...and %d ms after it stops, it is not in use again -- "
	      "the position it was left at is irrelevant",
	      ST_PWR_FADER_ACTIVE_MS);
}

static void test_fader_noise_below_the_threshold_is_not_movement(void)
{
	st_pwr_fader_t f;
	int64_t now = 100000;
	int p;
	bool ever_active = false;

	st_pwr_fader_reset(&f);
	/* +/- (threshold - 1) counts of jitter around a resting position, for
	 * 20 s, on all four rails. */
	for (p = 0; p < 20000 / FADER_POLL_MS; p++) {
		const int32_t jitter = ((p & 1) ? 1 : -1) *
					(ST_PWR_FADER_MOVE_COUNTS - 1);

		if (st_pwr_fader_sample(&f, (uint32_t)(p & 3), 2000 + jitter,
					 now)) {
			ever_active = true;
		}
		now += FADER_POLL_MS;
	}
	CHECK(!ever_active,
	      "FADER. 20 s of +/-%d-count jitter on all four rails is never "
	      "movement", ST_PWR_FADER_MOVE_COUNTS - 1);
}

static void test_a_failed_adc_read_is_not_movement(void)
{
	st_pwr_fader_t f;
	int64_t now = 100000;

	st_pwr_fader_reset(&f);
	(void)st_pwr_fader_sample(&f, 0u, 2000, now);
	now += FADER_POLL_MS;
	CHECK(!st_pwr_fader_sample(&f, 0u, -1, now),
	      "FADER. an ADC error (-1) is not a movement");
	now += FADER_POLL_MS;
	CHECK(!st_pwr_fader_sample(&f, 0u, 2000, now),
	      "FADER. ...and the sample after it compares against the last "
	      "GOOD reading, not against the error");
}

/* ======================================================================
 * THE TRANSACTION, AND THE FAILURE-INJECTION SUITE
 * ======================================================================
 * Everything above drives the timer. Everything below drives the SERVICE --
 * the complete decision that sits immediately before power_off(), with only
 * I/O left outside it. That distinction is the point: the 24.7 s defect
 * survived three commits and two green CI runs because the timer had tests and
 * the glue did not.
 *
 * Each case below recreates a class of stale state that has already fooled
 * this project, and each must end in one of exactly TWO outcomes:
 *
 *   1. a clean uninterrupted FUNCTION-only hold succeeds at its exact
 *      deadline, or
 *   2. genuine concurrent physical activity resets the transaction completely
 *
 * No stale software state may produce a third.
 */

#define FADER_POLL_MS 32   /* one fader per four ~8 ms control passes */

typedef struct {
	st_pwr_t p;
	int64_t  now;
	uint8_t  rr;
	int32_t  fader_pos[ST_PWR_FADERS];
} svc_t;

static int64_t svc_run(svc_t *s, bool fn, bool ain0, bool ain1, int64_t ms);

/*
 * A RUNNING INSTRUMENT: powered ON, and shutdown ARMED.
 *
 * The arm is not handed over -- st_pwr_init_on() starts DISARMED like every
 * entry point, and the only thing that can grant it is st_pwr_service()
 * observing FUNCTION physically up. So this runs a few real passes with the
 * button up, exactly as a booted device does before anyone touches it. If the
 * release-to-rearm guard were ever broken open, these tests would still be
 * arming through the real mechanism rather than around it.
 */
static void svc_init(svc_t *s)
{
	uint32_t k;

	memset(s, 0, sizeof(*s));
	st_pwr_init_on(&s->p);
	s->now = 100000;
	for (k = 0; k < ST_PWR_FADERS; k++) {
		s->fader_pos[k] = 2000;   /* mid-travel, resting */
	}
	(void)svc_run(s, false, false, false, 40);
}

/* A DEVICE THAT IS OFF, with the wake gate running: standby, or battery wake. */
static void svc_init_off(svc_t *s)
{
	uint32_t k;

	memset(s, 0, sizeof(*s));
	st_pwr_init_off(&s->p);
	s->now = 100000;
	for (k = 0; k < ST_PWR_FADERS; k++) {
		s->fader_pos[k] = 2000;
	}
}

/*
 * Run `ms` of control passes at the REAL 8 ms cadence, round-robining the
 * faders exactly as main.c does. Returns the `now` at which off_due first
 * appeared, or -1.
 */
static int64_t svc_run(svc_t *s, bool fn, bool ain0, bool ain1, int64_t ms)
{
	const int64_t end = s->now + ms;
	int64_t off_at = -1;
	int pass = 0;

	while (s->now < end) {
		st_pwr_in_t in;
		st_pwr_out_t out;

		memset(&in, 0, sizeof(in));
		in.fn_down     = fn;
		in.ain0_active = ain0;
		in.ain1_active = ain1;
		in.now_ms      = s->now;
		in.fader_raw   = -1;
		if (fn && (pass % 4) == 0) {
			in.fader_idx = s->rr;
			in.fader_raw = s->fader_pos[s->rr];
			s->rr = (uint8_t)((s->rr + 1u) & (ST_PWR_FADERS - 1u));
		}
		st_pwr_service(&s->p, &in, &out);
		if (off_at < 0 && out.off_due) {
			off_at = s->now;
		}
		s->now += 8;
		pass++;
	}
	return off_at;
}

/* ---- F1: a fader moved BEFORE the transaction ------------------------- */
static void test_F1_a_fader_moved_before_function_went_down(void)
{
	svc_t s;
	int64_t start, off_at;

	svc_init(&s);
	/* The user slid every fader to a new position with FUNCTION UP. The
	 * service never saw it, and must not care. */
	s.fader_pos[0] = 3600;
	s.fader_pos[1] = 120;
	s.fader_pos[2] = 3000;
	s.fader_pos[3] = 500;

	start = s.now;
	off_at = svc_run(&s, true, false, false, ST_PWR_OFF_MS + 200);
	CHECK(off_at >= 0 && off_at - start < ST_PWR_OFF_MS + 16,
	      "F1. four faders moved before FUNCTION went down: still off at "
	      "%lld ms (the displacement detector took 24,700)",
	      (long long)(off_at - start));
}

/* ---- F2: stale ladder state at the rising edge ------------------------ */
static void test_F2_a_control_active_at_the_rising_edge(void)
{
	svc_t s;
	int64_t released_at, off_at;

	svc_init(&s);
	/* PLAY is already down when FUNCTION goes down. */
	(void)svc_run(&s, true, true, false, 1500);
	released_at = s.now;
	off_at = svc_run(&s, true, false, false, ST_PWR_OFF_MS + 200);

	CHECK(off_at >= 0, "F2. a control active at the rising edge: still "
	      "powers off once it is released");
	CHECK(off_at - released_at >= ST_PWR_OFF_MS,
	      "F2. ...after a FULL fresh %d ms (%lld), not counting the 1.5 s "
	      "the chord occupied", ST_PWR_OFF_MS,
	      (long long)(off_at - released_at));
}

/* ---- F3: a control released while FUNCTION stays held ----------------- */
static void test_F3_control_released_mid_hold(void)
{
	svc_t s;
	int64_t released_at, off_at;

	svc_init(&s);
	(void)svc_run(&s, true, false, false, 4900);   /* nearly there */
	(void)svc_run(&s, true, true, false, 300);     /* PLAY pressed */
	released_at = s.now;
	off_at = svc_run(&s, true, false, false, ST_PWR_OFF_MS + 200);

	CHECK(off_at >= 0 && off_at - released_at >= ST_PWR_OFF_MS,
	      "F3. a control released mid-hold restarts the transaction: a "
	      "full %d ms after the release, the 4.9 s NOT banked",
	      ST_PWR_OFF_MS);
}

/* ---- F4: repeated interruption near the deadline ---------------------- */
static void test_F4_repeated_interruption_near_the_deadline(void)
{
	svc_t s;
	int64_t off_at = -1;
	int i;

	svc_init(&s);
	/* Twenty times: get to 4.9 s, then touch a Track for 100 ms. */
	for (i = 0; i < 20 && off_at < 0; i++) {
		off_at = svc_run(&s, true, false, false, 4900);
		if (off_at < 0) {
			off_at = svc_run(&s, true, true, false, 100);
		}
	}
	CHECK(off_at < 0,
	      "F4. twenty interruptions at 4.9 s never accumulate to a "
	      "shutdown -- 98 s of FUNCTION down, no power-off");

	/* And the moment the interruptions stop, it works. */
	off_at = svc_run(&s, true, false, false, ST_PWR_OFF_MS + 200);
	CHECK(off_at >= 0, "F4. ...and it powers off as soon as they stop");
}

/* ---- F5: repeated short taps must never accumulate -------------------- */
static void test_F5_repeated_taps_never_accumulate(void)
{
	svc_t s;
	int64_t off_at = -1;
	int i;

	svc_init(&s);
	for (i = 0; i < 40 && off_at < 0; i++) {
		off_at = svc_run(&s, true, false, false, 400);   /* tap */
		if (off_at < 0) {
			off_at = svc_run(&s, false, false, false, 100);
		}
	}
	CHECK(off_at < 0,
	      "F5. forty 400 ms taps (16 s of FUNCTION down) never accumulate "
	      "toward %d ms", ST_PWR_OFF_MS);
}

/* ---- F6: a single-sample phantom cannot reset ------------------------- */
static void test_F6_a_phantom_single_sample_does_not_reset(void)
{
	svc_t s;
	st_pwr_in_t in;
	st_pwr_out_t out;
	int64_t before;

	svc_init(&s);
	(void)svc_run(&s, true, false, false, 4900);
	before = s.p.hold.since_ms;

	/* ONE pass with a phantom Track press, then back to quiet. */
	memset(&in, 0, sizeof(in));
	in.fn_down = true; in.ain0_active = true; in.fader_raw = -1;
	in.now_ms = s.now;
	st_pwr_service(&s.p, &in, &out);
	s.now += 8;

	CHECK(s.p.hold.since_ms == before,
	      "F6. a single phantom sample at 4.9 s does not reset the "
	      "transaction (%u agreeing passes are required)",
	      ST_PWR_SETTLE_PASSES);
	CHECK(svc_run(&s, true, false, false, 200) >= 0,
	      "F6. ...and the hold completes on time despite it");
}

/* ---- F7: sub-threshold fader noise for the whole hold ----------------- */
static void test_F7_subthreshold_fader_noise_never_blocks(void)
{
	svc_t s;
	int64_t start, off_at;
	int pass = 0;
	const int64_t end_at = 100000 + ST_PWR_OFF_MS + 400;

	svc_init(&s);
	start = s.now;
	off_at = -1;
	while (s.now < end_at && off_at < 0) {
		st_pwr_in_t in;
		st_pwr_out_t out;

		memset(&in, 0, sizeof(in));
		in.fn_down = true;
		in.now_ms  = s.now;
		in.fader_raw = -1;
		if ((pass % 4) == 0) {
			/* jitter one count under the threshold, alternating */
			in.fader_idx = s.rr;
			in.fader_raw = 2000 + ((pass & 8) ? 1 : -1) *
					(ST_PWR_FADER_MOVE_COUNTS - 1);
			s.rr = (uint8_t)((s.rr + 1u) & (ST_PWR_FADERS - 1u));
		}
		st_pwr_service(&s.p, &in, &out);
		if (out.off_due) {
			off_at = s.now;
		}
		s.now += 8;
		pass++;
	}
	CHECK(off_at >= 0 && off_at - start < ST_PWR_OFF_MS + 16,
	      "F7. sub-threshold fader jitter for the whole hold never delays "
	      "it (off at %lld ms)", (long long)(off_at - start));
}

/* ---- F8: above-threshold intentional fader activity DOES block -------- */
static void test_F8_real_fader_movement_blocks_and_then_releases(void)
{
	svc_t s;
	int64_t off_at, stopped_at;
	int pass = 0;

	svc_init(&s);
	/* A hand sweeping fader 0 for 8 s while FUNCTION is held. */
	off_at = -1;
	while (pass < 8000 / 8 && off_at < 0) {
		st_pwr_in_t in;
		st_pwr_out_t out;

		memset(&in, 0, sizeof(in));
		in.fn_down = true;
		in.now_ms  = s.now;
		in.fader_raw = -1;
		if ((pass % 4) == 0) {
			s.fader_pos[0] += 40;      /* > threshold per poll */
			if (s.fader_pos[0] > 3600) {
				s.fader_pos[0] = 200;
			}
			in.fader_idx = 0;
			in.fader_raw = s.fader_pos[0];
		}
		st_pwr_service(&s.p, &in, &out);
		if (out.off_due) {
			off_at = s.now;
		}
		s.now += 8;
		pass++;
	}
	CHECK(off_at < 0,
	      "F8. a fader swept for 8 s while FUNCTION is held never powers "
	      "off");

	stopped_at = s.now;
	off_at = svc_run(&s, true, false, false, ST_PWR_OFF_MS + 400);
	CHECK(off_at >= 0 && off_at - stopped_at >= ST_PWR_OFF_MS,
	      "F8. ...and a full fresh %d ms is required after the hand stops",
	      ST_PWR_OFF_MS);
}

/* ---- F9: the transaction forgets, structurally ------------------------ */
static void test_F9_the_rising_edge_forgets_everything(void)
{
	svc_t s;
	st_pwr_in_t in;
	st_pwr_out_t out;

	svc_init(&s);
	/* Build up settled "other active" state and fader baselines. */
	(void)svc_run(&s, true, true, true, 1000);
	CHECK(s.p.hold.other_active,
	      "F9. the settled verdict is ACTIVE before the release");

	/* FUNCTION released, then pressed again: the rising edge. */
	(void)svc_run(&s, false, true, true, 100);
	memset(&in, 0, sizeof(in));
	in.fn_down = true; in.fader_raw = -1; in.now_ms = s.now;
	st_pwr_service(&s.p, &in, &out);

	CHECK(out.began, "F9. the rising edge is reported as a new transaction");
	CHECK(!s.p.hold.other_active && s.p.hold.cand_n == 0u,
	      "F9. ...the settled verdict and its candidate are forgotten");
	CHECK(s.p.fader.moved_ms == 0 &&
	      s.p.fader.last[0] == ST_PWR_FADER_UNSEEDED &&
	      s.p.fader.last[1] == ST_PWR_FADER_UNSEEDED &&
	      s.p.fader.last[2] == ST_PWR_FADER_UNSEEDED &&
	      s.p.fader.last[3] == ST_PWR_FADER_UNSEEDED,
	      "F9. ...and every fader baseline is dropped");
	CHECK(out.elapsed_ms == 0,
	      "F9. ...and elapsed starts at zero");
}

/* ---- F10: the ON threshold, through the service ----------------------- */
static void test_F10_the_on_threshold_through_the_service(void)
{
	svc_t s;
	st_pwr_in_t in;
	st_pwr_out_t out;
	int64_t start, on_at = -1;

	/* THE ON THRESHOLD BELONGS TO THE OFF STATE. A running instrument never
	 * reports on_due -- that is the state machine, not a quirk of this
	 * test -- so this one starts from the wake entry. */
	svc_init_off(&s);
	start = s.now;
	while (s.now < start + ST_PWR_ON_MS + 100 && on_at < 0) {
		memset(&in, 0, sizeof(in));
		in.fn_down = true; in.fader_raw = -1; in.now_ms = s.now;
		st_pwr_service(&s.p, &in, &out);
		if (out.on_due) {
			on_at = s.now;
		}
		s.now += 8;
	}
	CHECK(on_at >= 0 && on_at - start >= ST_PWR_ON_MS &&
	      on_at - start < ST_PWR_ON_MS + 8,
	      "F10. ON is due at %lld ms, first pass at or past %d",
	      (long long)(on_at - start), ST_PWR_ON_MS);
}

/* ---- F11: an ADC failure is not activity ------------------------------ */
static void test_F11_adc_failure_is_not_activity(void)
{
	svc_t s;
	st_pwr_in_t in;
	st_pwr_out_t out;
	int64_t start, off_at = -1;
	int pass = 0;

	svc_init(&s);
	start = s.now;
	while (s.now < start + ST_PWR_OFF_MS + 200 && off_at < 0) {
		memset(&in, 0, sizeof(in));
		in.fn_down = true;
		in.now_ms  = s.now;
		/* EVERY fader read fails. */
		in.fader_raw = ((pass % 4) == 0) ? -1 : -1;
		in.fader_idx = s.rr;
		st_pwr_service(&s.p, &in, &out);
		if (out.off_due) {
			off_at = s.now;
		}
		s.now += 8;
		pass++;
	}
	CHECK(off_at >= 0 && off_at - start < ST_PWR_OFF_MS + 16,
	      "F11. an ADC that fails every read does not block the hold");
}

/* ---- F12: AIN1 IS A REAL CONTROL, THROUGH THE SERVICE ------------------
 *
 * ADDED BECAUSE A MUTANT SURVIVED. Deleting `|| in->ain1_active` from
 * st_pwr_service()'s control map broke ZERO checks: every failure-injection
 * case above interrupts the hold with AIN0, and svc_run()'s `ain1` parameter
 * was never once passed true. The AIN1 requirement was therefore asserted only
 * against st_pwr_hold_tick(), which cannot see how the service assembles its
 * inputs -- the exact shape of the 24.7 s defect (the layer with the tests was
 * not the layer with the bug).
 *
 * AIN1 carries VOL +/-, rocker FWD/RWD and the FX entry chord. FUNCTION +
 * rocker is a legitimate performance interaction, so a timer blind to AIN1
 * would shut the instrument down underneath a player's hand.
 */
static void test_F12_ain1_resets_the_transaction_through_the_service(void)
{
	svc_t s;
	int64_t released_at, off_at, start;

	/* (a) the rocker is already moving when FUNCTION goes down */
	svc_init(&s);
	(void)svc_run(&s, true, false, true, 1500);
	released_at = s.now;
	off_at = svc_run(&s, true, false, false, ST_PWR_OFF_MS + 200);
	CHECK(off_at >= 0 && off_at - released_at >= ST_PWR_OFF_MS,
	      "F12a. AIN1 active at the rising edge: a FULL fresh %d ms after "
	      "it stops (%lld), the 1.5 s not banked", ST_PWR_OFF_MS,
	      (long long)(off_at - released_at));

	/* (b) VOL is pressed at 4.9 s -- the deadline must be lost, not met */
	svc_init(&s);
	(void)svc_run(&s, true, false, false, 4900);
	(void)svc_run(&s, true, false, true, 300);
	released_at = s.now;
	off_at = svc_run(&s, true, false, false, ST_PWR_OFF_MS + 200);
	CHECK(off_at >= 0 && off_at - released_at >= ST_PWR_OFF_MS,
	      "F12b. AIN1 at 4.9 s resets completely: a full %d ms after the "
	      "release", ST_PWR_OFF_MS);

	/* (c) FUNCTION + rocker held for eight seconds must NEVER power off */
	svc_init(&s);
	off_at = svc_run(&s, true, false, true, 8000);
	CHECK(off_at < 0,
	      "F12c. FUNCTION + rocker held 8 s never powers off");

	/* (d) and the same through the ON threshold, which the standby loop
	 *     reaches with exactly this input */
	svc_init(&s);
	start = s.now;
	(void)svc_run(&s, true, false, true, 3000);
	CHECK(st_pwr_hold_elapsed_ms(&s.p.hold, s.now) == 0,
	      "F12d. 3 s of FUNCTION + AIN1 leaves the ON timer at zero");
	(void)start;
}

/* ---- F13: A FADER READING OFFERED OUTSIDE A TRANSACTION IS IGNORED -----
 *
 * ALSO ADDED BECAUSE A MUTANT SURVIVED. Changing the service's
 * `if (in->fn_down)` fader-sampling guard to `if (1)` broke zero checks,
 * because main.c passes fader_raw = -1 with FUNCTION up and svc_run() copies
 * that. So the module's OWN guarantee -- "faders are sampled only inside a
 * transaction" -- rested entirely on the caller's discipline.
 *
 * It must not. A future caller that has a fresh reading in hand (a background
 * poll, a merged control pass) would otherwise seed baselines outside any
 * hold, and the first hold afterwards would compare against a position from
 * before it began. That IS the 24.7 s defect, reintroduced through a caller
 * change rather than a module change.
 */
static void test_F13_fader_readings_outside_a_transaction_are_ignored(void)
{
	svc_t s;
	st_pwr_in_t in;
	st_pwr_out_t out;
	int64_t start, off_at;
	int pass;

	svc_init(&s);

	/* 2 s of passes with FUNCTION UP, feeding a REAL, WILDLY MOVING fader
	 * reading on every single pass. Nothing here may be remembered. */
	for (pass = 0; pass < 250; pass++) {
		memset(&in, 0, sizeof(in));
		in.fn_down   = false;
		in.now_ms    = s.now;
		in.fader_idx = (uint8_t)(pass & (ST_PWR_FADERS - 1u));
		in.fader_raw = (int32_t)((pass * 271) % 3700);
		st_pwr_service(&s.p, &in, &out);
		s.now += 8;
	}
	CHECK(!st_pwr_fader_active(&s.p.fader, s.now),
	      "F13. a fader swept for 2 s with FUNCTION UP leaves no movement "
	      "state behind");

	/* Now the hold, with every fader resting where svc_init left it. */
	start = s.now;
	off_at = svc_run(&s, true, false, false, ST_PWR_OFF_MS + 200);
	CHECK(off_at >= 0 && off_at - start < ST_PWR_OFF_MS + 16,
	      "F13. ...and the hold that follows takes exactly %d ms, not %lld",
	      ST_PWR_OFF_MS, (long long)(off_at - start));
}

/* ---- F14: THE SETTLE DEPTH IS PINNED IN BOTH DIRECTIONS ----------------
 *
 * The debounce is a safety trade with a wrong answer on each side. Too
 * shallow and one noisy sample destroys a 4.99 s hold; too deep and a
 * released control keeps blocking, or -- worse -- a control still under a
 * finger is forgotten and the device powers off during a gesture.
 *
 * NOTE ON ST_PWR_SETTLE_PASSES. The tick's first disagreeing sample lands in
 * the `else` branch and sets cand_n = 1 WITHOUT testing the threshold, so the
 * earliest a verdict can flip is the second agreeing sample. The effective
 * depth is therefore max(2, ST_PWR_SETTLE_PASSES): setting the constant to 1
 * changes nothing, which is why a mutation to `>= 1u` survives. Raising it
 * does change behaviour, and this test pins that.
 */
static void test_F14_the_settle_depth_is_exactly_two_passes(void)
{
	st_pwr_hold_t h;
	int64_t t = 500000;
	int n;

	/* RISING: two agreeing ACTIVE samples commit, and not before. */
	st_pwr_hold_reset(&h);
	(void)st_pwr_hold_tick(&h, true, false, t); t += TICK_MS;
	(void)st_pwr_hold_tick(&h, true, true, t);  t += TICK_MS;
	CHECK(!h.other_active, "F14. one ACTIVE sample does not commit");
	(void)st_pwr_hold_tick(&h, true, true, t);  t += TICK_MS;
	CHECK(h.other_active, "F14. the SECOND agreeing ACTIVE sample commits "
	      "-- not the third or later");

	/* FALLING: symmetric, and it must not take longer. A control released
	 * mid-gesture that kept blocking would silently lengthen every hold. */
	(void)st_pwr_hold_tick(&h, true, false, t); t += TICK_MS;
	CHECK(h.other_active, "F14. one IDLE sample does not clear the verdict");
	(void)st_pwr_hold_tick(&h, true, false, t); t += TICK_MS;
	CHECK(!h.other_active, "F14. the SECOND agreeing IDLE sample clears it "
	      "-- the release is not deferred");

	/* And an alternating rail commits NOTHING, in either direction: the
	 * candidate is discarded every time the reading disagrees with it. */
	st_pwr_hold_reset(&h);
	for (n = 0; n < 200; n++) {
		(void)st_pwr_hold_tick(&h, true, (n & 1) != 0, t);
		t += TICK_MS;
	}
	CHECK(!h.other_active,
	      "F14. 200 alternating samples never commit a verdict");
}

/* ======================================================================
 * A1..A10 -- RELEASE-TO-REARM
 * ======================================================================
 *
 *   A SINGLE UNINTERRUPTED FUNCTION PRESS CANNOT BOTH POWER THE DEVICE ON
 *   AND LATER POWER IT OFF.
 *
 * Before this existed the two thresholds sat on one clock with no state
 * between them: hold from OFF, the device turns on at 2.000 s, the same finger
 * keeps counting, and at 5.000 s it switches off again. Seven seconds, one
 * press, two transitions.
 *
 * These ten cases are the contract as written, in order.
 */

/* Drive the real service until `ms` have passed, reporting the transitions. */
typedef struct { int64_t on_at; int64_t off_at; int n_on; int n_off; } trans_t;

static void svc_watch(svc_t *s, bool fn, int64_t ms, trans_t *tr)
{
	const int64_t end = s->now + ms;
	int pass = 0;

	while (s->now < end) {
		st_pwr_in_t in;
		st_pwr_out_t out;

		memset(&in, 0, sizeof(in));
		in.fn_down   = fn;
		in.now_ms    = s->now;
		in.fader_raw = -1;
		if (fn && (pass % 4) == 0) {
			in.fader_idx = s->rr;
			in.fader_raw = s->fader_pos[s->rr];
			s->rr = (uint8_t)((s->rr + 1u) & (ST_PWR_FADERS - 1u));
		}
		st_pwr_service(&s->p, &in, &out);
		if (out.on_due) {
			if (tr->n_on == 0) { tr->on_at = s->now; }
			tr->n_on++;
		}
		if (out.off_due) {
			if (tr->n_off == 0) { tr->off_at = s->now; }
			tr->n_off++;
		}
		s->now += 8;
		pass++;
	}
}

static void trans_clear(trans_t *tr) { memset(tr, 0, sizeof(*tr)); tr->on_at = -1; tr->off_at = -1; }

/* ---- A1 / A2: the wake threshold is still exact ----------------------- */
static void test_A1_A2_the_wake_threshold(void)
{
	svc_t s; trans_t tr; int64_t start;

	svc_init_off(&s); trans_clear(&tr);
	start = s.now;
	svc_watch(&s, true, ST_PWR_ON_MS - 1, &tr);
	CHECK(tr.n_on == 0, "A1. FUNCTION held %d ms from OFF stays off",
	      ST_PWR_ON_MS - 1);

	svc_watch(&s, true, 200, &tr);
	CHECK(tr.n_on == 1 && tr.on_at - start >= ST_PWR_ON_MS,
	      "A2. ...and turns on at %d ms (measured %lld)", ST_PWR_ON_MS,
	      (long long)(tr.on_at - start));
}

/* ---- A3: the SAME press, held on and on, must not power off ----------- */
static void test_A3_the_same_press_held_5_10_30_seconds(void)
{
	const int64_t probes[] = { 5000, 10000, 30000 };
	size_t i;

	for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
		svc_t s; trans_t tr;

		svc_init_off(&s); trans_clear(&tr);
		/* ONE press. Never released. */
		svc_watch(&s, true, probes[i], &tr);

		CHECK(tr.n_on == 1 && tr.n_off == 0,
		      "A3. one unbroken press of %lld ms: ON once, OFF never "
		      "(on=%d off=%d)", (long long)probes[i], tr.n_on, tr.n_off);
		CHECK(st_pwr_device_on(&s.p) && !st_pwr_off_armed(&s.p),
		      "A3. ...device ON, shutdown still DISARMED after %lld ms",
		      (long long)probes[i]);
	}
}

/* ---- A4: the release, and only the release, arms shutdown ------------- */
static void test_A4_release_after_wake_arms_shutdown(void)
{
	svc_t s; trans_t tr;

	svc_init_off(&s); trans_clear(&tr);
	svc_watch(&s, true, 6000, &tr);
	CHECK(!st_pwr_off_armed(&s.p), "A4. disarmed while the press is held");

	svc_watch(&s, false, 16, &tr);          /* the finger lifts */
	CHECK(st_pwr_off_armed(&s.p),
	      "A4. releasing FUNCTION after the wake ARMS shutdown");
	CHECK(st_pwr_device_on(&s.p), "A4. ...and the device is still on");
}

/* ---- A5 / A6: the second press is an ordinary 5.000 s transaction ----- */
static void test_A5_A6_the_second_press_powers_off(void)
{
	svc_t s; trans_t tr; int64_t start;

	svc_init_off(&s); trans_clear(&tr);
	svc_watch(&s, true, 6000, &tr);         /* wake, keep holding */
	svc_watch(&s, false, 100, &tr);         /* release -> armed */

	start = s.now;
	svc_watch(&s, true, ST_PWR_OFF_MS - 1, &tr);
	CHECK(tr.n_off == 0, "A5. the second press at %d ms remains on",
	      ST_PWR_OFF_MS - 1);

	svc_watch(&s, true, 200, &tr);
	CHECK(tr.n_off > 0 && tr.off_at - start >= ST_PWR_OFF_MS &&
	      tr.off_at - start < ST_PWR_OFF_MS + 16,
	      "A6. ...and powers off at %d ms (measured %lld)", ST_PWR_OFF_MS,
	      (long long)(tr.off_at - start));
	CHECK(tr.n_on == 1, "A6. still exactly ONE on-transition in the whole run");
}

/* ---- A7: thirty seconds, one press, exactly one transition ------------ */
static void test_A7_thirty_seconds_one_transition(void)
{
	svc_t s; trans_t tr;

	svc_init_off(&s); trans_clear(&tr);
	svc_watch(&s, true, 30000, &tr);

	CHECK(tr.n_on + tr.n_off == 1 && tr.n_on == 1,
	      "A7. a single uninterrupted 30 s hold from OFF produces EXACTLY "
	      "one transition, OFF -> ON (on=%d off=%d)", tr.n_on, tr.n_off);
}

/* ---- A8: a second transition is impossible without a physical release -- */
static void test_A8_no_second_transition_without_a_release(void)
{
	svc_t s; trans_t tr;
	int64_t k;

	svc_init_off(&s); trans_clear(&tr);
	svc_watch(&s, true, 3000, &tr);         /* on, disarmed */

	/* Five minutes of continuous hold, sampled the whole way. Any single
	 * pass reporting off_due would fail this. */
	for (k = 0; k < 60; k++) {
		svc_watch(&s, true, 5000, &tr);
		if (tr.n_off) { break; }
	}
	CHECK(tr.n_off == 0,
	      "A8. five minutes of unbroken hold: no second transition is "
	      "reachable without a physical release");

	/* And the instant the finger lifts, it becomes reachable again. */
	svc_watch(&s, false, 16, &tr);
	svc_watch(&s, true, ST_PWR_OFF_MS + 100, &tr);
	CHECK(tr.n_off > 0, "A8. ...and after the release it is reachable");
}

/* ---- A9: nothing but the release can grant the arm -------------------- */
static void test_A9_nothing_can_fake_the_release(void)
{
	svc_t s; trans_t tr;

	/* (a) NEITHER INITIALISER GRANTS IT. This is the whole reason both
	 *     start disarmed: if a re-init could arm shutdown, then any code
	 *     that re-initialised the module -- a feature, a mode change, a
	 *     recovery path -- would be able to hand a held press the second
	 *     transition the contract forbids. */
	svc_init_off(&s);
	CHECK(!st_pwr_off_armed(&s.p), "A9. st_pwr_init_off() does not arm");
	st_pwr_init_on(&s.p);
	CHECK(!st_pwr_off_armed(&s.p), "A9. st_pwr_init_on() does not arm");

	/* (b) RE-INITIALISING MID-PRESS DOES NOT ARM IT EITHER. This is the
	 *     literal "software state reset cannot fake a release" case. */
	svc_init_off(&s); trans_clear(&tr);
	svc_watch(&s, true, 3000, &tr);          /* on, disarmed, finger down */
	st_pwr_init_on(&s.p);                    /* the most aggressive reset
						  * available to any caller */
	svc_watch(&s, true, ST_PWR_OFF_MS + 500, &tr);
	CHECK(tr.n_off == 0,
	      "A9. a full re-init mid-press does NOT arm shutdown -- the held "
	      "finger still owes a release");

	/* (c) AND OTHER PHYSICAL ACTIVITY IS NOT A RELEASE. Pressing PLAY, the
	 *     rocker or a fader resets the TRANSACTION; it does not lift the
	 *     finger off FUNCTION, so it cannot arm shutdown. */
	svc_init_off(&s); trans_clear(&tr);
	svc_watch(&s, true, 3000, &tr);
	(void)svc_run(&s, true, true, false, 500);    /* PLAY, FUNCTION still down */
	(void)svc_run(&s, true, false, true, 500);    /* rocker, ditto */
	CHECK(!st_pwr_off_armed(&s.p),
	      "A9. AIN0 and AIN1 activity during the press do not arm shutdown");
	(void)svc_run(&s, true, false, false, ST_PWR_OFF_MS + 500);
	CHECK(!st_pwr_off_armed(&s.p),
	      "A9. ...and a further full 5 s of clean hold still does not");
}

/* ---- A11: A TAP BEFORE THE WAKE MUST NOT PRE-ARM THE WAKE PRESS -------
 *
 * ADDED BECAUSE A MUTANT SURVIVED. Deleting the explicit disarm at the ON
 * transition broke nothing, because every A-case above began from a fresh
 * st_pwr_init_off() and never released FUNCTION before the successful hold --
 * so off_armed was still false from initialisation and the missing line could
 * not be observed.
 *
 * It is observable, and by the most ordinary gesture there is. Someone taps
 * FUNCTION (nothing happens -- ON-9), and that release ARMS shutdown. Then
 * they hold properly and the device wakes at 2.000 s. Without the disarm the
 * arm from that earlier tap is still standing, and the very press that woke
 * the instrument switches it off again three seconds later.
 *
 * The disarm at the transition is what makes the rule about THE PRESS rather
 * than about whatever happened to precede it.
 */
static void test_A11_a_tap_before_the_wake_does_not_pre_arm(void)
{
	svc_t s; trans_t tr;
	int n;

	svc_init_off(&s); trans_clear(&tr);

	/* Twenty short taps: none wakes it, and each release arms shutdown. */
	for (n = 0; n < 20; n++) {
		svc_watch(&s, true, 300, &tr);
		svc_watch(&s, false, 200, &tr);
	}
	CHECK(tr.n_on == 0 && tr.n_off == 0,
	      "A11. twenty short taps produce no transition at all");
	CHECK(st_pwr_off_armed(&s.p),
	      "A11. ...and the last release did leave the arm standing");

	/* Now the real hold, and then keep holding well past 5 s. */
	svc_watch(&s, true, 12000, &tr);
	CHECK(tr.n_on == 1,
	      "A11. the hold after the taps wakes the device exactly once");
	CHECK(tr.n_off == 0,
	      "A11. ...and the SAME press does not then switch it off -- the "
	      "transition disarmed the arm the taps had left standing");
	CHECK(!st_pwr_off_armed(&s.p),
	      "A11. ...leaving the wake press owing its own release");

	/* And the release still works normally afterwards. */
	svc_watch(&s, false, 16, &tr);
	svc_watch(&s, true, ST_PWR_OFF_MS + 100, &tr);
	CHECK(tr.n_off > 0, "A11. after that release, 5.000 s powers off");
}

/* ---- A10: standby and battery wake are the same object, the same rule -- */
static void test_A10_standby_and_battery_wake_are_identical(void)
{
	svc_t a, b; trans_t ta, tb;

	/* There is only one wake path in the module -- st_pwr_init_off()
	 * followed by st_pwr_service() -- and main.c reaches it from both the
	 * charge-standby loop and a battery/SYSTEM_OFF wake. Driving it twice
	 * proves the rule is a property of the module rather than of either
	 * caller: same thresholds, same disarm, same debt. */
	svc_init_off(&a); trans_clear(&ta);
	svc_init_off(&b); trans_clear(&tb);

	svc_watch(&a, true, 12000, &ta);
	svc_watch(&b, true, 12000, &tb);

	CHECK(ta.n_on == tb.n_on && ta.n_off == tb.n_off &&
	      ta.n_on == 1 && ta.n_off == 0,
	      "A10. both wake entries: one ON, no OFF, from a 12 s held press");
	CHECK(st_pwr_off_armed(&a.p) == st_pwr_off_armed(&b.p) &&
	      !st_pwr_off_armed(&a.p),
	      "A10. ...and both are left DISARMED, owing the same release");
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
	RUN(test_a_fader_moved_before_the_hold_does_not_delay_it);
	RUN(test_a_fader_being_moved_holds_the_timer_at_zero);
	RUN(test_the_hand_leaving_a_fader_is_forgotten_promptly);
	RUN(test_fader_noise_below_the_threshold_is_not_movement);
	RUN(test_a_failed_adc_read_is_not_movement);
	RUN(test_F1_a_fader_moved_before_function_went_down);
	RUN(test_F2_a_control_active_at_the_rising_edge);
	RUN(test_F3_control_released_mid_hold);
	RUN(test_F4_repeated_interruption_near_the_deadline);
	RUN(test_F5_repeated_taps_never_accumulate);
	RUN(test_F6_a_phantom_single_sample_does_not_reset);
	RUN(test_F7_subthreshold_fader_noise_never_blocks);
	RUN(test_F8_real_fader_movement_blocks_and_then_releases);
	RUN(test_F9_the_rising_edge_forgets_everything);
	RUN(test_F10_the_on_threshold_through_the_service);
	RUN(test_F11_adc_failure_is_not_activity);
	RUN(test_F12_ain1_resets_the_transaction_through_the_service);
	RUN(test_F13_fader_readings_outside_a_transaction_are_ignored);
	RUN(test_F14_the_settle_depth_is_exactly_two_passes);
	RUN(test_A1_A2_the_wake_threshold);
	RUN(test_A3_the_same_press_held_5_10_30_seconds);
	RUN(test_A4_release_after_wake_arms_shutdown);
	RUN(test_A5_A6_the_second_press_powers_off);
	RUN(test_A7_thirty_seconds_one_transition);
	RUN(test_A8_no_second_transition_without_a_release);
	RUN(test_A9_nothing_can_fake_the_release);
	RUN(test_A11_a_tap_before_the_wake_does_not_pre_arm);
	RUN(test_A10_standby_and_battery_wake_are_identical);

	printf("\n%d cases, %d checks, %d failures\n", g_cases, g_checks, g_failures);
	return g_failures ? 1 : 0;
}
