/*
 * st_stem_meter.c -- see st_stem_meter.h for what this displays and why it
 * is deliberately not connected to the tempo.
 */

#include "st_stem_meter.h"

void st_stem_meter_reset(st_stem_meter_t *m)
{
	m->env = 0u;
}

/*
 * Move `env` a fraction of the way toward `target`, that fraction being
 * dt_ms/tc_ms. Proportional rather than linear, so the movement is even in
 * decibels: the same time constant looks the same starting from a loud level
 * as from a quiet one.
 *
 * A step at or past the whole time constant is stated directly rather than
 * left to the arithmetic, because the proportional form would compute a
 * >=100% move and rely on a clamp.
 */
static uint32_t glide(uint32_t env, uint32_t target, uint32_t dt_ms,
		       uint32_t tc_ms)
{
	uint32_t span;

	if (tc_ms == 0u || dt_ms >= tc_ms) {
		return target;
	}
	if (dt_ms == 0u) {
		return env;
	}
	if (target >= env) {
		span = target - env;
		return env + (uint32_t)(((uint64_t)span * dt_ms) / tc_ms);
	}
	span = env - target;
	return env - (uint32_t)(((uint64_t)span * dt_ms) / tc_ms);
}

void st_stem_meter_update(st_stem_meter_t *m, uint32_t peak, uint32_t dt_ms)
{
	if (peak > ST_STEM_METER_FULL_SCALE) {
		peak = ST_STEM_METER_FULL_SCALE;
	}

	if (peak >= m->env) {
		/* RISING. Attack of 0 is the default and means the peak lands
		 * whole -- see the header on why that is right for a transient
		 * and why anything under ~15 ms is instant anyway. */
		m->env = glide(m->env, peak, dt_ms, ST_STEM_METER_ATTACK_MS);
		return;
	}

	/*
	 * FALLING, toward the incoming peak rather than toward zero.
	 *
	 * That distinction matters and is easy to get wrong. Decaying toward
	 * zero while a quieter sound is still playing would drag the light
	 * below the level of audio that is genuinely there -- a sustained pad
	 * following a drum hit on the same stem would fade to black underneath
	 * the hit's decay and then have to climb back. Falling toward the
	 * current peak lands the envelope ON the sustained level and stays
	 * there, which is what a sustained sound should look like.
	 */
	m->env = glide(m->env, peak, dt_ms, ST_STEM_METER_RELEASE_MS);
}

uint8_t st_stem_meter_brightness(const st_stem_meter_t *m)
{
	const uint32_t env = m->env;
	uint32_t msb = 0u, v, frac, log_q4, floor_q4, ref_q4, span, lit;

	/* THE FLOOR DECIDES OFF, and it is the only thing that does. */
	if (env <= ST_STEM_METER_FLOOR) {
		return 0u;
	}

	/*
	 * Integer log2 with 4 fractional bits. `msb` is the index of the
	 * highest set bit; the 4 bits below it interpolate within that octave,
	 * which is close enough to logarithmic inside one octave that the
	 * residual error is far under a single visible brightness step.
	 */
	v = env;
	while (v > 1u) {
		v >>= 1;
		msb++;
	}
	frac = (msb >= 4u) ? ((env >> (msb - 4u)) & 0x0Fu) : 0u;
	log_q4 = (msb << 4) | frac;

	/* The same Q4 log of the two ends of the curve, computed the same way
	 * so the endpoints cannot disagree with the values being mapped. */
	floor_q4 = 0u;
	v = ST_STEM_METER_FLOOR;
	while (v > 1u) {
		v >>= 1;
		floor_q4++;
	}
	floor_q4 <<= 4;   /* the floor is a power of two: no fractional part */

	ref_q4 = 0u;
	v = ST_STEM_METER_REF;
	while (v > 1u) {
		v >>= 1;
		ref_q4++;
	}
	{
		const uint32_t rmsb = ref_q4;

		ref_q4 = (rmsb << 4) |
			  ((rmsb >= 4u)
			   ? ((ST_STEM_METER_REF >> (rmsb - 4u)) & 0x0Fu) : 0u);
	}

	if (ref_q4 <= floor_q4) {
		/* A sensitivity set at or below the floor leaves no range to
		 * map; anything audible then reads full rather than dividing
		 * by nothing. */
		return (uint8_t)ST_STEM_METER_MAX;
	}

	/*
	 * THE VISIBLE WINDOW is the SPAN octaves immediately below the
	 * reference -- not the whole distance down to the noise floor. See the
	 * header: stretching the display over all ~66 dB of headroom is what
	 * makes a row of level meters read as static.
	 *
	 * The bottom of the window is clamped to the floor so that a very wide
	 * span cannot ask for range that does not exist.
	 */
	span = ST_STEM_METER_SPAN_OCTAVES * 16u;
	if (ref_q4 < floor_q4 + span) {
		span = ref_q4 - floor_q4;
	}

	if (log_q4 >= ref_q4) {
		lit = 255u;                  /* at or above the reference */
	} else if (log_q4 <= ref_q4 - span) {
		lit = 0u;                    /* audible, but under the window */
	} else {
		lit = ((log_q4 - (ref_q4 - span)) * 255u) / span;
	}

	/*
	 * THE VISIBLE RANGE. Anything above the floor is making sound, so it
	 * is lifted to at least MIN_ON: the eye needs OFF vs QUIET far more
	 * than it needs the bottom few duty steps, and on real hardware a duty
	 * of 1/255 may not light the LED at all. Zero is unreachable from
	 * here -- the floor above is what produces darkness.
	 */
	return (uint8_t)(ST_STEM_METER_MIN_ON +
			  (lit * (ST_STEM_METER_MAX - ST_STEM_METER_MIN_ON)) / 255u);
}
