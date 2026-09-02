/*
 * st_scratch.c -- the signed-head transport primitive. See st_scratch.h for
 * why scratch, scrub and reverse are one state machine and not three.
 */

#include "st_scratch.h"

/* Saturating add/subtract toward a target, in Q16. Written once because both
 * directions and both ramps use it, and because a walk that overshoots its
 * target and comes back is a wobble the hand would feel. */
static int32_t walk_toward(int32_t cur, int32_t target, int32_t step)
{
	if (step <= 0) {
		return cur;
	}
	if (target > cur) {
		return (target - cur <= step) ? target : cur + step;
	}
	if (target < cur) {
		return (cur - target <= step) ? target : cur - step;
	}
	return cur;
}

void st_scratch_begin(st_scratch_t *s, int32_t from_rate_q16, uint32_t max_rate_q16)
{
	s->max_rate_q16 = (int32_t)max_rate_q16;

	/*
	 * GRABBING A MOVING TAPE STARTS FROM ITS MOTION. A hand landing on a
	 * spinning record does not stop it dead, and starting the integrator at
	 * zero would be exactly that -- an instantaneous drop to a standstill,
	 * heard as a click, on every FUNCTION press regardless of what the
	 * player then did.
	 *
	 * The incoming rate can legitimately exceed this target's clamp: the
	 * transport may have been at 2x pitch when four heads were locked and
	 * afforded it, while the gesture about to start moves all four and
	 * cannot. Clamp on entry rather than refuse, so the grab is always
	 * possible and the eMMC bound is still honoured from the first tick.
	 */
	if (from_rate_q16 > s->max_rate_q16) {
		from_rate_q16 = s->max_rate_q16;
	} else if (from_rate_q16 < -s->max_rate_q16) {
		from_rate_q16 = -s->max_rate_q16;
	}
	s->rate_q16  = from_rate_q16;
	s->drive_q16 = 0;
	s->engaged   = true;
	s->coasting  = false;   /* re-grabbing mid-coast cancels it */
}

void st_scratch_set_drive(st_scratch_t *s, int32_t drive_q16)
{
	if (drive_q16 > ST_SCRATCH_DRIVE_FULL) {
		drive_q16 = ST_SCRATCH_DRIVE_FULL;
	} else if (drive_q16 < -ST_SCRATCH_DRIVE_FULL) {
		drive_q16 = -ST_SCRATCH_DRIVE_FULL;
	}
	s->drive_q16 = drive_q16;
}

void st_scratch_tick(st_scratch_t *s, uint32_t dt_us)
{
	int32_t target;
	uint32_t ramp_ms;
	int32_t step;

	if (!s->engaged || dt_us == 0u) {
		return;
	}

	/*
	 * THE TARGET IS THE DRIVE, SCALED TO THE CLAMP. Full deflection asks for
	 * the fastest this target may go; half asks for half. The clamp is
	 * applied HERE, to the target, rather than to the output -- so the walk
	 * approaches a legal rate smoothly instead of racing past one and being
	 * chopped, which would flatten the top of every fast gesture into a
	 * discontinuity.
	 */
	target = (int32_t)(((int64_t)s->drive_q16 * s->max_rate_q16) / ST_SCRATCH_DRIVE_FULL);

	/*
	 * WHICH RAMP: is this tick REDUCING the head's speed, or building it?
	 *
	 * The question is about |rate|, and it has to account for sign, which is
	 * the subtlety that got this wrong once. Comparing magnitudes alone --
	 * "is the target faster than we are?" -- reads a full-reverse target as
	 * an acceleration, because |-2.656| >= |+2.125| is true while the head is
	 * still travelling FORWARD. It then used the slow build-up ramp for the
	 * whole reversal, and two equal alternating presses exactly undid each
	 * other: the head sawtoothed 0 -> +2.1 -> 0 -> +2.1 and never once
	 * crossed zero. Forward-only pumping, not scratching.
	 *
	 * So an opposite-signed target is a DECELERATION until the head actually
	 * reaches zero, whatever its magnitude. Physically that is right too: a
	 * hand reversing a moving record is stopping it before it is driving it,
	 * and stopping is the quick half of the gesture.
	 *
	 * Once the head passes zero, sign(target) == sign(rate) and this same
	 * test flips to the accel ramp with no special case for the crossing --
	 * which is the property the magnitude comparison was reaching for, now
	 * actually obtained.
	 */
	{
		const int32_t cur_mag = (s->rate_q16 < 0) ? -s->rate_q16 : s->rate_q16;
		const int32_t tgt_mag = (target < 0) ? -target : target;
		const bool opposed = (s->rate_q16 > 0 && target < 0) ||
				      (s->rate_q16 < 0 && target > 0);
		const bool slowing = (s->rate_q16 != 0) && (opposed || tgt_mag < cur_mag);

		ramp_ms = slowing ? ST_SCRATCH_DECEL_MS : ST_SCRATCH_ACCEL_MS;
	}

	/*
	 * The step for this tick: how far the rate may move, given that a full
	 * traverse of the clamp takes `ramp_ms`. Rounded UP so a short tick can
	 * never round to zero and stall the walk -- the same stall that left the
	 * LED envelope stuck above its floor, and for the same integer-division
	 * reason.
	 */
	step = (int32_t)(((int64_t)s->max_rate_q16 * dt_us + (int64_t)ramp_ms * 1000 - 1) /
			  ((int64_t)ramp_ms * 1000));
	if (step <= 0) {
		step = 1;
	}

	s->rate_q16 = walk_toward(s->rate_q16, target, step);
}

int32_t st_scratch_release(st_scratch_t *s)
{
	s->engaged   = false;
	s->drive_q16 = 0;
	s->coasting  = (s->rate_q16 != ST_SCRATCH_UNITY_Q16);
	return s->rate_q16;
}

bool st_scratch_coast(st_scratch_t *s, uint32_t dt_us)
{
	int32_t step;

	if (!s->coasting) {
		return false;
	}
	if (dt_us == 0u) {
		return true;
	}

	/*
	 * THE DECEL RAMP, not the accel one, and not st_scrub's tape inertia.
	 *
	 * Coming off a record is a hand releasing, so it belongs to the quick
	 * ramp -- the same one a mid-gesture release uses, which is what makes
	 * letting go feel like one thing rather than two. st_scrub's ramp is
	 * the motor spinning up after STOP, measured in hundreds of
	 * milliseconds; using it here would leave the song audibly sliding back
	 * to pitch long after the hand was clear.
	 *
	 * Rounded up for the same reason the tick's step is: an integer step
	 * that rounds to zero never arrives, and this one would strand the
	 * transport permanently off-speed.
	 */
	step = (int32_t)(((int64_t)s->max_rate_q16 * dt_us +
			   (int64_t)ST_SCRATCH_DECEL_MS * 1000 - 1) /
			  ((int64_t)ST_SCRATCH_DECEL_MS * 1000));
	if (step <= 0) {
		step = 1;
	}

	s->rate_q16 = walk_toward(s->rate_q16, ST_SCRATCH_UNITY_Q16, step);
	if (s->rate_q16 == ST_SCRATCH_UNITY_Q16) {
		s->coasting = false;
		return false;
	}
	return true;
}

int32_t st_scratch_drive_from_fader(int32_t delta_counts, uint32_t dt_us)
{
	int64_t cps;
	int64_t drive;
	int32_t mag;

	if (dt_us == 0u) {
		return 0;
	}

	/* Counts per second the fader actually moved. */
	cps = ((int64_t)delta_counts * 1000000) / (int64_t)dt_us;

	/*
	 * THE DEADBAND IS APPLIED TO SPEED, NOT TO THE RAW DELTA, and that is
	 * deliberate: the same one-count wobble is noise at a 125 Hz sample rate
	 * and a real (if slow) movement at a much lower one. Gating the quantity
	 * that actually means "how fast is the hand moving" keeps the threshold
	 * correct if the control cadence ever changes.
	 */
	mag = (int32_t)((cps < 0) ? -cps : cps);
	if (mag < ST_SCRATCH_FADER_DEADBAND_CPS) {
		return 0;
	}

	/*
	 * Subtract the deadband rather than stepping over it. Without this, the
	 * drive would jump from 0 to the deadband's worth the instant a movement
	 * is believed -- an audible lurch at the start of every slow scratch,
	 * which is precisely the gesture that needs to start gently.
	 */
	mag -= ST_SCRATCH_FADER_DEADBAND_CPS;

	drive = ((int64_t)mag * ST_SCRATCH_DRIVE_FULL) / ST_SCRATCH_FADER_FULL_CPS;
	if (drive > ST_SCRATCH_DRIVE_FULL) {
		drive = ST_SCRATCH_DRIVE_FULL;
	}
	return (cps < 0) ? (int32_t)-drive : (int32_t)drive;
}
