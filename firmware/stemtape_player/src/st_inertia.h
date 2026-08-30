/*
 * st_inertia.h -- tape transport inertia: the reel has mass.
 *
 * PLAY does not begin at speed and STOP does not end at speed. The transport
 * spins up and spins down, and because playback position advances at the
 * instantaneous rate, PITCH AND TIME MOVE TOGETHER -- the low, stretched
 * "wuuuup" on start and the classic falling tape-stop on release. This is not
 * a volume fade and nothing here touches gain.
 *
 * ======================================================================
 * WHAT THIS MODULE IS, AND IS NOT
 * ======================================================================
 * It is an ENVELOPE, 0..1 in Q16, and nothing else. It never knows the song,
 * the sample rate in Hz, or what rate the player asked for. The caller
 * multiplies:
 *
 *     rate_q16 = (requested_rate_q16 * st_inertia_env_q16(s)) >> 16
 *
 * so inertia LAYERS OVER varispeed rather than replacing it. With a 0.8x
 * target selected, PLAY ramps 0 -> 0.8x and STOP ramps 0.8x -> 0, because the
 * envelope still runs 0 -> 1 -> 0 underneath. That composition is the whole
 * reason this is an envelope and not a rate.
 *
 * It owns no clock. The caller advances it in FRAMES, from the same audio
 * clock that advances the playhead, so the ramp cannot drift against the
 * audio it is bending.
 *
 * ======================================================================
 * THE CURVES, AND WHY THEY ARE TABLES
 * ======================================================================
 * Both curves are 33-entry Q16 PROGRESS tables (0 -> 65536), linearly
 * interpolated. Deliberately tables rather than a closed form: the brief was
 * that the timing and feel must be tunable by ear on hardware, and a table is
 * something you can reshape by editing sixteen numbers without re-deriving
 * any algebra. It also keeps the per-frame cost to a compare, a multiply and
 * a shift.
 *
 * SPIN-UP is a smoothstep, 3t^2 - 2t^3: gentle off the mark, strongest
 * acceleration through the middle, settling softly into the target rather
 * than arriving with a corner. A corner at the top is audible as a click in
 * the pitch contour, which is exactly what "feels like mass" excludes.
 *
 * SPIN-DOWN is exponential-ish decay, 1 - 2^(-6t): the speed falls
 * immediately and smoothly at first, then the curve flattens into a long
 * crawl at low rate. That flattening is what produces the classic tape stop.
 * Note the crawl is a PERCEPTUAL acceleration even though the absolute rate
 * of change is falling -- pitch is logarithmic, so 0.4x -> 0.2x is the same
 * octave drop as 1.0x -> 0.5x, and the last stretch is where the drama is.
 *
 * ======================================================================
 * PARTIAL RAMPS ARE SCALED, NOT RESTARTED
 * ======================================================================
 * Pressing PLAY while the reel is still spinning down must CATCH it, not
 * drop it to zero and start again -- that would be a discontinuity in rate,
 * heard as a click, and it is not what a physical transport does. So a ramp
 * always runs from wherever the envelope already is, and its duration is
 * scaled by the distance left to cover. A reel already at 0.8 reaches 1.0
 * quickly; one at 0.05 takes nearly the full spin-up.
 */
#ifndef ST_INERTIA_H
#define ST_INERTIA_H

#include <stdbool.h>
#include <stdint.h>

/* Envelope fixed point. 65536 == 1.0 == the caller's requested rate. */
#define ST_INERTIA_ONE 65536u

/* ---- TUNING, by ear, on hardware -------------------------------------
 * Full-travel durations in milliseconds. A ramp that starts part-way takes
 * proportionally less. Both sit inside the brief's ranges (spin-up 250-500,
 * spin-down 400-800) and are the two numbers to reach for first. */
#define ST_INERTIA_SPINUP_MS   350u
#define ST_INERTIA_SPINDOWN_MS 600u

/* The envelope below which the transport is considered at rest and playback
 * is cut. 1/256 of nominal is about eight octaves down -- inaudible as pitch,
 * and low enough that ending there is not a step. Ending at exactly zero
 * instead would mean waiting out an asymptote that never arrives. */
#define ST_INERTIA_REST_Q16 256u

/* A ramp shorter than this is not worth running; the envelope is set
 * directly. Guards the degenerate "PLAY at 0.999" case from producing a
 * one-frame ramp whose duration arithmetic divides by almost nothing. */
#define ST_INERTIA_MIN_RAMP_FRAMES 32u

typedef enum {
	ST_INERTIA_STOPPED = 0,  /* at rest; no audio need be rendered */
	ST_INERTIA_SPINUP,       /* accelerating toward the requested rate */
	ST_INERTIA_RUNNING,      /* at the requested rate, envelope == 1.0 */
	ST_INERTIA_SPINDOWN,     /* decelerating toward rest */
} st_inertia_state_t;

typedef struct {
	uint8_t  state;
	uint32_t elapsed;    /* frames into the current ramp */
	uint32_t total;      /* frames the current ramp will take */
	uint32_t from_q16;   /* envelope when this ramp began */
	uint32_t to_q16;     /* envelope this ramp is heading for */
	uint32_t env_q16;    /* the current envelope: what the caller reads */
} st_inertia_t;

void st_inertia_reset(st_inertia_t *s);

/*
 * PLAY. Begins spinning up toward full rate from wherever the reel is now --
 * including mid-spin-down, which catches it rather than restarting.
 * `sample_rate` converts the millisecond constants into frames; it is passed
 * rather than assumed so nothing here hard-codes 48 kHz.
 */
void st_inertia_play(st_inertia_t *s, uint32_t sample_rate);

/* STOP. Begins spinning down toward rest from wherever the reel is now. */
void st_inertia_stop(st_inertia_t *s, uint32_t sample_rate);

/*
 * Advance `frames` output frames. The envelope is recomputed from the ramp's
 * own elapsed count rather than integrated step by step, so advancing 1 frame
 * 256 times and advancing 256 frames once give the SAME envelope -- the audio
 * thread renders in runs of varying length and must not get a different ramp
 * for it.
 */
void st_inertia_advance(st_inertia_t *s, uint32_t frames);

/* The multiplier to apply to the requested rate, Q16. */
static inline uint32_t st_inertia_env_q16(const st_inertia_t *s)
{
	return s->env_q16;
}

/*
 * True while the transport is moving at all, i.e. while audio must still be
 * read and mixed. This is what makes STOP not a mute: the caller keeps
 * rendering through the whole spin-down and only silences the output once
 * this returns false.
 */
static inline bool st_inertia_moving(const st_inertia_t *s)
{
	return s->state != ST_INERTIA_STOPPED;
}

/* True only at exactly nominal rate. The audio path uses this to take its
 * proven 1:1 fast path, so ordinary playback is bit-identical to a build
 * without inertia and the interpolating reader runs only during a ramp. */
static inline bool st_inertia_at_unity(const st_inertia_t *s)
{
	return s->state == ST_INERTIA_RUNNING;
}

#endif /* ST_INERTIA_H */
