/*
 * st_stem_meter.c -- see st_stem_meter.h for what this displays, why there are
 * two envelopes rather than one, and why none of it consults the tempo.
 */

#include "st_stem_meter.h"

void st_stem_meter_reset(st_stem_meter_t *m)
{
	m->fast = 0u;
	m->body = 0u;
}

/*
 * Move `env` a fraction of the way toward `target`, that fraction being
 * dt_ms/tc_ms. Proportional rather than linear, so the movement is even in
 * decibels: the same time constant looks the same starting from a loud level
 * as from a quiet one.
 *
 * A step at or past the whole time constant is stated directly rather than
 * left to the arithmetic, because the proportional form would compute a >=100%
 * move and rely on a clamp.
 *
 * THE MINIMUM STEP IS ONE, and it is what makes a proportional decay actually
 * terminate. span * dt / tc is integer division, so once the remaining span is
 * smaller than tc/dt the step rounds to zero and the envelope STOPS -- at
 * whatever value it happened to reach, forever. At a ~10 ms service rate and a
 * 300 ms release that stall region is span < 30. In the 24-bit domain the noise
 * floor was 2048, far above it, so the stall could only ever happen below the
 * floor where the light is already dark and nothing could be seen. At v1.3's
 * 16 bits the floor is 8 and the stall region straddles it: an envelope
 * resting anywhere in 9..29 would never reach the floor, and the Track LED
 * would sit faintly lit after the stem went silent and stay there.
 *
 * Stepping one instead of zero fixes it without touching the shape of the
 * decay anywhere the proportional step is already >= 1 -- which is the whole
 * visible range. The step is still capped at `span`, so it converges onto the
 * target rather than crossing it.
 */
static uint32_t glide(uint32_t env, uint32_t target, uint32_t dt_ms,
		       uint32_t tc_ms)
{
	uint32_t span, step;

	if (tc_ms == 0u || dt_ms >= tc_ms) {
		return target;
	}
	if (dt_ms == 0u) {
		return env;
	}

	span = (target >= env) ? (target - env) : (env - target);
	step = (uint32_t)(((uint64_t)span * dt_ms) / tc_ms);
	if (step == 0u && span > 0u) {
		step = 1u;
	}

	return (target >= env) ? (env + step) : (env - step);
}

/*
 * Both envelopes chase the SAME peak, and differ only in how quickly they are
 * allowed to. Everything expressive about the display comes out of that
 * difference: see the header's "BODY + ACCENT" section.
 *
 * Note that both fall toward the incoming peak rather than toward zero. A
 * sustained sound following a hit on the same stem must land the envelope ON
 * the sustained level and stay there, not fade to black underneath the hit's
 * decay and then climb back.
 */
void st_stem_meter_update(st_stem_meter_t *m, uint32_t peak, uint32_t dt_ms)
{
	if (peak > ST_STEM_METER_FULL_SCALE) {
		peak = ST_STEM_METER_FULL_SCALE;
	}

	m->fast = glide(m->fast, peak, dt_ms,
			 (peak >= m->fast) ? ST_STEM_METER_ATTACK_MS
					   : ST_STEM_METER_FAST_RELEASE_MS);

	/* The body's LAGGING attack is what opens the accent gap. With
	 * BODY_ATTACK_MS at 0 this line would track `fast` exactly and the
	 * accent would be identically zero forever. */
	m->body = glide(m->body, peak, dt_ms,
			 (peak >= m->body) ? ST_STEM_METER_BODY_ATTACK_MS
					   : ST_STEM_METER_BODY_RELEASE_MS);
}

/*
 * Integer log2 with 4 fractional bits. The 4 bits below the leading one
 * interpolate within that octave, which is close enough to logarithmic inside
 * a single octave that the residual error is far under one visible brightness
 * step. Returns 0 for v == 0.
 */
static uint32_t log2_q4(uint32_t v)
{
	uint32_t msb = 0u, t = v;

	if (v == 0u) {
		return 0u;
	}
	while (t > 1u) {
		t >>= 1;
		msb++;
	}
	return (msb << 4) |
	       ((msb >= 4u) ? ((v >> (msb - 4u)) & 0x0Fu) : 0u);
}

uint8_t st_stem_meter_brightness(const st_stem_meter_t *m)
{
	const uint32_t usable = ST_STEM_METER_MAX - ST_STEM_METER_MIN_ON;
	const uint32_t body_range =
		(usable * ST_STEM_METER_BODY_SHARE_Q8) / 256u;
	const uint32_t accent_range = usable - body_range;
	uint32_t ref_q4, floor_q4, body_q4, fast_q4;
	uint32_t span, body_lit = 0u, accent_lit = 0u, level;

	/* SILENCE IS THE ONLY WAY TO ZERO, and it takes BOTH envelopes being
	 * under the floor -- the body still glowing means the stem was sounding
	 * a moment ago and is decaying, which is a thing to show, not hide. */
	if (m->body <= ST_STEM_METER_FLOOR && m->fast <= ST_STEM_METER_FLOOR) {
		return 0u;
	}

	ref_q4   = log2_q4(ST_STEM_METER_REF);
	floor_q4 = log2_q4(ST_STEM_METER_FLOOR);
	body_q4  = log2_q4(m->body);
	fast_q4  = log2_q4(m->fast);

	/* ---- THE BODY: how much this stem is doing lately ------------------
	 * Mapped across BODY_SPAN octaves below the reference, and capped at
	 * body_range rather than at full brightness. The cap is the
	 * anti-saturation rule: sustained material must sit in the middle of
	 * the range so that accents have somewhere to go. */
	span = ST_STEM_METER_BODY_SPAN_OCTAVES * 16u;
	if (ref_q4 > floor_q4 && ref_q4 < floor_q4 + span) {
		span = ref_q4 - floor_q4;   /* never ask for range that is not there */
	}
	if (span == 0u) {
		body_lit = body_range;
	} else if (body_q4 >= ref_q4) {
		body_lit = body_range;
	} else if (body_q4 > ref_q4 - span) {
		body_lit = ((body_q4 - (ref_q4 - span)) * body_range) / span;
	}
	/* else: audible but below the body window -- glow stays at zero and the
	 * light rests on MIN_ON, which is "quiet", not "off". */

	/* ---- THE ACCENT: how far ahead of itself this stem just got --------
	 * A RELATIVE measure, deliberately. It fires on a 6 dB jump whether
	 * that jump lands at -30 dBFS or at -3 dBFS, so a quiet passage still
	 * animates instead of flattening out just because it is quiet. This is
	 * the "0.30 -> 0.45 deserves a visible pulse" requirement: that is
	 * 3.5 dB, better than half of a full accent.
	 *
	 * Zero when fast has fallen back to body, which is exactly the steady
	 * state between musical events. */
	if (fast_q4 > body_q4) {
		const uint32_t gap  = fast_q4 - body_q4;
		const uint32_t full = ST_STEM_METER_ACCENT_SPAN_OCTAVES * 16u;

		accent_lit = (gap >= full) ? accent_range
					   : (gap * accent_range) / full;
	}

	level = ST_STEM_METER_MIN_ON + body_lit + accent_lit;
	if (level > ST_STEM_METER_MAX) {
		level = ST_STEM_METER_MAX;
	}
	return (uint8_t)level;
}
