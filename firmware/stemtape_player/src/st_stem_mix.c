/*
 * st_stem_mix.c — see st_stem_mix.h.
 */

#include "st_stem_mix.h"

#include <limits.h>

/* Clamps a wide accumulator straight to the int16_t range -- taking the
 * comparison/clamp on the (still-wide) int64_t value itself, never on an
 * already-narrowed int32_t, so an absurd caller-supplied gain can never
 * hit implementation-defined narrowing-cast overflow before the clamp
 * gets a chance to run. */
static int16_t saturate_s16(int64_t v)
{
	if (v > INT16_MAX) {
		return INT16_MAX;
	}
	if (v < INT16_MIN) {
		return INT16_MIN;
	}
	return (int16_t)v;
}

void st_stem_mix_frame(const st11_audio_frame_t *frame, const st_stem_mix_channel_t channels[ST11_STEM_COUNT],
			int16_t *out_l, int16_t *out_r)
{
	bool any_solo = false;
	uint32_t s;

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		if (channels[s].solo) {
			any_solo = true;
			break;
		}
	}

	int64_t acc_l = 0;
	int64_t acc_r = 0;

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		/* Mute always wins (even over this same stem's own solo);
		 * otherwise, if anything anywhere is soloed, only soloed
		 * stems play -- see this header's own doc comment. */
		bool active = !channels[s].mute && (!any_solo || channels[s].solo);

		if (!active) {
			continue;
		}
		acc_l += ((int64_t)frame->stem_l[s] * channels[s].gain_q8) / ST_STEM_MIX_GAIN_UNITY_Q8;
		acc_r += ((int64_t)frame->stem_r[s] * channels[s].gain_q8) / ST_STEM_MIX_GAIN_UNITY_Q8;
	}

	/* Reduce the 24-bit stem-storage domain down to the 16-bit I2S
	 * output domain main.c's existing, unchanged audio_thread() actually
	 * consumes (an 8-bit shift == ST11_PCM_BIT_DEPTH - 16), THEN
	 * saturate -- up to ST11_STEM_COUNT stems summed near full-scale, or
	 * a caller-supplied gain above unity, can legitimately push past the
	 * original 24-bit domain; this is the one place that gets clamped
	 * rather than silently wrapped. */
	*out_l = saturate_s16(acc_l >> (ST11_PCM_BIT_DEPTH - 16u));
	*out_r = saturate_s16(acc_r >> (ST11_PCM_BIT_DEPTH - 16u));
}
