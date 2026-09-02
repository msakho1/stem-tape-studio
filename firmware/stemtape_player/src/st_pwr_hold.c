/*
 * st_pwr_hold.c -- see st_pwr_hold.h for what the timer means, why its
 * argument list is the safety property, and why "every other physical
 * control" is a map rather than a convenient subset.
 */

#include "st_pwr_hold.h"

void st_pwr_hold_reset(st_pwr_hold_t *h)
{
	h->since_ms     = ST_PWR_HOLD_IDLE;
	h->other_active = false;
	h->cand         = false;
	h->cand_n       = 0u;
}

int64_t st_pwr_hold_tick(st_pwr_hold_t *h, bool fn_down, bool other_raw,
			  int64_t now_ms)
{
	/*
	 * ---- SETTLE THE OTHER CONTROLS, BOTH DIRECTIONS -------------------
	 *
	 * A reading that agrees with the settled verdict clears the candidate:
	 * evidence for what we already believe is not progress toward changing
	 * it. A reading that disagrees builds a candidate, and only
	 * ST_PWR_SETTLE_PASSES agreeing disagreements commit.
	 *
	 * Neither edge is single-sample, and that is deliberate in both
	 * directions -- see the header. One noisy sample cannot reset a hold
	 * that is 4.99 s along; one dropped sample cannot power the device off
	 * under a finger that is still on a button.
	 */
	if (other_raw == h->other_active) {
		h->cand_n = 0u;
	} else if (other_raw == h->cand) {
		if (++h->cand_n >= ST_PWR_SETTLE_PASSES) {
			h->other_active = h->cand;
			h->cand_n       = 0u;
		}
	} else {
		h->cand   = other_raw;
		h->cand_n = 1u;
	}

	/*
	 * ---- THE TIMER MEANS ONE THING -----------------------------------
	 *
	 * ZERO, not paused. The instant FUNCTION comes up, or any other
	 * control is (settled) active, the elapsed time is gone -- not banked,
	 * not resumed. When the other control is released the count starts
	 * again from zero and the full threshold must elapse afresh.
	 *
	 * That is what lets a player hold FUNCTION through PLAY, a Track, the
	 * rocker, volume, FX, a grid clear or any future musical gesture for
	 * as long as they like without the device shutting down underneath
	 * them -- and it is also why no amount of prior gesture can shorten
	 * the hold that does shut it down.
	 */
	if (!fn_down || h->other_active) {
		h->since_ms = ST_PWR_HOLD_IDLE;
		return 0;
	}

	if (h->since_ms == ST_PWR_HOLD_IDLE) {
		h->since_ms = now_ms;
	}

	/*
	 * Subtraction of two monotonic int64 millisecond stamps: no wrap to
	 * reason about within any plausible uptime. Clamped at zero so a
	 * caller that passes a `now_ms` behind the stamp gets "no hold yet"
	 * rather than a negative duration that would compare oddly against a
	 * threshold.
	 */
	return (now_ms > h->since_ms) ? (now_ms - h->since_ms) : 0;
}

int64_t st_pwr_hold_elapsed_ms(const st_pwr_hold_t *h, int64_t now_ms)
{
	if (h->since_ms == ST_PWR_HOLD_IDLE) {
		return 0;
	}
	return (now_ms > h->since_ms) ? (now_ms - h->since_ms) : 0;
}

/* ---- the faders: see st_pwr_hold.h for why this is a delta ------------- */

void st_pwr_fader_reset(st_pwr_fader_t *f)
{
	uint32_t k;

	for (k = 0; k < ST_PWR_FADERS; k++) {
		f->last[k] = ST_PWR_FADER_UNSEEDED;
	}
	f->moved_ms = 0;
}

bool st_pwr_fader_active(const st_pwr_fader_t *f, int64_t now_ms)
{
	return f->moved_ms != 0 &&
	       (now_ms - f->moved_ms) < ST_PWR_FADER_ACTIVE_MS;
}

bool st_pwr_fader_sample(st_pwr_fader_t *f, uint32_t idx, int32_t raw,
			  int64_t now_ms)
{
	if (idx < ST_PWR_FADERS && raw >= 0) {
		if (f->last[idx] == ST_PWR_FADER_UNSEEDED) {
			/* THE FIRST SAMPLE OF A HOLD IS NEVER A MOVEMENT. It
			 * establishes where the fader is; only what happens
			 * afterwards is a hand. */
			f->last[idx] = raw;
		} else {
			int32_t d = raw - f->last[idx];

			if (d < 0) {
				d = -d;
			}
			if (d >= ST_PWR_FADER_MOVE_COUNTS) {
				f->moved_ms = now_ms;
			}
			/* The previous sample is ALWAYS updated, movement or
			 * not. That is what stops a gap accumulating -- the
			 * defect the header describes. */
			f->last[idx] = raw;
		}
	}
	return st_pwr_fader_active(f, now_ms);
}

/* ---- the transaction: see st_pwr_hold.h ------------------------------- */

static void st_pwr_init(st_pwr_t *p, bool device_on)
{
	st_pwr_hold_reset(&p->hold);
	st_pwr_fader_reset(&p->fader);
	p->fn_prev   = false;
	p->device_on = device_on;

	/*
	 * DISARMED, IN BOTH ENTRY POINTS, DELIBERATELY.
	 *
	 * This is the only place either initialiser could have granted the arm,
	 * and it does not. Shutdown becomes possible in exactly one way -- by
	 * st_pwr_service() observing FUNCTION physically UP -- so a boot under a
	 * held finger owes that finger's release just as a boot caused by one
	 * does, and no re-init anywhere can be used to fake it.
	 *
	 * From a cold boot with the button up this costs one control pass.
	 */
	p->off_armed = false;
}

void st_pwr_init_off(st_pwr_t *p) { st_pwr_init(p, false); }
void st_pwr_init_on(st_pwr_t *p)  { st_pwr_init(p, true); }

void st_pwr_service(st_pwr_t *p, const st_pwr_in_t *in, st_pwr_out_t *out)
{
	bool other_raw;

	/*
	 * THE RISING EDGE IS THE WHOLE OF THE TRANSACTION RULE.
	 *
	 * Beginning a hold means forgetting everything that preceded it:
	 * st_pwr_hold_reset() drops elapsed AND the settled verdict AND its
	 * candidate, st_pwr_fader_reset() drops every baseline so the next
	 * sample of each fader re-seeds rather than comparing against a
	 * position from a previous hold. Nothing from before this edge can
	 * extend, suppress, pause, restart or poison what follows.
	 */
	out->began = (in->fn_down && !p->fn_prev);
	if (out->began) {
		st_pwr_hold_reset(&p->hold);
		st_pwr_fader_reset(&p->fader);
	}

	/*
	 * ---- THE ONLY LINE IN THIS PROJECT THAT ARMS SHUTDOWN --------------
	 *
	 * A physical FUNCTION release, observed here, and nothing else. Not an
	 * initialiser, not a reset, not a feature flag, not a combo, not the
	 * dispatcher, not a boot-duration guess. One press therefore causes at
	 * most one power transition: the press that turned the device on is
	 * still down, so it is still disarmed, no matter how long it is held.
	 *
	 * CI asserts that this assignment appears exactly once in this file and
	 * that it sits inside this branch.
	 */
	if (!in->fn_down) {
		p->off_armed = true;
	}

	p->fn_prev = in->fn_down;

	/*
	 * THE FADERS ARE ONLY SAMPLED INSIDE A TRANSACTION. Outside one there
	 * is nothing to protect and nothing to remember -- and a baseline kept
	 * across the gap is exactly the stale state this design forbids.
	 */
	if (in->fn_down) {
		(void)st_pwr_fader_sample(&p->fader, in->fader_idx,
					   in->fader_raw, in->now_ms);
	}

	other_raw = in->ain0_active || in->ain1_active ||
		     (in->fn_down && st_pwr_fader_active(&p->fader, in->now_ms));

	out->elapsed_ms   = st_pwr_hold_tick(&p->hold, in->fn_down, other_raw,
					      in->now_ms);
	out->other_active = p->hold.other_active;

	/*
	 * ---- THE STATE MACHINE --------------------------------------------
	 *
	 * The two thresholds are not two independent comparisons on one clock;
	 * they belong to different states, and only one of them can be reached
	 * at a time. That is what makes ON -> OFF under a single press
	 * structurally impossible rather than merely unlikely: while the device
	 * is OFF the off threshold is not consulted at all, and the instant it
	 * becomes ON the guard below is already false.
	 */
	out->on_due  = false;
	out->off_due = false;

	if (!p->device_on) {
		if (st_pwr_hold_on_due(out->elapsed_ms)) {
			p->device_on = true;

			/* THE PRESS THAT TURNED IT ON OWES A RELEASE. It is
			 * still down right now -- that is what completing the
			 * hold means -- so the arm cannot be granted until the
			 * finger physically leaves the button. */
			p->off_armed = false;
			out->on_due  = true;   /* an EDGE: one pass only */
		}
	} else {
		out->off_due = p->off_armed &&
			        st_pwr_hold_off_due(out->elapsed_ms);
	}

	out->device_on = p->device_on;
	out->off_armed = p->off_armed;
}
