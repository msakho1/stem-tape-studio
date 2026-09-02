/*
 * test_power_hold.c -- the escape hatch, proven from every reachable state.
 *
 * This is postmortem invariants P1 and P7:
 *
 *   P1  the shutdown timer is not inside any feature-gated branch
 *   P7  any new FUNCTION handling ships with an explicit test proving a 2.5 s
 *       power hold still works FROM EVERY POSSIBLE FEATURE STATE -- enumerated,
 *       not sampled
 *
 * and the enumeration below is the thing that makes P7 mean something. Every
 * suppressor st54 actually contains is named, and each is exercised: the
 * scratch series only added a THIRD producer of `function_consumed` to two that
 * were already there, and `combo_seen` suppresses the shutdown from three more
 * gestures on top of that. A test that checked "hold FUNCTION, device turns
 * off" from a clean idle state would have passed on st55 too.
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

/* main.c's control pass. Every simulated hold is driven at this cadence, so a
 * count of passes is a real number of iterations of the real loop. */
#define PASS_MS 8

/*
 * ======================================================================
 * THE FEATURE-STATE SPACE -- every suppressor st54 actually has
 * ======================================================================
 * These are NOT inputs to st_pwr_hold_tick(). That is the point: the module's
 * signature has nowhere to put them, so the enumeration is here to prove the
 * property holds while they are set, not to feed them in.
 *
 * Each row records where the flag is set in st54 and what it suppressed.
 */
typedef struct {
	const char *name;
	const char *set_by;
	const char *suppressed;   /* what it stopped, in st54 */
	bool        chord_first;  /* does reaching this state require another
				    * physical button to have been held? */
} feature_state_t;

static const feature_state_t FEATURE_STATES[] = {
	{ "idle",                "-",
	  "nothing",                                              false },
	{ "loop latched",        "st_ctl.c ST_LOOP_ACT_LATCH -> fn_consumed",
	  "the whole power-off branch (function_consumed)",       true  },
	{ "reverse double-tap",  "st_ctl.c 2nd tap -> fn_consumed",
	  "the whole power-off branch (function_consumed)",       true  },
	{ "scratch armed",       "st55 scratch_service() -> fn_consumed, EVERY PASS",
	  "the whole power-off branch, continuously",             true  },
	{ "FN+PLAY combo",       "main.c combo_seen",
	  "countdown + shutdown for the rest of the press",       true  },
	{ "FN+Track bank jump",  "main.c combo_seen (bank surf)",
	  "countdown + shutdown for the rest of the press",       true  },
	{ "tap-run grid clear",  "main.c combo_seen (M8a hold)",
	  "countdown + shutdown for the rest of the press",       true  },
	{ "FN+VOL chop chord",   "main.c cp_cnt == 3 -> continue",
	  "the pass, before the hold check",                      true  },
	{ "FX overlay held",     "s_fx_track_claim",
	  "nothing directly -- included because it is live state", true  },
	{ "solo held",           "trk[].solo via track_mask",
	  "nothing directly -- included because it is live state", true  },
};

#define N_FEATURE_STATES (sizeof(FEATURE_STATES) / sizeof(FEATURE_STATES[0]))

/*
 * ONE SIMULATED PRESS, at the real control cadence.
 *
 * `chord_passes` is how long another physical control was ALSO held at the
 * start of the press -- which is what putting the firmware into each feature
 * state above requires. After that the player holds FUNCTION alone.
 *
 * Returns the millisecond at which the device powered off, or -1.
 */
static int64_t simulate_press(int chord_passes, int total_passes)
{
	st_pwr_hold_t h;
	int64_t now = 1000;   /* not 0: a real device has been up a while */
	int p;

	st_pwr_hold_reset(&h);

	for (p = 0; p < total_passes; p++) {
		const bool fn_down = true;
		const bool other_down = (p < chord_passes);

		if (st_pwr_hold_tick(&h, fn_down, other_down, now)) {
			return now;
		}
		now += PASS_MS;
	}
	return -1;
}

/* ======================================================================
 * P7 -- from every feature state
 * ====================================================================== */

static void test_a_clean_hold_powers_off_from_every_feature_state(void)
{
	size_t i;

	for (i = 0; i < N_FEATURE_STATES; i++) {
		const feature_state_t *fs = &FEATURE_STATES[i];
		/* Reaching the state costs a chord; then the player lets go of
		 * everything but FUNCTION and holds. Give the hold generous
		 * room -- what is under test is that it fires at all. */
		const int chord = fs->chord_first ? (500 / PASS_MS) : 0;
		const int total = chord + (4000 / PASS_MS);
		const int64_t off_at = simulate_press(chord, total);
		const int64_t chord_ended = 1000 + (int64_t)chord * PASS_MS;

		CHECK(off_at >= 0,
		      "P7. powers off from \"%s\" (%s)", fs->name, fs->set_by);
		CHECK(off_at >= 0 && off_at - chord_ended >= ST_PWR_HOLD_MS &&
		      off_at - chord_ended < ST_PWR_HOLD_MS + 2 * PASS_MS,
		      "P7. ...%lld ms after the last other button came up "
		      "(exactly one clean hold, not banked time)",
		      (long long)(off_at - chord_ended));
	}
}

/*
 * THE CASE st54 FAILS, isolated and named.
 *
 * In st54, a FUNCTION+PLAY combo / bank jump / grid clear sets combo_seen, and
 * the branch then `continue`s past the shutdown FOR THE REST OF THAT PRESS --
 * so a player who chords, releases the other button, and keeps holding FUNCTION
 * gets nothing, forever, until they release FUNCTION and press again. Under the
 * reset-never-suppress rule they get a shutdown 2.5 s later.
 */
static void test_a_chord_does_not_latch_the_hatch_shut(void)
{
	const int chord = 300 / PASS_MS;          /* a brief chord */
	const int64_t off_at = simulate_press(chord, chord + (10000 / PASS_MS));

	CHECK(off_at >= 0,
	      "P2. a chord earlier in the SAME press does not latch the escape "
	      "hatch shut (st54's combo_seen does -- for the whole press)");
	printf("       st54: combo_seen is set once and suppresses the shutdown\n"
	       "             until FUNCTION is RELEASED. Holding it longer does\n"
	       "             nothing. That is the latch this replaces.\n");
}

/* ======================================================================
 * The other half of the rule: a chord must still not power off
 * ====================================================================== */

static void test_a_held_chord_never_powers_off(void)
{
	st_pwr_hold_t h;
	int64_t now = 1000;
	int p;
	bool fired = false;

	st_pwr_hold_reset(&h);
	/* FUNCTION + PLAY held together for TEN seconds -- four times the
	 * threshold, and well past the FN+PLAY toggle's own 350..5000 ms
	 * window. */
	for (p = 0; p < 10000 / PASS_MS; p++) {
		if (st_pwr_hold_tick(&h, true, true, now)) {
			fired = true;
		}
		now += PASS_MS;
	}
	CHECK(!fired,
	      "P2. FUNCTION + another control held 10 s never powers off -- a "
	      "chord means something else, and still does");
}

static void test_releasing_the_chord_restarts_the_clock_not_resumes_it(void)
{
	st_pwr_hold_t h;
	int64_t now = 1000;
	int p;
	int64_t off_at = -1;
	const int chord_passes = 5000 / PASS_MS;   /* 5 s of chord */

	st_pwr_hold_reset(&h);
	for (p = 0; p < chord_passes; p++) {
		(void)st_pwr_hold_tick(&h, true, true, now);
		now += PASS_MS;
	}
	/* PLAY comes up; FUNCTION stays down. */
	for (p = 0; p < 4000 / PASS_MS && off_at < 0; p++) {
		if (st_pwr_hold_tick(&h, true, false, now)) {
			off_at = now;
		}
		now += PASS_MS;
	}

	CHECK(off_at >= 0, "P2. ...and after the chord ends it does power off");
	CHECK(off_at - (1000 + (int64_t)chord_passes * PASS_MS) >= ST_PWR_HOLD_MS,
	      "P2. ...a full %d ms AFTER the release -- the 5 s of chord was "
	      "not banked", ST_PWR_HOLD_MS);
}

/* ======================================================================
 * Timing, exhaustively at the real cadence
 * ====================================================================== */

static void test_it_fires_at_the_threshold_and_not_before(void)
{
	st_pwr_hold_t h;
	int64_t now = 1000;
	int p;
	int64_t off_at = -1;

	st_pwr_hold_reset(&h);
	for (p = 0; p < 5000 / PASS_MS && off_at < 0; p++) {
		if (st_pwr_hold_tick(&h, true, false, now)) {
			off_at = now;
		}
		now += PASS_MS;
	}
	CHECK(off_at - 1000 >= ST_PWR_HOLD_MS,
	      "P1. never fires early (%lld ms >= %d)",
	      (long long)(off_at - 1000), ST_PWR_HOLD_MS);
	CHECK(off_at - 1000 < ST_PWR_HOLD_MS + PASS_MS,
	      "P1. and fires on the FIRST pass at or past it (%lld ms)",
	      (long long)(off_at - 1000));
}

static void test_a_release_forgets_everything(void)
{
	st_pwr_hold_t h;
	int64_t now = 1000;
	int p;

	st_pwr_hold_reset(&h);
	/* Almost there... */
	for (p = 0; p < 2400 / PASS_MS; p++) {
		(void)st_pwr_hold_tick(&h, true, false, now);
		now += PASS_MS;
	}
	/* ...then FUNCTION comes up for one pass. */
	(void)st_pwr_hold_tick(&h, false, false, now);
	now += PASS_MS;
	CHECK(st_pwr_hold_elapsed_ms(&h, now) == 0,
	      "P1. releasing FUNCTION forgets the 2400 ms it had accumulated");

	/* And the next press starts from zero, not from 2400. */
	CHECK(!st_pwr_hold_tick(&h, true, false, now),
	      "P1. the next press does not fire instantly");
}

static void test_it_stays_fired_while_held(void)
{
	st_pwr_hold_t h;
	int64_t now = 1000;
	int p, fires = 0;

	st_pwr_hold_reset(&h);
	for (p = 0; p < 4000 / PASS_MS; p++) {
		if (st_pwr_hold_tick(&h, true, false, now)) {
			fires++;
		}
		now += PASS_MS;
	}
	CHECK(fires > 1,
	      "P1. keeps reporting shutdown every pass past the threshold "
	      "(%d passes) -- a missed pass costs nothing, it is not a one-shot",
	      fires);
}

/*
 * RAIL NOISE CAN DELAY, NEVER PREVENT. `other_down` comes off the shared
 * ladder, which st55 proved can read a phantom press. A phantom that flickers
 * costs one pass each time; only a phantom held CONTINUOUSLY for the whole
 * hold could stop a shutdown, and that is a stuck button, not noise.
 */
static void test_intermittent_phantom_presses_only_delay(void)
{
	st_pwr_hold_t h;
	int64_t now = 1000;
	int p;
	int64_t off_at = -1;

	st_pwr_hold_reset(&h);
	for (p = 0; p < 20000 / PASS_MS && off_at < 0; p++) {
		/* a phantom every 40th pass (~320 ms) */
		const bool phantom = (p % 40) == 39;

		if (st_pwr_hold_tick(&h, true, phantom, now)) {
			off_at = now;
		}
		now += PASS_MS;
	}
	CHECK(off_at < 0,
	      "P-noise. a phantom every 320 ms DOES hold the hatch shut -- "
	      "which is why `other_down` must come from a DEBOUNCED decode, "
	      "recorded here as a real constraint on the caller");

	/* And with the phantom rarer than the threshold, it powers off. */
	st_pwr_hold_reset(&h);
	now = 1000; off_at = -1;
	for (p = 0; p < 20000 / PASS_MS && off_at < 0; p++) {
		const bool phantom = (p % 500) == 499;   /* every 4 s */

		if (st_pwr_hold_tick(&h, true, phantom, now)) {
			off_at = now;
		}
		now += PASS_MS;
	}
	CHECK(off_at >= 0,
	      "P-noise. a phantom rarer than the threshold only delays it");
}

int main(void)
{
	printf("power-hold escape hatch -- threshold %d ms, control pass %d ms\n",
	       ST_PWR_HOLD_MS, PASS_MS);
	printf("%zu feature states enumerated:\n", N_FEATURE_STATES);
	for (size_t i = 0; i < N_FEATURE_STATES; i++) {
		printf("   %-22s set by %-46s suppressed: %s\n",
		       FEATURE_STATES[i].name, FEATURE_STATES[i].set_by,
		       FEATURE_STATES[i].suppressed);
	}

	RUN(test_a_clean_hold_powers_off_from_every_feature_state);
	RUN(test_a_chord_does_not_latch_the_hatch_shut);
	RUN(test_a_held_chord_never_powers_off);
	RUN(test_releasing_the_chord_restarts_the_clock_not_resumes_it);
	RUN(test_it_fires_at_the_threshold_and_not_before);
	RUN(test_a_release_forgets_everything);
	RUN(test_it_stays_fired_while_held);
	RUN(test_intermittent_phantom_presses_only_delay);

	printf("\n%d cases, %d checks, %d failures\n", g_cases, g_checks, g_failures);
	return g_failures ? 1 : 0;
}
