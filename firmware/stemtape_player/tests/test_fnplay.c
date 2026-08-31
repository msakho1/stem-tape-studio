/*
 * test_fnplay.c -- the FUNCTION + PLAY tap gesture.
 *
 * ONE PROPERTY DOMINATES THIS FILE. x1 (slow) and x2 (snap home) are mutually
 * exclusive, so a double tap must produce the SNAP and must never also
 * produce the SLOW toggle -- not "must end up in the right state", but must
 * never emit the wrong action even for one pass. Slow is a whole octave; a
 * double that dipped through it before snapping home would be audible, on the
 * one gesture whose purpose is to come home cleanly.
 *
 * So every case below drives the module the way the control loop does --
 * tick() on EVERY pass, tap() on releases -- and records every action emitted,
 * then asserts on the whole sequence rather than on the final state.
 *
 * Build (from the repo root):
 *   cc -std=c11 -Wall -Wextra -Werror -Ifirmware/stemtape_player/src \
 *      firmware/stemtape_player/src/st_fnplay.c \
 *      firmware/stemtape_player/tests/test_fnplay.c \
 *      -o test_fnplay && ./test_fnplay
 */

#include <stdio.h>
#include <string.h>

#include "st_fnplay.h"

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

/* ---- a recording rig that ticks like the real control loop ------------ */

#define MAXACT 32

static st_fnplay_t  g_f;
static uint32_t     g_now;
static st_fnplay_action_t g_acts[MAXACT];
static uint32_t     g_act_ms[MAXACT];
static int          g_nact;

/* The control pass is ~8 ms in main.c; use it so the windows are exercised at
 * a realistic granularity rather than landing exactly on their boundaries. */
#define PASS_MS 8u

static void rig_reset(void)
{
	st_fnplay_reset(&g_f);
	g_now  = 1000u;      /* not zero, so a wrap-naive impl has no free pass */
	g_nact = 0;
	memset(g_acts, 0, sizeof(g_acts));
}

static void record(st_fnplay_action_t a)
{
	if (a != ST_FNPLAY_ACT_NONE && g_nact < MAXACT) {
		g_acts[g_nact]   = a;
		g_act_ms[g_nact] = g_now;
		g_nact++;
	}
}

/*
 * Advance the clock `ms` milliseconds, ticking every pass exactly as the
 * control loop does.
 *
 * Counted in PASSES rather than compared against an absolute end time. The
 * obvious `end = g_now + ms; while (g_now < end)` is wrong near the counter's
 * wrap -- `end` overflows to a small number and the loop exits immediately,
 * silently advancing nothing. The wrap case below is precisely where that
 * matters, and it would have made that case vacuously pass.
 */
static void advance(uint32_t ms)
{
	uint32_t passes = ms / PASS_MS;

	while (passes--) {
		g_now += PASS_MS;
		record(st_fnplay_tick(&g_f, g_now));
	}
}

/* One tap: press, hold `held` ms, release. The press edge is what opens the
 * double-tap window, so the hold time is deliberately varied by callers. */
static void tap(uint32_t held)
{
	record(st_fnplay_press(&g_f, g_now));
	advance(held);
	st_fnplay_release(&g_f, g_now);
}

static const char *name(st_fnplay_action_t a)
{
	switch (a) {
	case ST_FNPLAY_ACT_SLOW: return "SLOW";
	case ST_FNPLAY_ACT_SNAP: return "SNAP";
	default:                 return "NONE";
	}
}

static void show(void)
{
	int i;

	printf("     emitted:");
	if (g_nact == 0) {
		printf(" (nothing)");
	}
	for (i = 0; i < g_nact; i++) {
		printf(" %s@%ums", name(g_acts[i]), g_act_ms[i] - 1000u);
	}
	printf("\n");
}

/* ======================================================================
 * 1. THE COMPANION'S OWN LIST: x1 slow, x2 snap.
 * ====================================================================== */
static void case_the_spec(void)
{
	g_cases++;
	printf("\n-- x1 is slow, x2 is snap home\n");

	rig_reset();
	tap(40u);
	advance(1000u);
	show();
	CHECK(g_nact == 1 && g_acts[0] == ST_FNPLAY_ACT_SLOW,
	      "a single tap must emit exactly one SLOW");

	rig_reset();
	tap(40u);
	advance(120u);
	tap(40u);
	advance(1000u);
	show();
	CHECK(g_nact == 1 && g_acts[0] == ST_FNPLAY_ACT_SNAP,
	      "a double tap must emit exactly one SNAP");
}

/* ======================================================================
 * 2. THE CENTRAL PROPERTY: a double NEVER emits the single.
 *
 *    Checked across the whole width of the window, one control pass at a
 *    time, because "it works at 120 ms" says nothing about 440 ms -- and the
 *    interval just inside the boundary is exactly where an optimistic
 *    implementation would have already fired.
 * ====================================================================== */
static void case_double_never_emits_slow(void)
{
	uint32_t gap;
	int leaks = 0, misses = 0;

	g_cases++;
	printf("\n-- across the whole window, a double never dips through slow\n");

	/*
	 * `gap` is PRESS-TO-PRESS, which is what the window is measured on.
	 * The second tap is held 40 ms, so for gaps near the top of the window
	 * its RELEASE lands outside -- that is the exact shape of the bug this
	 * sweep caught, and it is why the hold time is non-zero here.
	 */
	/* Starts at 48, not at one pass: the second press cannot arrive before
	 * the first has been released, and a gap below the 40 ms hold would
	 * underflow the advance below rather than describe a real gesture. */
	for (gap = 48u; gap <= ST_FNPLAY_DOUBLE_MS; gap += PASS_MS) {
		int i;

		rig_reset();
		record(st_fnplay_press(&g_f, g_now));
		advance(40u);
		st_fnplay_release(&g_f, g_now);
		advance(gap - 40u);
		tap(40u);                    /* second press, gap ms after the first */
		advance(1000u);

		for (i = 0; i < g_nact; i++) {
			if (g_acts[i] == ST_FNPLAY_ACT_SLOW) {
				leaks++;
			}
		}
		if (g_nact != 1 || g_acts[0] != ST_FNPLAY_ACT_SNAP) {
			misses++;
		}
	}
	printf("     %u press-to-press gaps tested, 48..%u ms, second tap held "
	       "40 ms\n", (ST_FNPLAY_DOUBLE_MS - 48u) / PASS_MS + 1u,
	       ST_FNPLAY_DOUBLE_MS);
	CHECK(leaks == 0,
	      "%d gaps inside the window leaked a SLOW before the SNAP", leaks);
	CHECK(misses == 0,
	      "%d gaps inside the window failed to produce exactly one SNAP",
	      misses);
}

/* ======================================================================
 * 2a. THE WINDOW IS THE FIRMWARE'S 450 ms, NOT THE COMPANION'S 300.
 *
 *     Pinned with LITERALS, deliberately. The sweep above expresses its
 *     bounds in terms of ST_FNPLAY_DOUBLE_MS, so shrinking the constant
 *     shrinks the test with it and the change passes unnoticed -- the
 *     constant would be checking itself. These two numbers straddle the
 *     companion's 300 ms and can only be satisfied by a window at least 420
 *     and less than 600, so the decision recorded in st_fnplay.h is held to
 *     by something outside st_fnplay.h.
 * ====================================================================== */
static void case_the_window_is_the_firmwares(void)
{
	g_cases++;
	printf("\n-- the decision window is the firmware's own, not the app's\n");

	/* 420 ms apart: past the companion's 300, inside the firmware's 450. */
	rig_reset();
	tap(40u);
	advance(380u);          /* press-to-press = 420 ms */
	tap(40u);
	advance(1000u);
	show();
	CHECK(g_nact == 1 && g_acts[0] == ST_FNPLAY_ACT_SNAP,
	      "a 420 ms double must still snap -- the window shrank below the "
	      "firmware's own 450 ms figure");

	/* 600 ms apart: past any plausible window, so two singles. */
	rig_reset();
	tap(40u);
	advance(560u);          /* press-to-press = 600 ms */
	tap(40u);
	advance(1000u);
	show();
	CHECK(g_nact == 2 && g_acts[0] == ST_FNPLAY_ACT_SLOW &&
	      g_acts[1] == ST_FNPLAY_ACT_SLOW,
	      "a 600 ms gap must be two separate toggles, not a double");
}

/* ======================================================================
 * 2b. A LONG PRESS IS NOT A TAP.
 *
 *     Past the tap ceiling the press belongs to the mode toggle (350 ms -
 *     5 s) or the brightness toggle (5 s+), both of which main.c owns. It
 *     must leave nothing pending -- otherwise flipping the loop-length mode
 *     would also, half a second later, drop the song an octave.
 * ====================================================================== */
static void case_a_hold_is_not_a_tap(void)
{
	g_cases++;
	printf("\n-- a press past the tap ceiling arms nothing\n");

	rig_reset();
	tap(ST_FNPLAY_TAP_MAX_MS + 100u);        /* the mode-toggle press */
	advance(2000u);
	show();
	CHECK(g_nact == 0,
	      "a %u ms press emitted something; it belongs to the mode toggle",
	      ST_FNPLAY_TAP_MAX_MS + 100u);

	/* And it does not linger as half a double: the next real tap must be a
	 * single. */
	rig_reset();
	tap(ST_FNPLAY_TAP_MAX_MS + 100u);
	advance(100u);
	tap(40u);
	advance(1000u);
	show();
	CHECK(g_nact == 1 && g_acts[0] == ST_FNPLAY_ACT_SLOW,
	      "a tap following a hold must be a fresh single, not a double");
}

/* ======================================================================
 * 3. THE WINDOW HAS A FAR SIDE. Two taps far enough apart are two singles,
 *    not a double -- otherwise the gesture would swallow deliberate
 *    repeated toggling.
 * ====================================================================== */
static void case_outside_the_window(void)
{
	g_cases++;
	printf("\n-- two taps past the window are two separate slow toggles\n");

	rig_reset();
	tap(40u);
	advance(ST_FNPLAY_DOUBLE_MS + 200u);
	tap(40u);
	advance(1000u);
	show();
	CHECK(g_nact == 2 && g_acts[0] == ST_FNPLAY_ACT_SLOW &&
	      g_acts[1] == ST_FNPLAY_ACT_SLOW,
	      "two well-separated taps must be two SLOW toggles");
	CHECK(g_nact == 2 && g_act_ms[1] > g_act_ms[0],
	      "the two toggles must be ordered in time");
}

/* ======================================================================
 * 4. THE SINGLE COMMITS EVEN AFTER THE BUTTONS ARE LET GO.
 *
 *    The companion is explicit that deferred taps "commit up to
 *    trackDecisionMs after release, long after FUNCTION may have been
 *    released". A tick that only ran while FUNCTION was held would lose
 *    almost every real single tap, since nobody holds FUNCTION for another
 *    450 ms after tapping PLAY.
 * ====================================================================== */
static void case_commits_after_release(void)
{
	uint32_t fired_at;

	g_cases++;
	printf("\n-- the single still fires once the buttons are released\n");

	rig_reset();
	tap(40u);
	/* The player lets go of everything here. Only the tick keeps running. */
	advance(1000u);
	show();
	CHECK(g_nact == 1 && g_acts[0] == ST_FNPLAY_ACT_SLOW,
	      "the single must still commit after release");

	fired_at = (g_nact == 1) ? g_act_ms[0] - 1000u : 0u;
	printf("     tap at %ums, committed at %ums\n", 40u, fired_at);
	CHECK(fired_at > ST_FNPLAY_DOUBLE_MS,
	      "the single fired at %u ms, before the %u ms window closed",
	      fired_at, ST_FNPLAY_DOUBLE_MS);
	/* And not indefinitely late: one window plus a pass or two. */
	CHECK(fired_at <= 40u + ST_FNPLAY_DOUBLE_MS + 4u * PASS_MS,
	      "the single took %u ms to commit -- far longer than one window",
	      fired_at);
}

/* ======================================================================
 * 5. A HOLD CANCELS. main.c calls cancel() when a press turns out to be the
 *    mode toggle or the brightness toggle; nothing may be left pending, or
 *    one press would do two things.
 * ====================================================================== */
static void case_hold_cancels(void)
{
	g_cases++;
	printf("\n-- a press claimed as a hold leaves nothing pending\n");

	rig_reset();
	tap(40u);
	CHECK(st_fnplay_pending(&g_f),
	      "a fresh tap should be pending before its window closes");
	st_fnplay_cancel(&g_f);
	CHECK(!st_fnplay_pending(&g_f), "cancel must drop the pending tap");
	advance(1000u);
	show();
	CHECK(g_nact == 0,
	      "a cancelled tap must emit nothing at all");

	/* And the NEXT tap starts fresh: it must be a single, not a double
	 * paired with the tap that was already spent on the hold. */
	rig_reset();
	tap(40u);
	st_fnplay_cancel(&g_f);
	advance(100u);
	tap(40u);
	advance(1000u);
	show();
	CHECK(g_nact == 1 && g_acts[0] == ST_FNPLAY_ACT_SLOW,
	      "after a cancel the next tap must be a fresh single, not a double");
}

/* ======================================================================
 * 6. TICKING ALONE DOES NOTHING. A control loop runs this thousands of times
 *    a second; if idle ticks could emit, the transport would toggle on its
 *    own.
 * ====================================================================== */
static void case_idle_is_silent(void)
{
	int i;

	g_cases++;
	printf("\n-- idle ticks never emit anything\n");

	rig_reset();
	for (i = 0; i < 20000; i++) {
		g_now += PASS_MS;
		record(st_fnplay_tick(&g_f, g_now));
	}
	printf("     %d idle ticks (%u s of running)\n", i,
	       (uint32_t)(20000u * PASS_MS / 1000u));
	CHECK(g_nact == 0, "%d actions emitted from idle ticks alone", g_nact);
}

/* ======================================================================
 * 7. A THIRD TAP IS NOT x3. Heads is not implemented in this firmware, and
 *    the module says so honestly: the third tap lands as a fresh first tap
 *    (becoming a slow toggle) rather than silently doing nothing or firing
 *    a second snap.
 * ====================================================================== */
static void case_third_tap_is_a_fresh_single(void)
{
	g_cases++;
	printf("\n-- a third tap is a fresh single, not an unimplemented x3\n");

	rig_reset();
	tap(40u);
	advance(120u);
	tap(40u);              /* SNAP here */
	advance(120u);
	tap(40u);              /* a fresh first tap */
	advance(1000u);
	show();
	CHECK(g_nact == 2 && g_acts[0] == ST_FNPLAY_ACT_SNAP &&
	      g_acts[1] == ST_FNPLAY_ACT_SLOW,
	      "a triple tap must be SNAP then a fresh SLOW");
}

/* ======================================================================
 * 8. THE COUNTER WRAPS. k_uptime_get() is 64-bit in main.c but this module
 *    takes uint32_t ms, which wraps every 49.7 days. A device left running
 *    must not get a stuck or spuriously-firing gesture at the wrap.
 * ====================================================================== */
static void case_wrap(void)
{
	g_cases++;
	printf("\n-- the millisecond counter wraps without breaking the gesture\n");

	st_fnplay_reset(&g_f);
	g_nact = 0;
	g_now  = 0xFFFFFF00u;      /* 256 ms before the wrap */

	tap(40u);
	advance(1000u);            /* carries the clock through zero */
	show();
	CHECK(g_nact == 1 && g_acts[0] == ST_FNPLAY_ACT_SLOW,
	      "a single tap straddling the wrap must still commit once");

	st_fnplay_reset(&g_f);
	g_nact = 0;
	g_now  = 0xFFFFFF00u;
	tap(40u);
	advance(120u);             /* the second tap lands after the wrap */
	tap(40u);
	advance(1000u);
	show();
	CHECK(g_nact == 1 && g_acts[0] == ST_FNPLAY_ACT_SNAP,
	      "a double straddling the wrap must still be one SNAP");
}

int main(void)
{
	printf("== Stem Tape FUNCTION + PLAY TAP GESTURE ==\n");
	printf("x1 slow / x2 snap home, %u ms decision window, %u ms tap ceiling\n",
	       ST_FNPLAY_DOUBLE_MS, ST_FNPLAY_TAP_MAX_MS);

	case_the_spec();
	case_double_never_emits_slow();
	case_the_window_is_the_firmwares();
	case_a_hold_is_not_a_tap();
	case_outside_the_window();
	case_commits_after_release();
	case_hold_cancels();
	case_idle_is_silent();
	case_third_tap_is_a_fresh_single();
	case_wrap();

	printf("\n");
	if (g_failures) {
		printf("FNPLAY TEST FAILED (%d cases, %d checks, %d failures)\n",
		       g_cases, g_checks, g_failures);
		return 1;
	}
	printf("FNPLAY TEST PASSED (%d cases, %d checks, 0 failures)\n",
	       g_cases, g_checks);
	printf("NOTE: this proves the ARBITRATION. That PLAY decodes at the top "
	       "of the AIN0 ladder while FUNCTION is held is a separate, "
	       "already-shipping question -- see main.c's fraw > 1600 band.\n");
	return 0;
}
