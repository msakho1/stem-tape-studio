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

/*
 * ======================================================================
 * THE FADERS: MOVEMENT, AND WHY IT IS A DELTA RATHER THAN A DISPLACEMENT
 * ======================================================================
 * The four faders are continuous rails, so "in use" cannot mean "at some
 * position" -- it has to mean "a hand is moving one right now". They live here
 * rather than in main.c because the FIRST version of this lived in main.c,
 * was untested, and was wrong in a way that reproduced the very failure this
 * module exists to prevent.
 *
 * WHAT WAS WRONG. That version compared each fader against a reference that
 * chased the reading at 4 counts per sample, and called the fader in use while
 * the gap exceeded 32 counts. The reference was only advanced while FUNCTION
 * was held -- because that is the only time the poll runs -- so an ordinary
 * volume change made with FUNCTION UP left the reference stale by the whole
 * movement. A 500 -> 3000 fader move stranded a 2500-count gap that the
 * 4-counts-per-sample slew needed 617 polls to close, and one fader is polled
 * once per four control passes:
 *
 *     617 polls x 32 ms = 19.7 s of "fader in use"
 *     a 5.000 s FUNCTION-only hold would have taken 24.7 s
 *
 * which is the safety invariant broken by stale state -- st55's failure
 * wearing different clothes. It was found by auditing the wiring rather than
 * by a test, because the wiring had no test. Hence this module.
 *
 * WHAT IS RIGHT. Movement is the DELTA BETWEEN CONSECUTIVE SAMPLES of the same
 * fader, and the previous sample is re-seeded at the start of every hold:
 *
 *   noise            small deltas, below the threshold          -> not in use
 *   a hand moving    large deltas, sample after sample          -> in use
 *   the hand stops   deltas fall to zero                        -> not in use
 *                                                                  within
 *                                                                  ST_PWR_FADER_ACTIVE_MS
 *   a past movement  forgotten -- the seed is taken fresh       -> not in use
 *
 * The failure profile is now BOUNDED IN THE SAFE DIRECTION. An over-sensitive
 * threshold delays a shutdown by ST_PWR_FADER_ACTIVE_MS per noise event and
 * could only block one if noise exceeded the threshold continuously for the
 * whole five seconds. An under-sensitive one misses a movement slower than the
 * threshold, which costs protection during that move but never costs the
 * escape hatch. Neither can strand seconds of delay the way the displacement
 * version did.
 *
 * ST_PWR_FADER_MOVE_COUNTS IS STILL UNMEASURED -- postmortem measurement M3.
 * AIN0 and AIN1 have measured band tables behind them; the fader rails do not.
 * 16 counts of ~3700 travel, at one sample per ~32 ms, is about 500 counts per
 * second: comfortably above the few LSB a 12-bit rail with a 20 us acquisition
 * jitters, and reached by any deliberate move. Settle it once M3 exists.
 */
#define ST_PWR_FADERS            4
#define ST_PWR_FADER_MOVE_COUNTS 16
#define ST_PWR_FADER_ACTIVE_MS   250
#define ST_PWR_FADER_UNSEEDED    (-1)

typedef struct {
	int32_t last[ST_PWR_FADERS];  /* previous sample, or UNSEEDED */
	int64_t moved_ms;             /* uptime of the last movement, 0 = none */
} st_pwr_fader_t;

/*
 * Forget every fader. MUST be called on the rising edge of FUNCTION -- that is
 * what makes a movement made while FUNCTION was up unable to delay the hold
 * that follows it, and it is the whole of the fix described above.
 */
void st_pwr_fader_reset(st_pwr_fader_t *f);

/*
 * One fader sampled. `raw` < 0 means the ADC read failed and is ignored (the
 * previous sample is kept, so a dropped read is not a movement). Returns
 * whether ANY fader counts as in use right now.
 */
bool st_pwr_fader_sample(st_pwr_fader_t *f, uint32_t idx, int32_t raw,
			  int64_t now_ms);

/* The same verdict without sampling, for a caller that needs to ask twice in
 * one pass. */
bool st_pwr_fader_active(const st_pwr_fader_t *f, int64_t now_ms);

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
