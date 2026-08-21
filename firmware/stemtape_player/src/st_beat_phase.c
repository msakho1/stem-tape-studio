/*
 * st_beat_phase.c — see st_beat_phase.h.
 */

#include "st_beat_phase.h"

#include <limits.h>

bool st_beat_timing_init(st_beat_timing_t *out, uint32_t bpm_q8, uint32_t downbeat_frame, uint32_t sample_rate)
{
	out->downbeat_frame = downbeat_frame;
	out->frames_per_beat = 0u;

	if (bpm_q8 == 0u || sample_rate == 0u) {
		return false;
	}

	/* frames_per_beat = sample_rate * 60 / real_bpm, real_bpm = bpm_q8 / 256
	 *                 = sample_rate * 60 * 256 / bpm_q8
	 * rounded to the nearest whole frame (add half the divisor before the
	 * integer divide) -- 64-bit intermediate, no floating point. */
	uint64_t numerator = (uint64_t)sample_rate * 60ull * 256ull;
	uint64_t rounded = (numerator + (uint64_t)(bpm_q8 / 2u)) / (uint64_t)bpm_q8;

	if (rounded == 0u || rounded > (uint64_t)UINT32_MAX) {
		return false; /* absurd bpm/sample_rate combination -- fail closed, never wrap */
	}
	out->frames_per_beat = (uint32_t)rounded;
	return true;
}

void st_beat_pulse(const st_beat_timing_t *timing, uint32_t song_frame,
		    st_beat_pulse_t *out)
{
	uint32_t since, into_beat, window, last, half, rise;

	out->valid = false;
	out->in_pulse = false;
	out->envelope = 0u;
	out->beat_index = 0u;

	/* Fail closed: no tempo, or not yet at the first downbeat, means no
	 * pulse and no bar position -- not an invented one. */
	if (timing->frames_per_beat == 0u) {
		return;
	}
	if (song_frame < timing->downbeat_frame) {
		return;
	}

	since = song_frame - timing->downbeat_frame;
	out->beat_index = (uint8_t)((since / timing->frames_per_beat) & 3u);
	out->valid = true;

	into_beat = since % timing->frames_per_beat;
	window = (timing->frames_per_beat * ST_BEAT_PULSE_NUM) / ST_BEAT_PULSE_DEN;
	if (window == 0u) {
		return;   /* absurdly fast tempo: no window to light */
	}
	if (into_beat >= window) {
		return;   /* between pulses: dark, which is the point */
	}

	out->in_pulse = true;

	/* Symmetric triangle: 0 at both edges of the window, exactly 255 at its
	 * centre. Measured as the distance to the NEARER edge, which is
	 * symmetric by construction -- an earlier version scaled a rising/
	 * falling branch by window/2 and so peaked at 254, never at the 255 this
	 * module documents, and left a flat plateau on odd-length windows. */
	last = window - 1u;
	rise = (into_beat <= (last - into_beat)) ? into_beat : (last - into_beat);
	half = last / 2u;    /* the centre's own distance to the nearer edge */
	if (half == 0u) {
		out->envelope = 255u;   /* window of 1-2 frames: no room for a ramp */
		return;
	}
	out->envelope = (uint8_t)((rise * 255u) / half);
}
