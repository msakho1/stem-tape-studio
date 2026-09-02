/*
 * st_pwr_hold.h -- THE ESCAPE HATCH, and the only thing that decides it.
 *
 * ======================================================================
 * WHY THIS MODULE EXISTS AT ALL
 * ======================================================================
 * The st55 scratch build made the device's software power-off unreachable, and
 * the unit could not be switched off until its battery drained. The mechanism
 * was not that anything blocked power_off(). It was that the 2.5 s timer lived
 * INSIDE a branch a feature flag guarded:
 *
 *     if (pwr_pressed() && !g_stem_ctl_out.function_consumed) {
 *             if (press_start < 0) press_start = k_uptime_get();
 *             ...
 *             if (k_uptime_get() - press_start >= HOLD_MS_TO_OFF) power_off();
 *     }
 *     if (press_start >= 0) { ... press_start = -1; }   <-- timer destroyed
 *
 * so a flag going true mid-hold did not pause the countdown, it deleted the
 * variable the countdown was made of -- and then fell through to the "just
 * released" branch while the button was still physically down. See
 * docs/postmortems/2026-09-scratch-series.md, section 2.1.
 *
 * This module exists so that shape is unrepresentable. It owns the hold, it is
 * called unconditionally, and its signature has nowhere to put a feature flag.
 *
 * ======================================================================
 * RESET, NEVER SUPPRESS -- the whole correction, in one rule
 * ======================================================================
 * st54 already suppresses the hold in more places than the postmortem's
 * headline one. `combo_seen` is set by FUNCTION+PLAY, by the FUNCTION+Track
 * bank jump and by the tap-run grid clear, and its own comment calls that
 * "POWER-OFF SAFETY": once set, the branch `continue`s past the shutdown for
 * THE REST OF THAT PRESS. The intent is sound -- a chord means something else,
 * and should not also power the device off mid-gesture. The implementation is
 * not: it is a LATCH, so it keeps suppressing long after the other button has
 * been let go, and the player is left holding FUNCTION with nothing happening.
 *
 * The distinction this module draws instead:
 *
 *     another input is DOWN RIGHT NOW  ->  RESET the timer
 *     a feature "consumed" the press   ->  IRRELEVANT, not an input
 *
 * A chord therefore still cannot power the device off: while PLAY or a Track
 * or a VOLUME button is held, the timer is not merely paused, it is back at
 * zero. But the moment the player lets go of everything except FUNCTION, the
 * timer starts, and 2.5 s later the device is off. No latch, no flag and no
 * amount of feature state can extend that beyond 2.5 s from the last release.
 *
 * THAT is the guarantee: not "FUNCTION down for 2.5 s always powers off"
 * (which would shut the device down in the middle of a legitimate 3-second
 * FUNCTION+PLAY mode toggle), but "the device is never more than 2.5 s of a
 * clean FUNCTION hold away from off, from ANY state it can reach."
 *
 * ======================================================================
 * THE INPUT IS NOT ON THE CONTESTED RAIL
 * ======================================================================
 * FUNCTION is its own GPIO -- main.c's pwr_pressed() is a single register read
 * of PWR_PORT->IN, with no ADC, no BTN_COM, no conversion. That matters after
 * st55: the phantom Vocal solo came from converter traffic coupling into the
 * shared ladder rail, and the escape hatch is deliberately not reachable by
 * that class of fault. `other_down` IS derived from the ladder, but only ever
 * to RESET -- rail noise can therefore delay a shutdown by one pass, never
 * prevent one, because the noise would have to persist continuously.
 *
 * PURE: no Zephyr, no GPIO, no clock of its own. `now_ms` is the only time
 * source, which is what lets tests/test_power_hold.c drive it exhaustively.
 */

#ifndef STEMTAPE_PLAYER_PWR_HOLD_H_
#define STEMTAPE_PLAYER_PWR_HOLD_H_

#include <stdbool.h>
#include <stdint.h>

/* Hold this long, cleanly, and the device powers off. THE one definition;
 * main.c's HOLD_MS_TO_OFF is taken from here so the two cannot drift. */
#define ST_PWR_HOLD_MS 2500

/* since_ms when no clean hold is in progress. */
#define ST_PWR_HOLD_IDLE ((int64_t)-1)

typedef struct {
	/* When the current CLEAN hold began, or ST_PWR_HOLD_IDLE. This is the
	 * entire state of the escape hatch, and nothing outside this file's own
	 * two functions may write it. */
	int64_t since_ms;
} st_pwr_hold_t;

/* No hold in progress. Call once at boot; safe to call any time. */
static inline void st_pwr_hold_reset(st_pwr_hold_t *h)
{
	h->since_ms = ST_PWR_HOLD_IDLE;
}

/*
 * ONE PASS. Returns true exactly once the clean hold has reached
 * ST_PWR_HOLD_MS -- the caller's ONLY correct response to which is to power
 * the device off, immediately, without consulting anything else.
 *
 * THE ARGUMENT LIST IS THE SAFETY PROPERTY, so read it as a contract rather
 * than as parameters:
 *
 *   fn_down     the FUNCTION button, straight off its own GPIO. Not a
 *               debounced view, not a dispatcher's opinion of it, not a flag
 *               anything else has had a chance to write.
 *   other_down  whether ANY other physical control is down right now. Resets
 *               the timer; never suppresses it, never latches.
 *   now_ms      monotonic milliseconds.
 *
 * There is deliberately no fourth parameter, and there never may be. A feature
 * that wants to influence shutdown has nowhere to say so -- which is the point,
 * and is what CI asserts about the call site rather than trusting to review.
 *
 * Idempotent while held: it keeps returning true every pass past the
 * threshold, so a caller that somehow misses one pass still shuts down on the
 * next rather than needing the player to press again.
 */
bool st_pwr_hold_tick(st_pwr_hold_t *h, bool fn_down, bool other_down,
		       int64_t now_ms);

/*
 * How long the current clean hold has run, in ms; 0 when none is. For the
 * countdown the LEDs draw -- an observation, never an input to the decision.
 */
int64_t st_pwr_hold_elapsed_ms(const st_pwr_hold_t *h, int64_t now_ms);

#endif /* STEMTAPE_PLAYER_PWR_HOLD_H_ */
