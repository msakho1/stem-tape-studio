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

static bool any_channel_soloed(const st_stem_mix_channel_t channels[ST11_STEM_COUNT])
{
	uint32_t s;

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		if (channels[s].solo) {
			return true;
		}
	}
	return false;
}

/* The ONE place the mute/solo interaction rule is decided -- both
 * st_stem_mix_frame() (per audio frame) and st_stem_mix_channel_audible()
 * (per LED-feedback query) call this SAME function with a freshly computed
 * `any_solo`, so the two can never independently drift out of sync. */
static bool channel_active(const st_stem_mix_channel_t channels[ST11_STEM_COUNT], uint32_t index, bool any_solo)
{
	/* Mute always wins (even over this same stem's own solo);
	 * otherwise, if anything anywhere is soloed, only soloed stems
	 * play -- see this header's own doc comment. */
	return !channels[index].mute && (!any_solo || channels[index].solo);
}

bool st_stem_mix_channel_audible(const st_stem_mix_channel_t channels[ST11_STEM_COUNT], uint32_t index)
{
	return channel_active(channels, index, any_channel_soloed(channels));
}

void st_stem_mix_frame(const st11_audio_frame_t *frame, const st_stem_mix_channel_t channels[ST11_STEM_COUNT],
			int16_t *out_l, int16_t *out_r)
{
	bool any_solo = any_channel_soloed(channels);
	uint32_t s;

	int64_t acc_l = 0;
	int64_t acc_r = 0;

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		if (!channel_active(channels, s, any_solo)) {
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
