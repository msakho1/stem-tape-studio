/*
 * st_beat_phase.h — pure, RAM-only beat-phase computation for stem
 * playback LED feedback (Phase 3 control-matrix, beat-sync slice).
 *
 * SOURCE OF TRUTH: the selected, validated STIX record's own bpm_q8 and
 * downbeat_frame (docs/stem-tape-transfer-v1.1.md's song-level timing
 * metadata) are the authoritative inputs this module trusts for tempo —
 * never fabricated, never guessed from sector content. main.c's own boot
 * code (streamer_thread(), the one thread that reads eMMC) additionally
 * cross-checks the first sector's own header bpm_q8/downbeatFrame against
 * the STIX record's, for consistency, as a boot-time diagnostic only (see
 * main.c's own comment at that call site) — the STIX record always wins.
 * This module itself never reads a sector, never touches eMMC, and has no
 * concept of either source beyond the plain bpm_q8/downbeat_frame/
 * sample_rate values st_beat_timing_init() is given.
 *
 * ONE CLOCK, NOT TWO: st_beat_pulse() takes the CURRENT master
 * song_frame (st_stream_t's own "the ONE authoritative absolute song
 * frame") fresh on every call — there is no second, independently-
 * ticking clock anywhere in this module. A loop wrap (song_frame resets
 * via st_stream_t's own LOOPED tick) or any future variable-speed
 * playback changes song_frame's own rate of advance; since phase is
 * always re-derived from whatever song_frame IS right now, neither can
 * ever desynchronize a second clock from the real one.
 *
 * FIXED-POINT, NO FLOATS: frames_per_beat is computed once, in
 * st_beat_timing_init(), from 64-bit integer arithmetic (bpm_q8 is
 * Q8.8; the intermediate product is a uint64_t, rounded to the nearest
 * whole frame before the one division — no floating point anywhere,
 * matching this whole codebase's real-time-audio-path convention). The
 * per-call phase query is one subtraction and one plain uint32_t modulo
 * — cheap enough to call from led_service()'s own ~8 ms control-loop
 * resolution without concern.
 *
 * FAIL CLOSED, NEVER FABRICATED: bpm_q8 == 0 or sample_rate == 0 (tempo
 * absent or invalid) leaves the timing snapshot invalid
 * (frames_per_beat == 0); st_beat_pulse() reports valid=false for an
 * invalid snapshot — the caller's dark fallback is the correct answer,
 * never an invented tempo or pattern. song_frame < downbeat_frame (the
 * song has not yet reached its first downbeat — true briefly after boot
 * on a song with a pickup/lead-in, and again briefly after every loop
 * wrap if downbeat_frame > 0) uses the SAME fallback, not a fabricated
 * pre-roll pattern.
 *
 * NO DISPLAY DECISION LIVES HERE. This module answers timing questions
 * only. Every LED decision — which lights, how bright, in what priority
 * order — belongs to st_led_mvp.c, the single semantic owner of all
 * eight LEDs. An earlier revision of this header exported a track-LED
 * on/off/ghost decision (st_beat_led_decide()) alongside a bare
 * "is a beat happening" boolean (st_beat_phase_on_beat()). Both are
 * DELETED, deliberately and not merely unwired: the boolean handed the
 * same value to all four track LEDs, so they flashed uniformly and
 * carried no bar position, and the ghost/solid vocabulary it fed was
 * retired outright when the track row moved to real 0..255 brightness.
 * st_beat_pulse() below supersedes both — it returns bar position and an
 * envelope, which is what a chase and a fade actually need. Do not
 * reintroduce either function; wiring a tempo boolean back into the LED
 * path is the exact regression this deletion exists to prevent.
 *
 * PURE: no I/O, no Zephyr, no dynamic allocation.
 */

#ifndef STEMTAPE_PLAYER_BEAT_PHASE_H_
#define STEMTAPE_PLAYER_BEAT_PHASE_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t downbeat_frame;
	uint32_t frames_per_beat; /* 0 == invalid/unavailable -- see this header's own doc comment */
} st_beat_timing_t;

/*
 * Computes and stores frames_per_beat from `bpm_q8` (Q8.8, STIX record
 * convention; 0 = absent/unknown) and `sample_rate` (the STIX record's
 * own field). Returns true and leaves a valid (frames_per_beat != 0)
 * snapshot iff both bpm_q8 and sample_rate are nonzero and the result
 * fits a uint32_t; otherwise returns false and leaves *out with
 * frames_per_beat == 0 (downbeat_frame is still copied through either
 * way, but is meaningless without a valid frames_per_beat).
 */
bool st_beat_timing_init(st_beat_timing_t *out, uint32_t bpm_q8, uint32_t downbeat_frame, uint32_t sample_rate);

/* ---- BEAT PULSE: envelope + bar position (Option C) ---------------------
 *
 * This is the module's ONLY per-call query, and the only thing the LED
 * layer ever asks it. The playing display needs three things at once, all
 * derived from the SAME song_frame so no second clock can exist:
 *
 *   in_pulse   -- the LEDs are lit at all only inside a short window at the
 *                 start of each beat; between pulses everything is dark.
 *   envelope   -- 0..255 across that window, rising and falling, so the
 *                 pulse reads as a pulse rather than a square blink.
 *   beat_index -- 0..3 within the bar, which is what makes a 1->2->3->4
 *                 chase possible. Beat 0 is the downbeat.
 *
 * All three come out of one call so they cannot disagree with each other.
 */
typedef struct {
	bool    valid;       /* timing usable AND song_frame at/after the downbeat */
	bool    in_pulse;    /* inside this beat's pulse window */
	uint8_t envelope;    /* 0..255 within the window; 0 when !in_pulse */
	uint8_t beat_index;  /* 0..3; 0 == downbeat. Meaningless unless valid */
} st_beat_pulse_t;

/* Pulse window as a fraction of one beat: PULSE_NUM/PULSE_DEN. Centralized
 * here so the look is tuned in one place. 1/4 of a beat lit, 3/4 dark. */
#define ST_BEAT_PULSE_NUM 1u
#define ST_BEAT_PULSE_DEN 4u

/*
 * Fills *out from a VALID timing snapshot and the current master song_frame.
 * Fails closed: an invalid snapshot (frames_per_beat == 0) or a song_frame
 * still before the first downbeat yields valid=false, in_pulse=false,
 * envelope=0, beat_index=0 -- never a fabricated tempo and never a
 * fabricated bar position.
 *
 * The envelope is a symmetric triangle across the window: it rises to 255 at
 * the window's midpoint and falls back, which gives the visible rise/fall a
 * square gate does not. Integer only, no floats -- this is called from the
 * control loop, but the whole codebase's real-time convention applies.
 */
void st_beat_pulse(const st_beat_timing_t *timing, uint32_t song_frame,
		    st_beat_pulse_t *out);

#endif /* STEMTAPE_PLAYER_BEAT_PHASE_H_ */
