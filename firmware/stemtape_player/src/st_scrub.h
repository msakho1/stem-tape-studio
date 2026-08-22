/*
 * st_scrub.h — Stem Tape global forward/reverse scrub: the four persistent
 * speed multipliers and the tape-inertia release ramp, ported from
 * src/audio/inertia.ts (GLOBAL_SCRUB_SPEEDS, makeScrubReleaseSegment,
 * inertiaRateAt/inertiaDistance/inertiaZeroCrossing) — the EXACT documented
 * math, not a re-derivation. See task section 7: "do not choose new
 * extremes."
 *
 * PURE: floating-point math only, no audio I/O. The (deferred) audio
 * engine samples st_scrub_rate_at()/st_scrub_distance() once per render
 * block against its own elapsed time to drive the actual resampler; this
 * module owns only the shape of the ramp and the continuous position
 * integral, exactly like inertia.ts does for the web engine.
 */

#ifndef STEMTAPE_PLAYER_SCRUB_H_
#define STEMTAPE_PLAYER_SCRUB_H_

#include <stdbool.h>
#include <stdint.h>

/* [ts:src/audio/inertia.ts:196] GLOBAL_SCRUB_SPEEDS = [1.25, 1.6, 2.5, 4].
 * Reported, not re-chosen -- "do not choose new extremes". */
static const float ST_SCRUB_SPEEDS[4] = { 1.25f, 1.6f, 2.5f, 4.0f };
#define ST_SCRUB_DEFAULT_SPEED_INDEX 1u /* [ts:inertia.ts:197] DEFAULT_SCRUB_SPEED_INDEX */

/* [ts:inertia.ts:44] INERTIA_K: curvature, "audibly tape-like knee without a long tail". */
#define ST_SCRUB_INERTIA_K 4.0f

/* [ts:inertia.ts INERTIA_PRESETS.classic] the release ramp always uses the
 * Classic preset's wind-down duration as its nominal span. */
#define ST_SCRUB_RELEASE_NOMINAL_S 0.45f

/* [ts:inertia.ts:170-172] scaled = preset.stopS * min(2, span/reference);
 * duration clamped to [0.02, 1.2] seconds. */
#define ST_SCRUB_RELEASE_MIN_S 0.02f
#define ST_SCRUB_RELEASE_MAX_S 1.2f

typedef struct {
	float    from;       /* signed rate at t=0 (negative = reverse) */
	float    to;          /* always +1.0 (musical rate) for a scrub release */
	float    duration_s;
} st_scrub_release_t;

/*
 * [ts:inertia.ts makeScrubReleaseSegment] Builds the release ramp at the
 * exact audible rate the scrub was at when released/unlatched --
 * "use the exact audible scrub position at the scheduled audio seam", never
 * a rounded or hidden value.
 */
st_scrub_release_t st_scrub_make_release(float from_rate_signed);

/* [ts:inertia.ts inertiaRateAt] Instantaneous signed rate `dt` seconds into
 * the release ramp. Monotone for a forward release (never leaves forward);
 * crosses exactly zero once for a reverse release before continuing to
 * +1.0x, matching "reverse scrub decelerates to zero, crosses direction
 * continuously, and accelerates to +1.0x". */
float st_scrub_rate_at(const st_scrub_release_t *seg, float dt);

/* [ts:inertia.ts inertiaDistance] Media-time (seconds, signed) advanced
 * over the first `dt` seconds of the ramp -- the closed-form integral, so
 * "position discontinuity at the seam" is provably zero in the model
 * (bounded only by the caller's own render-block quantization, which is
 * the audio engine's job, not this module's). */
float st_scrub_distance(const st_scrub_release_t *seg, float dt);

/* [ts:inertia.ts inertiaZeroCrossing] Seconds into the ramp at which a
 * reverse->forward release passes through exactly zero rate, or -1.0f if
 * the ramp never changes sign (forward release, or `from` was already 0). */
float st_scrub_zero_crossing(const st_scrub_release_t *seg);

#endif /* STEMTAPE_PLAYER_SCRUB_H_ */
