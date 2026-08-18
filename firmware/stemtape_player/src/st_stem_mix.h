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
 * unity"), reused here rather than inventing a second convention. No
 * ceiling is enforced here on the caller-supplied gain value itself
 * (mapping a physical fader's ADC range to a gain_q8 value, including any
 * maximum-boost policy, is a Phase 3 control-matrix concern, not this
 * module's) -- whatever gain is supplied, the mixdown always saturates
 * safely to the output range.
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

typedef struct {
	int32_t gain_q8; /* 256 = unity; see this header's own doc comment */
	bool mute;
	bool solo;
} st_stem_mix_channel_t;

/*
 * Mixes one decoded frame's ST11_STEM_COUNT stems down to one stereo
 * sample pair, applying `channels[s]`'s gain/mute/solo state to
 * `frame->stem_l[s]`/`stem_r[s]` (see this header's own doc comment for
 * the exact solo/mute interaction rule), and writes the saturated
 * signed-16-bit result to `*out_l`/`*out_r`. Never reads or writes
 * anything else; safe to call from a hard-real-time audio ISR/thread
 * context (no allocation, no loops beyond the fixed ST11_STEM_COUNT,
 * bounded, deterministic execution time).
 */
void st_stem_mix_frame(const st11_audio_frame_t *frame, const st_stem_mix_channel_t channels[ST11_STEM_COUNT],
			int16_t *out_l, int16_t *out_r);

#endif /* STEMTAPE_PLAYER_STEM_MIX_H_ */
