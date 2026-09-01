/*
 * st_stem_mix.c — see st_stem_mix.h.
 */

#include "st_stem_mix.h"

#include <limits.h>

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

/*
 * THE ARITHMETIC ITSELF NOW LIVES IN THE HEADER, as
 * st_stem_mix_frame_prepared_inline() -- see st_stem_mix.h for why the audio
 * thread needs it inline. This out-of-line symbol REMAINS, and calls it, so
 * that there is exactly one implementation: the host tests, the FX reference
 * gate and the firmware symbol gate all name this function, and every one of
 * them therefore exercises the same code the 48 kHz path runs.
 *
 * Everything the numbers rest on is recorded here rather than in the header,
 * because this is where it was derived:
 *
 * NO MUTE/SOLO BRANCH. An inaudible stem arrives with gain 0 and contributes
 * 0. There is no second audibility rule anywhere.
 *
 * ROUND-TOWARD-ZERO WITHOUT A DIVIDE. This was `(x * g) / 256`, and the
 * comment defending it said the division "rounds toward zero, which is what
 * this mixdown has always produced, so this rewrite changes the cost and
 * nothing else". Both halves were true and the cost was much larger than it
 * looks: a SIGNED division cannot become an arithmetic shift, because >>
 * rounds toward negative infinity and / rounds toward zero, so the compiler is
 * obliged to emit SDIV. That is EIGHT hardware divides per output frame --
 * four stems, two channels -- 384,000 a second at 48 kHz, on the thread with a
 * hard 5.333 ms deadline, on a device where audio CPU comes straight out of
 * the streamer's read throughput.
 *
 * THE IDENTITY. For a power-of-two divisor, adding (divisor-1) to a negative
 * numerator before an arithmetic shift converts floor into truncation:
 *
 *     x >= 0 :  x / 2^k  ==  x >> k
 *     x <  0 :  x / 2^k  ==  (x + (2^k - 1)) >> k
 *
 * (x >> 31) is 0 for non-negative and -1 for negative, so the mask picks the
 * bias with no branch. Three single-cycle ALU ops replace a multi-cycle
 * divide, and the result is EQUAL for every representable input, not merely
 * close.
 *
 * NO OVERFLOW, at the bias or at the multiply. gain_q8 is clamped to
 * ST_STEM_MIX_GAIN_MAX_Q8 (256) and a stem sample is a sign-extended 24-bit
 * value, so the product is bounded by [-2^23 * 256, (2^23 - 1) * 256] =
 * [INT32_MIN, INT32_MAX-255]. The largest positive product plus the 255 bias
 * is exactly INT32_MAX; the bias is only ever added to negative values anyway,
 * and the most negative product plus 255 is well inside range.
 *
 * THE FINAL REDUCTION happens BEFORE the clamp: the 24-bit stem-storage domain
 * comes down to the 16-bit I2S output domain audio_thread() consumes (an
 * 8-bit shift == ST11_PCM_BIT_DEPTH - 16), and only then saturates.
 * ST11_STEM_COUNT stems summed near full-scale can legitimately push past the
 * original 24-bit domain; that is the one place clamped rather than silently
 * wrapped, and the comparison is taken on the FULL-WIDTH value so the
 * narrowing cast can never hit implementation-defined overflow first.
 */
void st_stem_mix_frame_prepared(const st11_audio_frame_t *frame,
				 const st_stem_mix_prepared_t *prepared,
				 int16_t *out_l, int16_t *out_r)
{
	st_stem_mix_frame_prepared_inline(frame, prepared, out_l, out_r);
}

void st_stem_mix_frame(const st11_audio_frame_t *frame, const st_stem_mix_channel_t channels[ST11_STEM_COUNT],
			int16_t *out_l, int16_t *out_r)
{
	st_stem_mix_prepared_t prepared;

	st_stem_mix_prepare(channels, &prepared);
	st_stem_mix_frame_prepared(frame, &prepared, out_l, out_r);
}
