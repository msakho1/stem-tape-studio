/*
 * st_loop.h — the global loop transport: window capture, musical divisions,
 * direction, the FUNCTION-latch / PLAY-exit grammar, and the exact exit
 * position.
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
 *   PLAY press edge, song playing    -> ARM. Capture the audio thread's
 *                                       authoritative frame RIGHT NOW as
 *                                       the loop-start candidate. Nothing
 *                                       audible happens yet.
 *   PLAY tap (< ST_LOOP_HOLD_MS)     -> not ours; main.c's play/pause. The
 *                                       candidate is discarded.
 *   PLAY held to ST_LOOP_HOLD_MS     -> ENTER, with loop_start_frame equal
 *                                       to the ARMED candidate -- the frame
 *                                       where PLAY went DOWN, not where the
 *                                       threshold expired. main.c SEEKS
 *                                       back to it immediately.
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
 *   VOL -/+ while looping, with PLAY
 *   or FUNCTION held                 -> DIVISION. See below.
 *
 * PLAYBACK IS FORWARD ONLY. There is no reverse gesture and no reverse
 * traversal in this phase. Reverse is a later phase and its gesture will be a
 * Track-button DOUBLE-TAP -- not FUNCTION + PLAY, which an earlier revision
 * of this file invented and which was never authorised. "Loop end direction"
 * meant only what happens after LEAVING the loop: ordinary forward playback,
 * resuming at end_frame.
 *
 * THERE IS EXACTLY ONE PLAY-HOLD OWNER. The inherited Tape Looper's
 * "hold PLAY >= 400 ms -> restart from the top" is a different instrument's
 * gesture on the same button; with a Stem Tape song selected it is not
 * reachable at all (main.c gates it on the same flag that hands this module
 * the button), so nothing races this threshold. In st15 both existed and the
 * 400 ms one always won, which is why holding PLAY produced no loop.
 *
 * FUNCTION IS NOT THE UNLATCH CONTROL. Product ruling: FUNCTION latches,
 * PLAY exits. A latched loop is left by pressing PLAY, and both that press
 * AND its release are consumed.
 *
 * ======================================================================
 * ENTRY: THE LOOP STARTS WHERE THE FINGER LANDED
 * ======================================================================
 * The start frame is captured on the PLAY-DOWN EDGE and held as a candidate
 * across the hold. It is NOT sampled when the threshold expires.
 *
 * That is the difference between an instrument and a delay. Sampling at the
 * threshold means the loop begins ST_LOOP_HOLD_MS after the musical moment
 * the player chose, and -- worse -- the player then waits a further full
 * window before hearing the first repetition, because playback has to run
 * forward to the window's end before it can wrap. On a one-bar window at
 * 93.71 BPM that is 450 ms of offset followed by up to 2.56 s of nothing.
 * On real hardware that reads as "the loop takes three or four seconds".
 *
 * Capturing on the down edge and SEEKING BACK to it at the threshold makes
 * the first repetition audible AT the threshold. The whole song is already
 * in storage, so there is nothing to wait for -- only the sector, which
 * main.c has been pinning since the ARM.
 *
 * ======================================================================
 * HALF-OPEN BOUNDARIES, EVERYWHERE
 * ======================================================================
 * The looped section is [start_frame, end_frame) -- start INCLUSIVE, end
 * EXCLUSIVE -- in this module, in main.c's control and audio paths, in the
 * streamer and in the tests. No other convention appears anywhere.
 *
 *   playing:  start ... end-1, then wrap to start
 *   on exit:  the first normal frame is end
 *
 * ======================================================================
 * EXIT POSITION: THE FRAME AFTER THE LOOPED SECTION, FORWARD
 * ======================================================================
 * Every exit -- momentary release or latched-loop PLAY press -- resumes at
 * loop_end_frame, moving forward: the song picks up immediately AFTER the
 * section that was looping, so releasing is an instant return to the track
 * and nothing the player already heard is played again.
 *
 * CORRECTED FROM st15, on hardware evidence. st15 resumed at
 * loop_start_frame, and on a real SP-1 that reads as a defect: you release,
 * and the bar you were just looping plays through one more time before the
 * song moves on.
 *
 * This is NOT "where a hidden master timeline would have reached". There is
 * no such timeline and there never was: after four laps of a one-bar loop
 * the resume point is one bar past the capture, not four. Nor is it the live
 * frame inside the window -- that would rejoin mid-phrase. It is exactly the
 * first frame after the section that was looping.
 *
 * The ONE degenerate case: a window clamped to the very end of the song has
 * no frame after it. That exit resumes at frame 0, which is precisely what
 * the streamer already does when ordinary playback reaches the song end, so
 * it introduces no new behaviour and stays strictly in bounds. It cannot
 * replay any part of [start, end), because [start, end) ends at the song
 * end. st_loop_resume_frame() is the single place that answers "where", so
 * no caller can invent a different one.
 *
 * ======================================================================
 * DIVISION -- THE GESTURE IS FUNCTION + VOLUME -/+
 * ======================================================================
 * The agreed gesture, stated once so it cannot drift: while a global loop is
 * running -- latched, or momentary with PLAY still held -- FUNCTION together
 * with VOLUME - / VOLUME + sets the loop division. VOLUME alone is master
 * volume and is never captured by the loop; FUNCTION must be held.
 *
 * The four divisions are exactly src/machine/surface.ts:939's own shipped
 * global-loop table -- `order: (1|2|4|8)[] = [1, 2, 4, 8]`, read as bar
 * fractions -- with the same whole-bar default:
 *
 *      1 bar   |   1/2 bar   |   1/4 bar   |   1/8 bar
 *
 * COLD BOOT IS ALWAYS ONE BAR. st_loop_reset() -- called once at startup --
 * selects the whole bar, derived from the selected song's real bpm_q8,
 * sample rate and the documented 4 beats per bar. Nothing inherits a length
 * from retained RAM, from another song or from the classic engine. Within a
 * powered session the player's choice STICKS: entering a second loop keeps
 * whatever division was last selected, which is why the selection is not
 * re-defaulted on entry.
 *
 * st15 shipped an eight-entry ladder reaching 8 bars. That was never the
 * agreed gesture's range; it is gone rather than left as unreachable rungs.
 *
 * DIRECTION OF THE STEP: VOLUME + lengthens, VOLUME - shortens. surface.ts
 * steps `dir > 0` toward a larger division NUMBER, i.e. its Volume +
 * shortens; the product instruction for this firmware is the opposite, and
 * that is what is implemented. Anyone reconciling the two should change
 * surface.ts, not this file. Both ends CLAMP -- leaning on one button lands
 * on 1/8 bar or 1 bar and stays there rather than wrapping around.
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

#define ST_LOOP_LEN_COUNT   4u
#define ST_LOOP_LEN_DEFAULT 3u   /* 4 beats == one bar */


extern const st_loop_len_t st_loop_lengths[ST_LOOP_LEN_COUNT];

typedef enum {
	ST_LOOP_OFF = 0,
	ST_LOOP_MOMENTARY,   /* PLAY still held, not latched */
	ST_LOOP_LATCHED,     /* FUNCTION latched it; survives PLAY release */
} st_loop_state_t;

typedef enum {
	ST_LOOP_ACT_NONE = 0,
	ST_LOOP_ACT_ARM,      /* PLAY went down: candidate captured, pin it */
	ST_LOOP_ACT_ENTER,    /* window is now valid; SEEK to start, then loop */
	ST_LOOP_ACT_RESIZE,   /* end_frame changed; start_frame did not */
	ST_LOOP_ACT_LATCH,    /* momentary -> latched; window unchanged */
	ST_LOOP_ACT_EXIT,     /* resume forward at st_loop_resume_frame() */
} st_loop_action_t;

typedef struct {
	st_loop_state_t state;
	uint32_t start_frame;    /* captured at entry; NEVER moved by a resize */
	uint32_t end_frame;      /* exclusive; the wrap point */
	uint32_t resume_frame;   /* where the LAST exit resumed; see below */
	uint32_t cand_start;     /* frame captured on the PLAY-DOWN edge */
	uint32_t cand_end;       /* the window that candidate would produce */
	bool     cand_valid;     /* a PLAY press is armed and not yet resolved */
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

/* The armed candidate window, valid between ST_LOOP_ACT_ARM and either the
 * ENTER that consumes it or the release that discards it. main.c pins BOTH
 * of these sectors while the hold is still in progress, so the entry seek
 * and the eventual exit seek both land on resident data. */
static inline bool st_loop_armed(const st_loop_t *l)
{
	return l->cand_valid;
}

static inline uint32_t st_loop_cand_start(const st_loop_t *l)
{
	return l->cand_start;
}

static inline uint32_t st_loop_cand_end(const st_loop_t *l)
{
	return l->cand_end;
}

/*
 * Drop every in-flight gesture WITHOUT touching the player's chosen division.
 *
 * For the one case that needs it: no Stem Tape song is selected, so this
 * module owns nothing and must not carry a half-finished press across the
 * gap. st_loop_reset() would be wrong here -- it re-defaults the division,
 * which belongs to the powered session, not to a song selection.
 */
static inline void st_loop_reset_gesture_edges(st_loop_t *l)
{
	l->state              = ST_LOOP_OFF;
	l->play_consumed      = false;
	l->play_was_down      = false;
	l->fn_was_down        = false;
	l->entered_this_press = false;
	l->cand_valid         = false;
}

/* True while the loop owns playback (either momentary or latched). */
static inline bool st_loop_active(const st_loop_t *l)
{
	return l->state != ST_LOOP_OFF;
}

/*
 * THE resume position, for every exit: the frame immediately after the
 * looped section, always forward. Computed by st_loop_tick() on the pass
 * that reports ST_LOOP_ACT_EXIT, while the song length is in hand, and
 * exposed as the one answer so no caller can substitute the current frame,
 * the start frame, or a sector boundary.
 *
 * Also the frame main.c must PIN before the exit can happen: it is the first
 * frame the audio path will ask for after the seek, and at that moment it is
 * nowhere else in RAM.
 */
static inline uint32_t st_loop_resume_frame(const st_loop_t *l)
{
	return l->resume_frame;
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
 * streamer's prefetch cannot disagree about where the loop turns over. With
 * no loop active it is simply frame + 1 (bounded by the song).
 */
uint32_t st_loop_next_frame(const st_loop_t *l, uint32_t frame, uint32_t song_frames);



#endif /* ST_LOOP_H_ */
