/*
 * st_stem_mix.c — see st_stem_mix.h.
 */

#include "st_stem_mix.h"

#include <limits.h>

/* Clamps the accumulator straight to the int16_t range -- taking the
 * comparison/clamp on the FULL-WIDTH value, never on an already-narrowed
 * one, so the reduction to 16 bits can never hit implementation-defined
 * narrowing-cast overflow before the clamp gets a chance to run.
 *
 * int32_t is genuinely full width here now: with every applied gain
 * clamped to ST_STEM_MIX_GAIN_MAX_Q8 and every stem sample a signed
 * 24-bit value, each stem's contribution is bounded by 2^23 and the sum
 * of ST11_STEM_COUNT of them by 2^25 -- decades below int32_t's range.
 * See st_stem_mix.h's own "GAIN CEILING" note for why that bound exists. */
static int16_t saturate_s16(int32_t v)
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

void st_stem_mix_prepare(const st_stem_mix_channel_t channels[ST11_STEM_COUNT],
			  st_stem_mix_prepared_t *out)
{
	bool any_solo = any_channel_soloed(channels);
	uint32_t s;

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		int32_t g = channel_active(channels, s, any_solo) ? channels[s].gain_q8 : 0;

		/* The ceiling that makes the per-frame path int32_t-safe. Both
		 * signs are clamped: gain_q8 is a signed field, and a negative
		 * (phase-inverting) gain has to obey the same magnitude bound
		 * the positive one does. See st_stem_mix.h. */
		if (g > ST_STEM_MIX_GAIN_MAX_Q8) {
			g = ST_STEM_MIX_GAIN_MAX_Q8;
		} else if (g < -ST_STEM_MIX_GAIN_MAX_Q8) {
			g = -ST_STEM_MIX_GAIN_MAX_Q8;
		}
		out->gain_q8[s] = g;
	}
}

bool st_stem_mix_channel_audible(const st_stem_mix_channel_t channels[ST11_STEM_COUNT], uint32_t index)
{
	return channel_active(channels, index, any_channel_soloed(channels));
}

/* -O2 FOR THIS FUNCTION ONLY, same justification as sp1_emmc.c's crc16()
 * (see that file's own note): this is pure computation with no timing or
 * aliasing dependency, so -O2 can only make it faster and cannot change
 * the value it produces. It matters because it is the single hottest
 * function in the firmware -- called once per output frame at 48 kHz --
 * and because on this device audio-thread CPU is not free: the eMMC read
 * path is CPU-bound (bit-banged start-bit hunt, SPIM setup, CRC), so
 * every cycle spent here is a cycle the streamer does not get, and read
 * throughput falls in direct proportion. Measured on hardware: with the
 * audio thread at 51% the streamer got 40% and sustained only 644 kB/s
 * against the 1,152 kB/s that 48 kHz four-stem playback requires. */
__attribute__((optimize("O2")))
void st_stem_mix_frame_prepared(const st11_audio_frame_t *frame,
				 const st_stem_mix_prepared_t *prepared,
				 int16_t *out_l, int16_t *out_r)
{
	uint32_t s;

	int32_t acc_l = 0;
	int32_t acc_r = 0;

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		const int32_t g = prepared->gain_q8[s];

		/* No mute/solo branch: an inaudible stem arrives here with
		 * gain 0 and contributes 0. Division (not a shift) is kept
		 * deliberately -- it rounds toward zero, which is what this
		 * mixdown has always produced, so this rewrite changes the
		 * cost and nothing else. */
		acc_l += (frame->stem_l[s] * g) / ST_STEM_MIX_GAIN_UNITY_Q8;
		acc_r += (frame->stem_r[s] * g) / ST_STEM_MIX_GAIN_UNITY_Q8;
	}

	/* Reduce the 24-bit stem-storage domain down to the 16-bit I2S
	 * output domain main.c's existing, unchanged audio_thread() actually
	 * consumes (an 8-bit shift == ST11_PCM_BIT_DEPTH - 16), THEN
	 * saturate -- ST11_STEM_COUNT stems summed near full-scale can
	 * legitimately push past the original 24-bit domain; this is the one
	 * place that gets clamped rather than silently wrapped. */
	*out_l = saturate_s16(acc_l >> (ST11_PCM_BIT_DEPTH - 16u));
	*out_r = saturate_s16(acc_r >> (ST11_PCM_BIT_DEPTH - 16u));
}

void st_stem_mix_frame(const st11_audio_frame_t *frame, const st_stem_mix_channel_t channels[ST11_STEM_COUNT],
			int16_t *out_l, int16_t *out_r)
{
	st_stem_mix_prepared_t prepared;

	st_stem_mix_prepare(channels, &prepared);
	st_stem_mix_frame_prepared(frame, &prepared, out_l, out_r);
}
