/*
 * st_pitch.h -- song-level semitone control for Stem Tape, on the FWD/RWD
 * rocker, driving the SAME tape varispeed the Tape Looper has always used.
 *
 * ======================================================================
 * THIS IS AN ADAPTATION, NOT A NEW ENGINE. WHAT CAME FROM WHERE.
 * ======================================================================
 * The inherited Tape Looper (firmware/src/main.c) already has all of this
 * except the half-steps, and the parts reused here are named so the two
 * cannot quietly diverge:
 *
 *   THE GRID.  k_semi_q16[25] is 2^(k/12) in Q16 for k = -12..+12, i.e.
 *              round(65536 * 2^(k/12)). k_pitch_q16 below is the SAME
 *              formula at half the step, round(65536 * 2^(k/24)), so every
 *              whole semitone in this table is bit-identical to the Looper's
 *              entry for that semitone. A host test asserts that, entry by
 *              entry, so the two grids cannot drift apart.
 *
 *   THE MEANING OF THE NUMBER. In the Looper the Q16 value IS the playback
 *              speed (g_play_speed_q16), so pitch and time move together --
 *              tape varispeed. That is preserved exactly: this module returns
 *              a rate, the stem transport reads the tape at that rate, and the
 *              pitch follows because it is the same physical quantity. There
 *              is no time-stretching anywhere in this path and none was added.
 *
 *   THE DOUBLE-CLICK WINDOW. 350 ms, the Looper's own figure for "a second
 *              click in the same direction is one gesture, not two".
 *
 * WHAT IS DELIBERATELY DIFFERENT, and why, because "reuse" should not mean
 * copying a decision that does not transfer:
 *
 *   STEP SIZE. The Looper's rocker is a TEMPO control: a single click is
 *              1 BPM and only a double-click reaches for a semitone. Here the
 *              rocker is a PITCH control, so a single click is a half
 *              semitone and a double is a whole one. Same gesture vocabulary,
 *              different quantity, because the control means something
 *              different.
 *
 *   WHEN THE SINGLE COMMITS. The Looper fires the single click IMMEDIATELY
 *              and, if a second arrives, computes the semitone from the speed
 *              BEFORE the first so the click already applied is "absorbed,
 *              not compounded". That is right for tempo: zero latency, and a
 *              double-click SNAPS to an absolute grid point, so replacing the
 *              first click's effect is well defined.
 *
 *              It does not transfer to a CUMULATIVE control. Absorbing works
 *              because the Looper's double-click is absolute; here +0.5 then
 *              +1.0 are relative, and firing the single first would put the
 *              song briefly at +0.5 before landing on +1.0 -- an audible
 *              half-step blip on every double, which on a pitch control is
 *              exactly the artefact this is meant to avoid. So a single is
 *              HELD for the window and committed only if no second click
 *              arrives. The cost is that a single click is a window late; the
 *              benefit is that a double NEVER emits the single action at all,
 *              which is the requirement.
 *
 * ======================================================================
 * THE RANGE IS NOT SYMMETRIC, AND THE REASON IS THE eMMC
 * ======================================================================
 * Pitching DOWN is free -- slower playback reads less tape per second. The
 * floor is -12.0 semitones (0.5x), matching the Looper's own bottom.
 *
 * Pitching UP is bounded by READ THROUGHPUT, not by the pitch maths, and the
 * bound is derived from st_latency.h's own measured figures rather than
 * chosen:
 *
 *     one sector holds        ST_LAT_SECTOR_US   = 7083 us of audio
 *     a typical read costs    ST_LAT_READ_TYP_US = 5073 us
 *     so sustained playback needs 7083/5073 = 1.396x at the absolute limit
 *     with st_latency.h's own ST_LAT_MARGIN_PCT (15%): 1.187x
 *
 * Read-ahead depth does not change this. Depth absorbs a single worst-case
 * read; it cannot cover a sustained deficit, because if the average read
 * cannot keep up then no depth is enough -- the ring simply drains more
 * slowly. Above that rate the stream starves, and this file's own history
 * records what starvation sounds like: stored songs playing "slow and
 * crushed".
 *
 * +2.5 semitones (1.1554x) is the last half-step under the margined limit;
 * +3.0 would be 1.1892x, over it. So the range is -12.0 .. +2.5, and the
 * ceiling is a THROUGHPUT constant that moves if the read path ever gets
 * faster -- not a musical judgement.
 */

#ifndef STEMTAPE_PLAYER_PITCH_H_
#define STEMTAPE_PLAYER_PITCH_H_

#include <stdbool.h>
#include <stdint.h>

/* Q16 unity, the same fixed point the Looper's speed and the stem
 * transport's rate both use. */
#define ST_PITCH_ONE 65536u

/*
 * THE STATE IS COUNTED IN HALF SEMITONES, so the value is an integer and the
 * grid is exact. One step is 50 cents; +1 is +0.5 semitones, -3 is -1.5.
 *
 * Deliberately not a fixed-point "semitones" value: half-steps are the
 * smallest thing the control can produce, so counting them makes every
 * reachable state exactly representable and makes accumulation exact. The
 * brief's "do not round the internal pitch state to integers" is about not
 * losing the half-step -- which counting halves is the strongest form of.
 */
#define ST_PITCH_MIN_HALF (-24)   /* -12.0 semitones, 0.5x -- the Looper's floor */
#define ST_PITCH_MAX_HALF (5)     /* +2.5 semitones, 1.1554x -- see the header */

#define ST_PITCH_SINGLE_HALF 1    /* one rocker click  = 0.5 semitone */
#define ST_PITCH_DOUBLE_HALF 2    /* a double-click    = 1.0 semitone */

/* The Looper's own double-click window, reused verbatim. */
#define ST_PITCH_DOUBLE_MS 350u

typedef enum {
	ST_PITCH_ACT_NONE = 0,
	ST_PITCH_ACT_SINGLE_UP,
	ST_PITCH_ACT_SINGLE_DOWN,
	ST_PITCH_ACT_DOUBLE_UP,
	ST_PITCH_ACT_DOUBLE_DOWN,
} st_pitch_action_t;

typedef struct {
	int16_t  half;        /* the song's pitch, in half semitones */
	int8_t   pend_dir;    /* 0 none; +1/-1 a click waiting out its window */
	uint32_t pend_ms;     /* when that click arrived */
} st_pitch_t;

/* Zeroes the pitch and drops any pending click. */
void st_pitch_reset(st_pitch_t *p);

/*
 * ONE FRESH ROCKER CLICK, `dir` being +1 for up and -1 for down. Call this on
 * the press EDGE only -- a held rocker is not a stream of clicks.
 *
 * Returns the action committed by THIS call, which is
 *   ST_PITCH_ACT_NONE       the click is being held pending its window
 *   ST_PITCH_ACT_DOUBLE_*   it completed a double, and the pending single
 *                           was discarded without ever being applied
 *   ST_PITCH_ACT_SINGLE_*   a pending click in the OTHER direction expired
 *                           early because this one reversed it
 */
st_pitch_action_t st_pitch_click(st_pitch_t *p, int dir, uint32_t now_ms);

/*
 * Call every control pass. Commits a pending single once ST_PITCH_DOUBLE_MS
 * has passed with no second click. This is the only place a single is ever
 * applied, which is what makes "a double never also fires the single" a
 * structural property rather than a timing coincidence.
 */
st_pitch_action_t st_pitch_tick(st_pitch_t *p, uint32_t now_ms);

/*
 * The playback rate for the current pitch, Q16. This is a SPEED: the stem
 * transport advances the playhead at it, so time and pitch move together.
 */
uint32_t st_pitch_ratio_q16(const st_pitch_t *p);

/* Cents, for diagnostics and display. +1 half-step == +50 cents exactly. */
static inline int st_pitch_cents(const st_pitch_t *p)
{
	return (int)p->half * 50;
}

/* True when the pitch is exactly nominal, so the caller can take its
 * bit-identical 1:1 path. */
static inline bool st_pitch_is_unity(const st_pitch_t *p)
{
	return p->half == 0;
}

#endif /* STEMTAPE_PLAYER_PITCH_H_ */
