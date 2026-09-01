/*
 * st_stem_mix.h — deterministic fixed-point 4-stem-to-stereo mixdown.
 *
 * Phase 2 (real four-stem playback engine) first slice: the pure numeric
 * core of the mixer, with NO I/O, NO Zephyr, NO storage/streaming/session
 * state, and NOT YET WIRED into main.c's audio_thread()/looper_audio_
 * block() -- that wiring is a later, separate slice (its own commit, its
 * own real-Zephyr-build proof), because it also has to resolve how this
 * new source interacts with the existing USB-audio-input mix path, which
 * is not yet decided (see the wiring commit's own doc comment when it
 * lands). Until then this module exists, is real, is host-tested, and is
 * linked into the firmware image (see CMakeLists.txt) but has no caller
 * yet -- exactly the same shape st_ab_session.c had between the commit
 * that introduced it and the commit that wired main.c's 'W' handler to
 * call it.
 *
 * Takes one already-decoded st11_audio_frame_t (st_sector_v11.h's real
 * STSC decode output -- 4 stems, each a signed 24-bit sample widened to
 * int32_t) plus a per-stem gain/mute/solo state, and produces ONE final
 * stereo sample pair at the width main.c's existing, already-proven I2S
 * TX path actually consumes (16-bit -- see main.c's `audio_thread()`'s
 * `struct i2s_config` `.word_size = 16`, unchanged by this module; stems
 * are stored and decoded at 24-bit for headroom/quality, then reduced to
 * 16-bit only at this final mixdown step, the same width the classic
 * looper's own mono tracks already play at).
 *
 * Gain convention: Q8 fixed-point, 256 = unity -- the SAME scale and
 * meaning main.c's own `struct looptrk.vol_q8` field already uses for its
 * 4 classic loop tracks (see main.c's own comment: "fader volume, 256 =
 * unity"), reused here rather than inventing a second convention.
 *
 * GAIN CEILING (changed, deliberately): the applied gain is clamped to
 * +/-ST_STEM_MIX_GAIN_MAX_Q8 == unity. This module used to accept an
 * unbounded gain and absorb it in an int64_t accumulator. That int64_t was
 * the single most expensive thing in the firmware's 48 kHz hot path -- two
 * 64-bit multiplies, two 64-bit signed divisions and two 64-bit adds per
 * stem, 48000 times a second -- and on this device audio-thread CPU is not
 * spare capacity: the eMMC read path is CPU-bound, so cycles burned here
 * come straight out of stream throughput (measured on hardware: audio at
 * 51% left the streamer 40% and only 644 kB/s against the 1152 kB/s that
 * 48 kHz four-stem playback needs, which is why playback ran slow).
 * Clamping at unity is what lets the whole mixdown run in int32_t. Nothing
 * loses a capability it had: main.c's own fader handler already clamps
 * every stem gain to 256 before it ever reaches here (see its `q > 256u ?
 * 256u : q`), so above-unity boost was a path no caller could reach --
 * exactly the dead weight this build does not carry.
 *
 * Solo/mute convention (standard mixing-console semantics, chosen because
 * neither docs/FIRMWARE_CONTRACT_V1.md nor docs/firmware-contract-v1.json
 * specifies channel-strip-internal solo/mute-interaction semantics at
 * this level -- only the higher-level `stem.solo`/lane gestures, which a
 * later control-matrix slice maps down to these two booleans): a muted
 * stem is always silent, even if also soloed (mute wins over solo for
 * that same stem); if ANY stem is soloed, every non-soloed stem is
 * silent regardless of its own mute state; if no stem is soloed, every
 * non-muted stem plays.
 *
 * PURE except for no I/O at all: every function operates only on
 * caller-supplied values, deterministic, no floating point (fixed-point
 * only, matching this whole codebase's existing real-time-audio-path
 * convention -- see e.g. main.c's own mixer for the classic tracks).
 */

#ifndef STEMTAPE_PLAYER_STEM_MIX_H_
#define STEMTAPE_PLAYER_STEM_MIX_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_sector_v11.h"
#include "st_v11_format.h"

#define ST_STEM_MIX_GAIN_UNITY_Q8 256
/* log2 of the unity gain, so the mixer's round-toward-zero shift and the
 * divisor can never drift apart. Asserted, not assumed. */
#define ST_STEM_MIX_GAIN_UNITY_SHIFT 8
_Static_assert((1 << ST_STEM_MIX_GAIN_UNITY_SHIFT) == ST_STEM_MIX_GAIN_UNITY_Q8,
		"the mixer's shift must equal log2 of its unity gain");

/*
 * The ceiling every applied gain is clamped to (see this header's own
 * "GAIN CEILING" note). It is exactly unity, and that is not arbitrary:
 * a decoded stem sample is a sign-extended signed 24-bit value, so
 * |sample| <= 2^23, and |sample * ST_STEM_MIX_GAIN_MAX_Q8| <= 2^31 --
 * representable in int32_t. That bound is the whole reason the 48 kHz
 * mixdown below can be int32_t arithmetic instead of int64_t.
 */
#define ST_STEM_MIX_GAIN_MAX_Q8 ST_STEM_MIX_GAIN_UNITY_Q8

typedef struct {
	int32_t gain_q8; /* 256 = unity; see this header's own doc comment */
	bool mute;
	bool solo;
} st_stem_mix_channel_t;

/*
 * The per-stem EFFECTIVE gain, with mute/solo already resolved and the
 * ceiling already applied -- everything about a channel strip that is
 * constant for a whole audio block, collapsed into the only thing the
 * per-frame mixdown actually needs.
 *
 * A stem that is inaudible (muted, or silenced by another stem's solo)
 * has effective gain 0, so the per-frame path multiplies by zero rather
 * than branching. There is no second audibility rule anywhere: 0 here IS
 * "not heard", and st_stem_mix_channel_audible() answers from the same
 * st_stem_mix_prepare() logic.
 */
typedef struct {
	int32_t gain_q8[ST11_STEM_COUNT];
} st_stem_mix_prepared_t;

/*
 * Resolves channels[]'s mute/solo state and gain ceiling into the compact
 * per-block form above. Call this ONCE per audio block (channel strip
 * state is a control-rate quantity -- faders, mute, solo -- and cannot
 * change inside a block), then call st_stem_mix_frame_prepared() per
 * frame. Pure, bounded, no allocation.
 */
void st_stem_mix_prepare(const st_stem_mix_channel_t channels[ST11_STEM_COUNT],
			  st_stem_mix_prepared_t *out);

/*
 * THE 48 kHz HOT PATH. Mixes one decoded frame's ST11_STEM_COUNT stems
 * down to one stereo sample pair using an already-prepared gain set, and
 * writes the saturated signed-16-bit result to `*out_l`/`*out_r`.
 *
 * Branchless and entirely int32_t: no solo scan, no mute test, no 64-bit
 * arithmetic. Identical output to st_stem_mix_frame() given the same
 * inputs -- it is literally the second half of it.
 *
 * PRECONDITION on `frame`: each stem sample is a sign-extended signed
 * 24-bit value (|sample| <= 2^23). That is exactly, and only, what
 * st11_sector_decode_frame() produces (see st_sector_v11.c's get_i24le(),
 * which sign-extends from bit 23), and it is what makes the int32_t
 * product bound in ST_STEM_MIX_GAIN_MAX_Q8's own comment hold.
 */
void st_stem_mix_frame_prepared(const st11_audio_frame_t *frame,
				 const st_stem_mix_prepared_t *prepared,
				 int16_t *out_l, int16_t *out_r);

/*
 * Mixes one decoded frame's ST11_STEM_COUNT stems down to one stereo
 * sample pair, applying `channels[s]`'s gain/mute/solo state to
 * `frame->stem_l[s]`/`stem_r[s]` (see this header's own doc comment for
 * the exact solo/mute interaction rule), and writes the saturated
 * signed-16-bit result to `*out_l`/`*out_r`. Never reads or writes
 * anything else; safe to call from a hard-real-time audio ISR/thread
 * context (no allocation, no loops beyond the fixed ST11_STEM_COUNT,
 * bounded, deterministic execution time).
 *
 * This is exactly st_stem_mix_prepare() followed by
 * st_stem_mix_frame_prepared() -- ONE implementation, not two. It is the
 * convenient form for callers that mix a single frame (tests, and any
 * non-real-time caller); the real-time audio path calls the two halves
 * separately so the prepare half runs once per block instead of 48000
 * times a second.
 */
void st_stem_mix_frame(const st11_audio_frame_t *frame, const st_stem_mix_channel_t channels[ST11_STEM_COUNT],
			int16_t *out_l, int16_t *out_r);

/*
 * True if channels[index] would actually be heard by st_stem_mix_frame()
 * given the WHOLE channels[] array's current mute/solo state -- the exact
 * same rule st_stem_mix_frame() applies internally (see this header's own
 * "Solo/mute convention" doc comment above: mute always wins; if ANY
 * channel is soloed, only soloed channels play), exposed so a caller that
 * needs to know audibility WITHOUT decoding/mixing an actual audio frame
 * (e.g. LED feedback, run from the control thread at ~8 ms resolution, far
 * below audio rate) can never drift from the real mixer's own decision --
 * one formula, not two independently-maintained copies.
 */
bool st_stem_mix_channel_audible(const st_stem_mix_channel_t channels[ST11_STEM_COUNT], uint32_t index);

#endif /* STEMTAPE_PLAYER_STEM_MIX_H_ */
