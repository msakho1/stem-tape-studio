/*
 * st_seam.h — the loop seam ducker. A counter and a multiply; no buffers.
 *
 * PURE: no Zephyr, no allocation, no clock, no state beyond one small struct.
 *
 * ======================================================================
 * THE PROBLEM IT SOLVES, AND WHY DEPTH NEVER COULD
 * ======================================================================
 * A loop seam joins two unrelated points in a waveform. The frame before the
 * jump and the frame after it are adjacent in the output and arbitrarily far
 * apart in amplitude, so the seam is a step edge -- a click -- even when every
 * frame is present, nothing is repeated or skipped, and no sector is missing.
 *
 * st17 shipped exactly that. Its gate reported zero silent frames at entry,
 * wrap and exit, and all three were audible on hardware, because the gate
 * measured frame INDICES and the defect is in sample VALUES. Adding pinned
 * sectors cannot help: residency fixes starvation, and this is not
 * starvation.
 *
 * ======================================================================
 * THE TECHNIQUE IS THE BASE SP-1's, NOT A NEW ONE
 * ======================================================================
 * The golden Tape Looper already solves this, in firmware/src/main.c's
 * "BOUNDARY FADE" (around line 1962):
 *
 *     int32_t g = 256;
 *     if (avail < 256) g = avail;                    // fade OUT
 *     if (trk[i].fade < 256u) {
 *             if ((int32_t)trk[i].fade < g) g = (int32_t)trk[i].fade;
 *             trk[i].fade++;                         // fade IN
 *     }
 *     if (g < 256) sv = (int16_t)(((int32_t)sv * g) >> 8);
 *
 * -- with its own comments "dropouts duck instead of clicking" and, where the
 * fade is armed, "ramp back in (~5 ms), no click". At record time it does the
 * same thing into the loop seam: "the pad used to be hard zeros -- a click
 * baked into the seam; fade the first 128 pad samples (~2.7 ms) down instead."
 *
 * WHAT THAT IMPLEMENTATION DOES AND DOES NOT DO, since the answer decides the
 * cost:
 *
 *   * It is a linear gain ramp in 0..256, stepped once per output frame.
 *   * It does NOT keep two playheads.
 *   * It does NOT overlap outgoing and incoming audio.
 *   * It reuses NO scratch memory -- the whole mechanism is a uint16_t per
 *     track plus one multiply in the mixer.
 *   * It never touches I2S transport state.
 *
 * So the seam is smoothed by ducking the gain through the discontinuity, not
 * by crossfading buffered copies of two positions. This module is that, for
 * the four-stem stream: one gain, applied where the master volume is already
 * applied, costing one struct and no sectors.
 *
 * ======================================================================
 * THE SHAPE
 * ======================================================================
 * A seam is ST_SEAM_FRAMES down and ST_SEAM_FRAMES up, linear, symmetric:
 *
 *     gain 1.0 ______                    ______ 1.0
 *                   \                  /
 *                    \                /
 *     gain 0.0        \____ jump ____/
 *
 * The jump happens at the bottom, where the signal is at zero, so whatever
 * the two sides' amplitudes are the output step across the seam is zero by
 * construction. Both halves are audio -- the outgoing frames keep advancing
 * while the gain falls, and the incoming frames start at the target while it
 * rises -- so nothing is silent for the width of the duck, merely quiet.
 *
 * ST_SEAM_FRAMES is 128, which is 2.67 ms at 48 kHz: the base SP-1's own
 * shorter fade, the one it uses specifically for a loop seam rather than for
 * starve recovery. Total seam width is therefore 5.33 ms, exactly one output
 * block, so a seam never spans more than two blocks and the transition can
 * always be started and finished inside the audio thread with no scheduling.
 */

#ifndef ST_SEAM_H_
#define ST_SEAM_H_

#include <stdbool.h>
#include <stdint.h>

/* Frames in each half of the duck. 128 at 48 kHz = 2.67 ms, the base SP-1's
 * loop-seam fade length. */
#define ST_SEAM_FRAMES 128u

/* Gain is Q8: ST_SEAM_GAIN_UNITY is 1.0. */
#define ST_SEAM_GAIN_SHIFT 8
#define ST_SEAM_GAIN_UNITY (1u << ST_SEAM_GAIN_SHIFT)

typedef enum {
	ST_SEAM_IDLE = 0,   /* unity gain; nothing in flight */
	ST_SEAM_DOWN,       /* ramping to zero, still emitting outgoing audio */
	ST_SEAM_UP,         /* ramping back to unity from the new position */
} st_seam_phase_t;

typedef struct {
	st_seam_phase_t phase;
	uint16_t        step;   /* 0..ST_SEAM_FRAMES within the current phase */
} st_seam_t;

static inline void st_seam_reset(st_seam_t *s)
{
	s->phase = ST_SEAM_IDLE;
	s->step  = 0u;
}

/*
 * Arm a seam: begin ducking out. The caller keeps emitting ORDINARY forward
 * audio while st_seam_jump_due() is false, then performs its jump on the
 * frame that returns true, then keeps emitting from the new position.
 */
static inline void st_seam_begin(st_seam_t *s)
{
	/* Re-arming mid-seam restarts the duck from wherever the gain is,
	 * rather than snapping to unity -- two transitions closer together
	 * than the seam width (a wrap immediately followed by a release) must
	 * not produce a step of their own. */
	if (s->phase == ST_SEAM_UP) {
		s->step = (uint16_t)(ST_SEAM_FRAMES - s->step);
	} else if (s->phase == ST_SEAM_IDLE) {
		s->step = 0u;
	}
	s->phase = ST_SEAM_DOWN;
}

/* The gain to apply to THIS frame, Q8. */
static inline uint16_t st_seam_gain(const st_seam_t *s)
{
	switch (s->phase) {
	case ST_SEAM_DOWN:
		return (uint16_t)((ST_SEAM_GAIN_UNITY *
				    (ST_SEAM_FRAMES - s->step)) / ST_SEAM_FRAMES);
	case ST_SEAM_UP:
		return (uint16_t)((ST_SEAM_GAIN_UNITY * s->step) / ST_SEAM_FRAMES);
	case ST_SEAM_IDLE:
	default:
		return ST_SEAM_GAIN_UNITY;
	}
}

/* True on the frame whose gain has reached zero: the caller must jump now. */
static inline bool st_seam_jump_due(const st_seam_t *s)
{
	return s->phase == ST_SEAM_DOWN && s->step >= ST_SEAM_FRAMES;
}

/* Advance one output frame. */
static inline void st_seam_tick(st_seam_t *s)
{
	switch (s->phase) {
	case ST_SEAM_DOWN:
		if (s->step >= ST_SEAM_FRAMES) {
			s->phase = ST_SEAM_UP;
			s->step  = 0u;
		} else {
			s->step++;
		}
		break;
	case ST_SEAM_UP:
		if (s->step >= ST_SEAM_FRAMES) {
			st_seam_reset(s);
		} else {
			s->step++;
		}
		break;
	case ST_SEAM_IDLE:
	default:
		break;
	}
}

/* True while a seam is in flight, for the caller's own bookkeeping. */
static inline bool st_seam_active(const st_seam_t *s)
{
	return s->phase != ST_SEAM_IDLE;
}

#endif /* ST_SEAM_H_ */
