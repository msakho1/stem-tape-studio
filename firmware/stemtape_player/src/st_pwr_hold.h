/*
 * st_pwr_hold.h -- THE ONE TIMER. Continuous FUNCTION-only hold duration.
 *
 * ======================================================================
 * WHAT THIS TIMER MEANS, AND IT MEANS ONLY THIS
 * ======================================================================
 *
 *     how long FUNCTION has been held with EVERY OTHER PHYSICAL CONTROL
 *     INACTIVE, continuously, without interruption.
 *
 * Not "how long FUNCTION has been down". Not "how long since the last
 * blocker cleared". Not a count that pauses and resumes. The instant any
 * other physical control becomes active the elapsed time is ZERO, and when
 * that control is released the count starts again from ZERO.
 *
 * Two thresholds read the same timer:
 *
 *     ST_PWR_ON_MS   2000    FUNCTION-only for 2.000 s  ->  ON
 *     ST_PWR_OFF_MS  5000    FUNCTION-only for 5.000 s  ->  OFF
 *
 * ONE TIMER, TWO THRESHOLDS, and the countdown the LEDs draw reads the same
 * elapsed value -- never a second clock that can disagree with the one that
 * actually fires. st_pwr_hold_tick() returns the elapsed time rather than a
 * verdict for exactly that reason.
 *
 * ======================================================================
 * THE SAFETY INVARIANT
 * ======================================================================
 *
 *     FUNCTION only, for 5.000 continuous seconds, powers the device off
 *     FROM EVERY REACHABLE FIRMWARE STATE.
 *
 * No feature flag, latch, dispatcher state, combo state, solo state, FX
 * state, reverse state, scratch state or transport state may block it,
 * delay it, or shorten it. This module cannot be handed any of those: its
 * signature takes two physical facts and a clock, and there is no fourth
 * parameter -- which is the structural half of the guarantee. The other
 * half is that main.c calls it unconditionally, above every dispatcher,
 * and CI asserts both.
 *
 * st55 made the device impossible to switch off, and the mechanism was not
 * that anything blocked power_off(): the timer lived inside a branch a
 * feature flag guarded, so the flag deleted the variable the countdown was
 * made of and then fell through to the release path with the button still
 * down. docs/postmortems/2026-09-scratch-series.md, section 2.1.
 *
 * ======================================================================
 * "EVERY OTHER PHYSICAL CONTROL" IS A MAP, NOT A CONVENIENT SUBSET
 * ======================================================================
 * An earlier version of this module took "settled AIN0 only", excluding the
 * AIN1 rail because it is noisy. That was the wrong kind of reasoning: it
 * defined the safety rule by what was convenient to sample, which creates
 * exactly the edge case the rule exists to remove -- FUNCTION + rocker is a
 * real performance interaction, and a timer that cannot see the rocker would
 * shut the device down underneath it.
 *
 * The semantic rule is: ANY INTENTIONAL PHYSICAL CONTROL INTERACTION BESIDES
 * FUNCTION RESETS THE TIMER. The implementation may debounce and filter
 * however each rail needs, but it may not omit a control because filtering it
 * is awkward. The full map on this hardware:
 *
 *   FUNCTION      dedicated GPIO (PWR_PORT/PWR_PIN)     -- the timer's subject
 *   AIN0 ladder   PLAY + Track 1..4, 15 chord masks     -- measured bands,
 *                                                          settled by st_ladder
 *   AIN1 ladder   VOL down/up + rocker FWD/RWD          -- measured bands,
 *                                                          settled HERE
 *   AIN3/6/2/7    four faders, continuous 0..~3700      -- movement-detected;
 *                                                          see the caller
 *   AIN4          battery divider                       -- not a control
 *
 * `other_raw` is the OR of everything in that map except FUNCTION. This module
 * then settles it, so a single noisy sample on any rail cannot reset a hold
 * that is nearly complete, and a real press resets it within
 * ST_PWR_SETTLE_PASSES passes.
 *
 * ======================================================================
 * WHY THE SETTLE IS SYMMETRIC, and what each direction costs
 * ======================================================================
 * Both failure directions are real and they pull opposite ways:
 *
 *   too eager to call a control ACTIVE  ->  noise resets the timer forever,
 *                                            shutdown unreachable. This is
 *                                            the st55 class of bug, arriving
 *                                            through a different door.
 *   too slow to call a control ACTIVE   ->  the device powers off in the
 *                                            middle of a gesture.
 *
 * So neither edge is taken on one sample. ST_PWR_SETTLE_PASSES agreeing reads
 * commit a change in either direction -- the same discipline st_ladder already
 * applies to the Track rail, for the same reason. At main.c's ~8 ms pass that
 * is ~16 ms: three orders of magnitude inside the 5 s threshold, and far
 * shorter than any real button press.
 *
 * PURE: no Zephyr, no GPIO, no clock of its own. `now_ms` is the only time
 * source, which is what lets tests/test_power_hold.c drive every case above
 * exactly, to the millisecond.
 */

#ifndef STEMTAPE_PLAYER_PWR_HOLD_H_
#define STEMTAPE_PLAYER_PWR_HOLD_H_

#include <stdbool.h>
#include <stdint.h>

/* FUNCTION-only, continuously, for this long -> the device turns ON. */
#define ST_PWR_ON_MS  2000

/* FUNCTION-only, continuously, for this long -> the device turns OFF. */
#define ST_PWR_OFF_MS 5000

/* Agreeing passes that commit a change in the "any other control is active"
 * verdict, in EITHER direction. See the header's own note on why neither edge
 * is taken on a single sample. */
#define ST_PWR_SETTLE_PASSES 2u

/* since_ms when no clean FUNCTION-only hold is in progress. */
#define ST_PWR_HOLD_IDLE ((int64_t)-1)

typedef struct {
	/* When the current clean FUNCTION-only hold began, or ST_PWR_HOLD_IDLE.
	 * The entire authoritative state of both power transitions. */
	int64_t since_ms;

	/* The settled verdict, and the candidate working toward changing it. */
	bool    other_active;
	bool    cand;
	uint8_t cand_n;
} st_pwr_hold_t;

/* No hold in progress, nothing settled. Call once at boot; safe any time. */
void st_pwr_hold_reset(st_pwr_hold_t *h);

/*
 * ONE PASS. Returns the CONTINUOUS FUNCTION-ONLY HOLD DURATION in ms -- 0
 * whenever FUNCTION is up or any other control is (settled) active.
 *
 * THE ARGUMENT LIST IS THE SAFETY PROPERTY. Read it as a contract:
 *
 *   fn_down    FUNCTION, straight off its own GPIO. Not a debounced view of
 *              it, not a dispatcher's opinion of it, not a flag anything else
 *              has had a chance to write.
 *   other_raw  this pass's raw verdict for EVERY other physical control in
 *              the map above. Settled here, not by the caller.
 *   now_ms     monotonic milliseconds.
 *
 * There is deliberately no fourth parameter and there never may be. A feature
 * that wants to influence the power transitions has nowhere to say so.
 */
int64_t st_pwr_hold_tick(st_pwr_hold_t *h, bool fn_down, bool other_raw,
			  int64_t now_ms);

/* The same elapsed value without advancing anything -- for the countdown the
 * LEDs draw. An observation of the authoritative timer, never a second one. */
int64_t st_pwr_hold_elapsed_ms(const st_pwr_hold_t *h, int64_t now_ms);

/* The two verdicts, both read from the one timer. Written as helpers rather
 * than left to each caller's own `>=`, so the thresholds are compared in
 * exactly one place each. */
static inline bool st_pwr_hold_off_due(int64_t elapsed_ms)
{
	return elapsed_ms >= ST_PWR_OFF_MS;
}

static inline bool st_pwr_hold_on_due(int64_t elapsed_ms)
{
	return elapsed_ms >= ST_PWR_ON_MS;
}

#if !defined(__cplusplus)
_Static_assert(ST_PWR_ON_MS < ST_PWR_OFF_MS,
	       "turning on must take less than turning off, or the on-hold "
	       "would always reach the off threshold too");
#endif

#endif /* STEMTAPE_PLAYER_PWR_HOLD_H_ */
