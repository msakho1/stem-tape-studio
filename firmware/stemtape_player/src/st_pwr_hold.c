/*
 * st_pwr_hold.c -- see st_pwr_hold.h for why the escape hatch is its own
 * module and why its argument list is the safety property.
 */

#include "st_pwr_hold.h"

bool st_pwr_hold_tick(st_pwr_hold_t *h, bool fn_down, bool other_down,
		       int64_t now_ms)
{
	/*
	 * RESET, NEVER SUPPRESS. Both of these are facts about what is
	 * PHYSICALLY down right now, so neither can outlive the finger that
	 * caused it -- which is the entire difference between this and the
	 * `combo_seen` / `function_consumed` latches it replaces. A player who
	 * lets go of everything but FUNCTION is always at most ST_PWR_HOLD_MS
	 * from off, from any state the firmware can be in.
	 *
	 * `other_down` resetting rather than pausing is deliberate too: pausing
	 * would let a chord accumulate hold time across its own duration, so
	 * releasing PLAY after a three-second FUNCTION+PLAY toggle would power
	 * the device off instantly. Restarting the clock makes the shutdown
	 * gesture always a full, deliberate, unambiguous 2.5 s.
	 */
	if (!fn_down || other_down) {
		h->since_ms = ST_PWR_HOLD_IDLE;
		return false;
	}

	if (h->since_ms == ST_PWR_HOLD_IDLE) {
		h->since_ms = now_ms;
		/* A hold that begins exactly at the threshold is still a hold
		 * that has lasted zero milliseconds. Falling through to the
		 * comparison below would be correct too -- 0 >= 2500 is false
		 * -- and it is left to fall through rather than returning
		 * early, so there is ONE exit that decides shutdown. */
	}

	/*
	 * IDEMPOTENT PAST THE THRESHOLD, not edge-triggered. A caller that
	 * misses a pass -- a long eMMC read, a USB transfer, anything -- shuts
	 * down on the next one instead of requiring the player to release and
	 * press again. The escape hatch does not get to be a one-shot.
	 *
	 * Subtraction of two monotonic int64 millisecond stamps, so there is no
	 * wrap to reason about within any plausible uptime.
	 */
	return (now_ms - h->since_ms) >= ST_PWR_HOLD_MS;
}

int64_t st_pwr_hold_elapsed_ms(const st_pwr_hold_t *h, int64_t now_ms)
{
	if (h->since_ms == ST_PWR_HOLD_IDLE) {
		return 0;
	}
	return (now_ms >= h->since_ms) ? (now_ms - h->since_ms) : 0;
}
