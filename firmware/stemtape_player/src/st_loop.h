/*
 * st_loop.h — the global loop transport: window capture, musical lengths,
 * the FUNCTION-latch / PLAY-unlatch grammar, and the exact exit position.
 *
 * PURE: no Zephyr, no eMMC, no clock, no allocation, no floating point.
 * main.c gathers the real button edges and the real master song frame, calls
 * st_loop_tick() once per control pass, and acts on the returned action. The
 * whole grammar lives here so it can be exercised deterministically.
 *
 * ======================================================================
 * ONE CLOCK, AND IT IS THE AUDIO PATH'S
 * ======================================================================
 * Every frame number this module handles is the master song frame that
 * st_stream_t advances and audio_block_epilogue() publishes. This module
 * never counts time, never advances a position, and never holds a timeline
 * of its own. It captures a frame, computes a window from it, and reports
 * where playback should be -- nothing here can drift against the audio.
 *
 * ======================================================================
 * THE GRAMMAR
 * ======================================================================
 *   PLAY tap (< ST_LOOP_HOLD_MS)     -> not ours; main.c's play/pause.
 *   PLAY held to ST_LOOP_HOLD_MS     -> ENTER. Capture loop_start_frame
 *                                       from the CURRENT master frame; do
 *                                       not pause first, do not seek.
 *   ...still held                    -> loop continues (MOMENTARY).
 *   PLAY released, never latched     -> EXIT, resuming at loop_start_frame.
 *   FUNCTION pressed while PLAY held -> LATCH. FUNCTION only ever latches;
 *                                       pressing it again does nothing.
 *   PLAY released while latched      -> nothing. The loop continues and
 *                                       does NOT relocate.
 *   new PLAY press while latched     -> EXIT on that press EDGE, resuming
 *                                       at loop_start_frame, and the press
 *                                       is marked consumed.
 *   that press's release             -> nothing at all: no pause, no new
 *                                       loop, no transport command.
 *
 * ======================================================================
 * EXIT POSITION: THE CAPTURED FRAME, FORWARD
 * ======================================================================
 * Every exit -- momentary release or latched-loop PLAY press -- resumes at
 * exactly loop_start_frame, moving forward. Never the current frame inside
 * the loop, never loop_end_frame, never a sector boundary, never zero.
 * st_loop_resume_frame() is the single place that answers "where", so no
 * caller can invent a different one.
 *
 * ======================================================================
 * LENGTHS
 * ======================================================================
 * PROVENANCE, and one deliberate divergence. src/machine/surface.ts:939
 * carries the web app's own authoritative global-loop table -- `order:
 * (1|2|4|8)[] = [1, 2, 4, 8]`, read as bar fractions 1/1, 1/2, 1/4 and 1/8
 * of a bar, defaulting to a whole bar. All four of those lengths appear in
 * the table below (1 bar, 2 beats, 1 beat, 1/2 beat), so this is a superset
 * of the shipped web model, not a competing one, and the default is the
 * same whole bar.
 *
 * The divergence is DIRECTION, and it is deliberate rather than an
 * oversight. surface.ts steps `dir > 0` toward a LARGER division number,
 * which makes its Volume + shorten the loop. The product instruction for
 * this firmware is the opposite -- Volume + lengthens, Volume - shortens --
 * and that is what is implemented. Anyone reconciling the two should change
 * surface.ts, not this file.
 *
 * Lengths are exact multiples/divisors of the song's OWN beat, derived from
 * the selected STIX record's bpm_q8 and downbeat_frame through
 * st_beat_timing_init(). No wall clock, no tempo detection, no floating
 * point: frames = frames_per_beat * num / den in 64-bit integer arithmetic,
 * rounded down, computed once per resize at control rate.
 *
 * FAIL CLOSED. A song with no trustworthy tempo (frames_per_beat == 0)
 * cannot produce a musical window, so the loop refuses to start at all
 * rather than inventing a length. A window that would run past the
 * committed song's end is clamped to the song end; a capture point with no
 * room left refuses. Nothing here can ever address a frame outside the
 * committed song region.
 */

#ifndef ST_LOOP_H_
#define ST_LOOP_H_

#include <stdbool.h>
#include <stdint.h>

/* [contract v1] playTapHoldMs -- the same 450 ms threshold st_gesture.h
 * already names, reused verbatim rather than invented again. */
#define ST_LOOP_HOLD_MS 450u

/* One entry of the musical length ladder: num/den beats. */
typedef struct {
	uint16_t num;
	uint16_t den;
} st_loop_len_t;

#define ST_LOOP_LEN_COUNT   8u
#define ST_LOOP_LEN_DEFAULT 4u   /* 4 beats == one bar */

extern const st_loop_len_t st_loop_lengths[ST_LOOP_LEN_COUNT];

typedef enum {
	ST_LOOP_OFF = 0,
	ST_LOOP_MOMENTARY,   /* PLAY still held, not latched */
	ST_LOOP_LATCHED,     /* FUNCTION latched it; survives PLAY release */
} st_loop_state_t;

typedef enum {
	ST_LOOP_ACT_NONE = 0,
	ST_LOOP_ACT_ENTER,    /* window is now valid; start looping */
	ST_LOOP_ACT_RESIZE,   /* end_frame changed; start_frame did not */
	ST_LOOP_ACT_LATCH,    /* momentary -> latched; window unchanged */
	ST_LOOP_ACT_EXIT,     /* resume forward at st_loop_resume_frame() */
} st_loop_action_t;

typedef struct {
	st_loop_state_t state;
	uint32_t start_frame;    /* captured at entry; NEVER moved by a resize */
	uint32_t end_frame;      /* exclusive; the wrap point */
	uint8_t  length_index;   /* into st_loop_lengths[] */
	bool     play_consumed;  /* this PLAY press already exited a latched loop */
	bool     play_was_down;  /* edge detection for PLAY */
	bool     fn_was_down;    /* edge detection for FUNCTION */
	bool     entered_this_press; /* this PLAY press already started a loop */
} st_loop_t;

typedef struct {
	bool     play_down;
	uint32_t play_held_ms;    /* 0 unless play_down */
	bool     function_down;

	/* ONE PRESS, ONE STEP. These are press EDGES, not levels: the caller
	 * must present each physical Volume press exactly once. main.c's
	 * existing 3-consecutive-read sticky commit already produces exactly
	 * that, so no second debounce is added here -- a bouncing button
	 * cannot reach this module. */
	bool     vol_minus_edge;
	bool     vol_plus_edge;

	bool     playing;         /* transport actually running */
	uint32_t song_frame;      /* THE authoritative master frame, right now */
	uint32_t song_frames;     /* total frames in the committed song */
	uint32_t frames_per_beat; /* from the STIX record; 0 == no tempo */
} st_loop_in_t;

void st_loop_reset(st_loop_t *l);

/*
 * One control pass. Total and deterministic: same state plus same inputs
 * gives the same action and the same window, with no hidden time source.
 * At most one action is reported per call; the grammar never needs two.
 */
st_loop_action_t st_loop_tick(st_loop_t *l, const st_loop_in_t *in);

/* True while the loop owns playback (either momentary or latched). */
static inline bool st_loop_active(const st_loop_t *l)
{
	return l->state != ST_LOOP_OFF;
}

/*
 * THE resume position, for every exit. Always the captured start frame,
 * always forward. Exposed as the one answer so no caller can substitute
 * the current frame, the end frame, or a sector boundary.
 */
static inline uint32_t st_loop_resume_frame(const st_loop_t *l)
{
	return l->start_frame;
}

/*
 * The window length in frames for `index` at this tempo, clamped so the
 * window never runs past `song_frames` from `start`. Returns 0 when no
 * usable window exists (no tempo, or no room left in the song) -- the
 * caller must then not loop rather than loop over nothing.
 */
uint32_t st_loop_window_frames(uint8_t index, uint32_t frames_per_beat,
			        uint32_t start, uint32_t song_frames);

/*
 * The next master frame given the current one, honouring the loop window.
 * This is the ONE place the wrap is decided, so the audio path and the
 * streamer's prefetch cannot disagree about where the loop turns over.
 * With no loop active it is simply frame + 1 (bounded by the song).
 */
uint32_t st_loop_next_frame(const st_loop_t *l, uint32_t frame, uint32_t song_frames);

#endif /* ST_LOOP_H_ */
