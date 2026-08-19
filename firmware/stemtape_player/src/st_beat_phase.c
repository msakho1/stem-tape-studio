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

bool st_beat_phase_on_beat(const st_beat_timing_t *timing, uint32_t song_frame, uint32_t window_frames)
{
	if (timing->frames_per_beat == 0u || song_frame < timing->downbeat_frame) {
		return false;
	}

	uint32_t since_downbeat = song_frame - timing->downbeat_frame;
	uint32_t phase = since_downbeat % timing->frames_per_beat;

	return phase < window_frames;
}

st_track_led_state_t st_beat_led_decide(bool audible, bool playing, bool on_beat)
{
	if (!audible) {
		return ST_TRACK_LED_GHOST;
	}
	if (!playing) {
		return ST_TRACK_LED_ON; /* stopped but loaded: solid, same as the classic engine's own precedent */
	}
	return on_beat ? ST_TRACK_LED_ON : ST_TRACK_LED_OFF;
}
