/*
 * st_stem_meter.c — see st_stem_meter.h.
 */

#include "st_stem_meter.h"

void st_stem_meter_reset(st_stem_meter_t *m)
{
	m->env = 0u;
}

void st_stem_meter_update(st_stem_meter_t *m, uint32_t peak, uint32_t dt_ms)
{
	if (peak > ST_STEM_METER_FULL_SCALE) {
		peak = ST_STEM_METER_FULL_SCALE;
	}

	if (dt_ms >= ST_STEM_METER_RELEASE_MS) {
		/* A gap at least as long as the whole release constant: the
		 * proportional fall below would compute a >=100% drop, so
		 * state it directly rather than relying on the subtraction
		 * happening to clamp. */
		m->env = 0u;
	} else if (dt_ms > 0u) {
		/* Proportional (constant-dB) fall. Done in 64-bit because
		 * env can be up to 2^23 and dt_ms up to the release constant;
		 * the product comfortably exceeds 32 bits only in principle,
		 * but the widening costs nothing here and removes the need to
		 * reason about it at all. */
		uint32_t drop = (uint32_t)(((uint64_t)m->env * dt_ms) / ST_STEM_METER_RELEASE_MS);

		m->env = (drop >= m->env) ? 0u : (m->env - drop);
	}

	/* Instant attack: the transient is the signal. */
	if (peak > m->env) {
		m->env = peak;
	}
}

uint8_t st_stem_meter_brightness(const st_stem_meter_t *m)
{
	uint32_t env = m->env;

	if (env <= ST_STEM_METER_FLOOR) {
		return 0u;
	}

	/* Integer log2 with 4 fractional bits.
	 *
	 * `msb` is the index of the highest set bit (11..23 for anything
	 * above the floor, since ST_STEM_METER_FLOOR is 2^11). The 4 bits
	 * BELOW the leading one interpolate linearly within that octave,
	 * which is close enough to logarithmic inside a single octave that
	 * the residual error is far below one visible brightness step. */
	uint32_t msb = 0u;
	uint32_t v = env;

	while (v > 1u) {
		v >>= 1;
		msb++;
	}

	uint32_t frac = (msb >= 4u) ? ((env >> (msb - 4u)) & 0x0Fu) : 0u;
	uint32_t log_q4 = (msb << 4) | frac;             /* 0 .. 383 (msb<=23) */

	/* Floor's own log value maps to 0, full scale maps to 255, linear in
	 * between -- so the visible range is spent entirely on the ~72 dB
	 * that is actually above the floor, rather than on the silence
	 * below it. */
	const uint32_t floor_q4 = 11u << 4;              /* log2(2048) = 11 exactly */
	/* Full scale is 2^23 - 1, NOT 2^23: its highest set bit is 22 and
	 * every fractional bit below it is 1, so its own log_q4 is
	 * (22 << 4) | 0xF == 367. Using 23 << 4 here would put the top of
	 * the curve one whole octave above any value the 24-bit domain can
	 * actually produce, and full-scale audio would read 253/255 instead
	 * of maxing out. */
	const uint32_t top_q4   = (22u << 4) | 0x0Fu;    /* log2(2^23 - 1) in Q4 */

	if (log_q4 <= floor_q4) {
		return 0u;
	}
	if (log_q4 >= top_q4) {
		return 255u;
	}

	return (uint8_t)(((log_q4 - floor_q4) * 255u) / (top_q4 - floor_q4));
}
