/*
 * st_pitch.c -- see st_pitch.h for what was reused from the Tape Looper, what
 * was deliberately changed, and why the upward range is bounded by the eMMC
 * rather than by the pitch maths.
 */

#include "st_pitch.h"

/*
 * THE EQUAL-TEMPERED GRID, round(65536 * 2^(k/24)) for k = ST_PITCH_MIN_HALF
 * .. ST_PITCH_MAX_HALF -- half semitones, so index (k - MIN) is the rate for
 * k half-steps.
 *
 * SAME FORMULA AS THE LOOPER'S k_semi_q16, at half the step. Every whole
 * semitone here is therefore bit-identical to the Looper's entry for that
 * semitone: 0 -> 65536 (1.0x), -24 -> 32768 (0.5x), +2 -> 69433, and so on.
 * tests/test_pitch.c re-derives the Looper's whole table out of this one and
 * compares it entry by entry, so the two cannot drift.
 *
 * Generated and checked for monotonicity rather than hand-typed; a table like
 * this is exactly where a transposed digit hides.
 */
static const uint32_t k_pitch_q16[ST_PITCH_MAX_HALF - ST_PITCH_MIN_HALF + 1] = {
	  32768u,   33728u,   34716u,   35734u,   36781u,   37859u,
	  38968u,   40110u,   41285u,   42495u,   43740u,   45022u,
	  46341u,   47699u,   49097u,   50535u,   52016u,   53540u,
	  55109u,   56724u,   58386u,   60097u,   61858u,   63670u,
	  65536u,   67456u,   69433u,   71468u,   73562u,   75717u,
};

void st_pitch_reset(st_pitch_t *p)
{
	p->half     = 0;
	p->pend_dir = 0;
	p->pend_ms  = 0u;
}

/* Apply a step, clamped to the range. Returns true if the value moved. */
static bool step_by(st_pitch_t *p, int halves)
{
	int v = (int)p->half + halves;

	if (v < ST_PITCH_MIN_HALF) {
		v = ST_PITCH_MIN_HALF;
	} else if (v > ST_PITCH_MAX_HALF) {
		v = ST_PITCH_MAX_HALF;
	}
	if (v == (int)p->half) {
		return false;
	}
	p->half = (int16_t)v;
	return true;
}

st_pitch_action_t st_pitch_click(st_pitch_t *p, int dir, uint32_t now_ms)
{
	if (dir == 0) {
		return ST_PITCH_ACT_NONE;
	}
	dir = (dir > 0) ? 1 : -1;

	/*
	 * A SECOND CLICK THE SAME WAY, INSIDE THE WINDOW, IS ONE GESTURE.
	 *
	 * The pending single is dropped here WITHOUT EVER HAVING BEEN APPLIED
	 * -- that is the whole point of holding it. The step taken is the
	 * DOUBLE's full amount, not an increment on top of a single, so a
	 * double from 0.0 lands on +/-1.0 and never passes through +/-0.5.
	 */
	if (p->pend_dir == dir &&
	    (uint32_t)(now_ms - p->pend_ms) <= ST_PITCH_DOUBLE_MS) {
		p->pend_dir = 0;
		(void)step_by(p, dir * ST_PITCH_DOUBLE_HALF);
		return (dir > 0) ? ST_PITCH_ACT_DOUBLE_UP
				 : ST_PITCH_ACT_DOUBLE_DOWN;
	}

	/*
	 * A click in the OTHER direction cannot be half of a double, so the one
	 * already waiting is settled now rather than being left to expire on
	 * its own. Committing it immediately is what makes up-then-down behave
	 * as two separate single clicks instead of silently losing the first.
	 */
	if (p->pend_dir != 0) {
		const int8_t old = p->pend_dir;

		p->pend_dir = dir;
		p->pend_ms  = now_ms;
		(void)step_by(p, (int)old * ST_PITCH_SINGLE_HALF);
		return (old > 0) ? ST_PITCH_ACT_SINGLE_UP
				 : ST_PITCH_ACT_SINGLE_DOWN;
	}

	/* First click: hold it and see whether a second arrives. */
	p->pend_dir = (int8_t)dir;
	p->pend_ms  = now_ms;
	return ST_PITCH_ACT_NONE;
}

st_pitch_action_t st_pitch_tick(st_pitch_t *p, uint32_t now_ms)
{
	int dir;

	if (p->pend_dir == 0 ||
	    (uint32_t)(now_ms - p->pend_ms) <= ST_PITCH_DOUBLE_MS) {
		return ST_PITCH_ACT_NONE;
	}

	/* The window closed with no second click: it was a single after all.
	 * THE ONLY PLACE a single is ever applied. */
	dir = p->pend_dir;
	p->pend_dir = 0;
	(void)step_by(p, dir * ST_PITCH_SINGLE_HALF);
	return (dir > 0) ? ST_PITCH_ACT_SINGLE_UP : ST_PITCH_ACT_SINGLE_DOWN;
}

uint32_t st_pitch_ratio_q16(const st_pitch_t *p)
{
	int idx = (int)p->half - ST_PITCH_MIN_HALF;

	/* Defensive: the state is clamped everywhere it is written, but this
	 * indexes a table from a value the caller owns, in firmware with no
	 * MMU. Costs two compares on a once-per-block path. */
	if (idx < 0) {
		idx = 0;
	} else if (idx > ST_PITCH_MAX_HALF - ST_PITCH_MIN_HALF) {
		idx = ST_PITCH_MAX_HALF - ST_PITCH_MIN_HALF;
	}
	return k_pitch_q16[idx];
}
