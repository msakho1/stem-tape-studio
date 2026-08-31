/*
 * st_fnplay.c -- see st_fnplay.h for the gesture, the companion spec it comes
 * from, why the single tap is held rather than fired optimistically, and why
 * the window is measured press-to-press.
 */

#include "st_fnplay.h"

void st_fnplay_reset(st_fnplay_t *f)
{
	f->pend     = false;
	f->pend_ms  = 0u;
	f->have_tap = false;
	f->last_ms  = 0u;
	f->down     = false;
	f->down_ms  = 0u;
	f->spent    = false;
}

void st_fnplay_cancel(st_fnplay_t *f)
{
	/* Same as a reset today, and deliberately a separate name: "this press
	 * was spent on something else" and "start from nothing" are different
	 * statements about the gesture, and only one of them is allowed to
	 * change if the window ever grows state. */
	st_fnplay_reset(f);
}

st_fnplay_action_t st_fnplay_press(st_fnplay_t *f, uint32_t now_ms)
{
	f->down    = true;
	f->down_ms = now_ms;
	f->spent   = false;

	/*
	 * A SECOND PRESS INSIDE THE WINDOW IS ONE GESTURE.
	 *
	 * The pending single is dropped here having never been emitted -- that
	 * is the entire reason it was held. Unsigned subtraction, so the
	 * comparison is correct across the millisecond counter's wrap.
	 */
	if (f->have_tap &&
	    (uint32_t)(now_ms - f->last_ms) <= ST_FNPLAY_DOUBLE_MS) {
		f->pend     = false;
		f->have_tap = false;
		f->spent    = true;   /* this press's release must not re-arm */
		return ST_FNPLAY_ACT_SNAP;
	}

	/*
	 * First press: remember it, but arm nothing yet. Whether it is a TAP is
	 * not known until the release -- a press that runs long is the mode or
	 * brightness gesture, and arming here would leave a single pending
	 * behind one of those.
	 */
	f->last_ms  = now_ms;
	f->have_tap = true;
	return ST_FNPLAY_ACT_NONE;
}

void st_fnplay_release(st_fnplay_t *f, uint32_t now_ms)
{
	const bool was_down = f->down;
	const bool spent    = f->spent;

	f->down  = false;
	f->spent = false;

	if (!was_down || spent) {
		return;
	}

	/* Too long to be a tap: it was a hold, and the hold gestures own it.
	 * Forget the press entirely so it cannot pair with a later tap. */
	if ((uint32_t)(now_ms - f->down_ms) >= ST_FNPLAY_TAP_MAX_MS) {
		f->pend     = false;
		f->have_tap = false;
		return;
	}

	/* A real tap. Hold it, timed from its PRESS edge, and see whether a
	 * second press arrives before the window closes. */
	f->pend    = true;
	f->pend_ms = f->down_ms;
}

st_fnplay_action_t st_fnplay_tick(st_fnplay_t *f, uint32_t now_ms)
{
	if (!f->pend ||
	    (uint32_t)(now_ms - f->pend_ms) <= ST_FNPLAY_DOUBLE_MS) {
		return ST_FNPLAY_ACT_NONE;
	}

	/* The window closed with no second press: it was a single after all.
	 * THE ONLY PLACE the slow toggle is ever emitted. */
	f->pend     = false;
	f->have_tap = false;
	return ST_FNPLAY_ACT_SLOW;
}
