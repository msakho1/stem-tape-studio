/*
 * test_track_hold.c — st_track_hold.c: pure momentary hold-to-solo
 * classifier, host-tested.
 *
 * CORRECTION (Phase 3 control-matrix): replaces the first version's
 * release-time toggle (a latch) with the momentary behavior actually
 * specified -- see st_track_hold.h's own doc comment. These tests drive
 * the pure tick() function directly with fabricated (held, pressed)
 * sequences representing physical press/hold/release timelines -- no
 * fabricated AUDIO content is involved (st_track_hold.h has no audio
 * concept at all), so this is the standard, necessary way to test a pure
 * timing state machine, not the fabrication this suite's own
 * non-fabrication rule forbids.
 *
 *     cc -std=c11 -Wall -Wextra -I../src \
 *        ../src/st_track_hold.c test_track_hold.c \
 *        -o test_track_hold && ./test_track_hold
 *
 * Does not need the repository-root working directory (no fixture
 * files) -- included for consistency with this suite's other test
 * binaries' own doc comments.
 */

#include <stdio.h>

#include "st_track_hold.h"

static int g_checks;
static int g_failures;
static int g_test_cases;

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
#define RUN(fn) do { g_test_cases++; fn(); } while (0)

#define THRESH 700u

/* SHORT TAP: pressed, released well before the threshold -- never active,
 * at any point, and stays inactive on release (main.c reads this as "not
 * solo", falls through to its own unchanged tap-to-mute logic). */
static void test_short_tap_never_activates(void)
{
	st_track_hold_t s;

	st_track_hold_reset(&s);

	bool a1 = st_track_hold_tick(&s, true, 0, THRESH);
	bool a2 = st_track_hold_tick(&s, true, 40, THRESH);
	bool a3 = st_track_hold_tick(&s, true, 120, THRESH);
	bool a4 = st_track_hold_tick(&s, false, 0, THRESH); /* released at 120 ms */

	CHECK(!a1 && !a2 && !a3, "short tap: never reports active while held well under threshold");
	CHECK(!a4, "short tap: not active on the release pass either");
	CHECK(!s.solo_active, "short tap: solo_active is false after release");
}

/* THRESHOLD CROSSING: active becomes true on the EXACT pass held_ms first
 * reaches threshold_ms, not one pass early, not one pass late. */
static void test_threshold_crossing_is_exact(void)
{
	st_track_hold_t s;

	st_track_hold_reset(&s);

	bool before = st_track_hold_tick(&s, true, THRESH - 1u, THRESH);
	bool at = st_track_hold_tick(&s, true, THRESH, THRESH);
	bool after = st_track_hold_tick(&s, true, THRESH + 250u, THRESH);

	CHECK(!before, "threshold crossing: still inactive one ms before the threshold");
	CHECK(at, "threshold crossing: active on the exact pass held_ms == threshold_ms");
	CHECK(after, "threshold crossing: stays active on later passes while still held");
}

/* LONG RELEASE: held well past the threshold (active), then released --
 * must go inactive on that exact release pass, and the caller's
 * mute-suppression read (checking solo_active BEFORE this pass, per
 * main.c's own documented ordering) would have seen it true. */
static void test_long_hold_then_release(void)
{
	st_track_hold_t s;

	st_track_hold_reset(&s);

	(void)st_track_hold_tick(&s, true, 800, THRESH);
	bool was_active_before_release = s.solo_active; /* what main.c's release handler reads */
	bool release_result = st_track_hold_tick(&s, false, 0, THRESH);

	CHECK(was_active_before_release, "long hold: solo_active is true the pass before release (this is what "
					  "main.c's release handler reads to suppress mute)");
	CHECK(!release_result, "long hold: tick() itself reports inactive on the release pass");
	CHECK(!s.solo_active, "long hold: solo_active is false immediately after release");
}

/* MUTE PRESERVATION: a short tap must never leave solo_active true, so
 * main.c's own unchanged tap-to-mute logic is free to run unsuppressed --
 * this is the same short-tap scenario expressed as "the release-time read
 * a mute-suppression decision would use is false". */
static void test_short_tap_never_suppresses_mute(void)
{
	st_track_hold_t s;

	st_track_hold_reset(&s);

	(void)st_track_hold_tick(&s, true, 50, THRESH);
	bool would_suppress_mute_at_release = s.solo_active;

	(void)st_track_hold_tick(&s, false, 0, THRESH);

	CHECK(!would_suppress_mute_at_release,
	      "mute preservation: a short (well under threshold) hold never leaves solo_active true, so "
	      "main.c's tap-to-mute branch is never suppressed");
}

/* MULTIPLE HELD SOLOS: independent instances (one per track, per
 * st_track_hold.h's own doc comment) never interact -- two held past
 * threshold simultaneously are BOTH active at once, and releasing one
 * does not affect the other. */
static void test_multiple_independent_instances_can_both_be_active(void)
{
	st_track_hold_t track_a, track_b;

	st_track_hold_reset(&track_a);
	st_track_hold_reset(&track_b);

	bool a_active = st_track_hold_tick(&track_a, true, 800, THRESH);
	bool b_active = st_track_hold_tick(&track_b, true, 900, THRESH);

	CHECK(a_active && b_active, "multi-solo: two independent instances held past threshold are both active "
				     "at the same time");

	/* Release track_a only; track_b, still held, must be unaffected. */
	bool a_after_release = st_track_hold_tick(&track_a, false, 0, THRESH);
	bool b_still_active = st_track_hold_tick(&track_b, true, 950, THRESH);

	CHECK(!a_after_release, "multi-solo: releasing track_a clears only track_a");
	CHECK(b_still_active, "multi-solo: track_b, still physically held, is unaffected by track_a's release");
}

/* RELEASE ORDERING: releasing track_b first, then track_a, clears each
 * independently in whichever order releases actually happen -- order of
 * release must not matter to the OTHER instance's own state. */
static void test_release_ordering_is_independent(void)
{
	st_track_hold_t track_a, track_b;

	st_track_hold_reset(&track_a);
	st_track_hold_reset(&track_b);

	(void)st_track_hold_tick(&track_a, true, 800, THRESH);
	(void)st_track_hold_tick(&track_b, true, 800, THRESH);

	/* Release track_b first. */
	bool b_released = st_track_hold_tick(&track_b, false, 0, THRESH);

	CHECK(!b_released, "release ordering: track_b clears on its own release");
	CHECK(track_a.solo_active, "release ordering: track_a is untouched by track_b's release");

	/* Then release track_a. */
	bool a_released = st_track_hold_tick(&track_a, false, 0, THRESH);

	CHECK(!a_released, "release ordering: track_a clears on its own, later release");
	CHECK(!track_a.solo_active && !track_b.solo_active,
	      "release ordering: both end inactive regardless of which released first");
}

/* MODE CHANGE / REBOOT: reset() always yields inactive, whether coming
 * from a fresh instance or one that was mid-hold -- "no solo state
 * persists across... mode change or reboot". */
static void test_reset_always_clears_even_mid_hold(void)
{
	st_track_hold_t s;

	st_track_hold_reset(&s);
	(void)st_track_hold_tick(&s, true, 900, THRESH); /* mid-hold, active */
	CHECK(s.solo_active, "reset test setup: confirm active before reset");

	st_track_hold_reset(&s);
	CHECK(!s.solo_active, "mode change / reboot: reset() clears solo_active even if it was active mid-hold");
}

int main(void)
{
	RUN(test_short_tap_never_activates);
	RUN(test_threshold_crossing_is_exact);
	RUN(test_long_hold_then_release);
	RUN(test_short_tap_never_suppresses_mute);
	RUN(test_multiple_independent_instances_can_both_be_active);
	RUN(test_release_ordering_is_independent);
	RUN(test_reset_always_clears_even_mid_hold);

	printf("\n%d distinct test cases, %d assertion checks\n", g_test_cases, g_checks);
	if (g_failures) {
		printf("TRACK HOLD TEST FAILED (%d/%d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("TRACK HOLD TEST PASSED (%d test cases, %d checks)\n", g_test_cases, g_checks);
	return 0;
}
