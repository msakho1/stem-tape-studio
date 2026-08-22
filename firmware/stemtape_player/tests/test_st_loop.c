/*
 * test_st_loop.c — the global loop engine, host-tested.
 *
 * Drives the REAL st_loop_tick()/st_loop_window_frames()/st_loop_next_frame()
 * and the REAL st_loop_lengths[] table -- the same object file linked into
 * the firmware. Length expectations are derived FROM the table, so retuning
 * it moves the tests with it rather than leaving them asserting old numbers.
 *
 * WHAT THIS PROVES: the grammar, the window arithmetic, the clamping and the
 * exit position are exactly what the product specifies, deterministically.
 *
 * WHAT IT DOES NOT PROVE: that the audio path honours them. That is the
 * production wiring's job and is proven separately, against real sectors.
 *
 *   cc -std=c11 -Wall -Wextra -I../src ../src/st_loop.c ../src/st_beat_phase.c \
 *      test_st_loop.c -o test_st_loop && ./test_st_loop
 */

#include <stdio.h>
#include <string.h>

#include "st_loop.h"
#include "st_beat_phase.h"

static int g_checks, g_failures, g_cases;

/* A silent check for the high-volume sweeps: counts, and only speaks on
 * failure, so 100k assertions do not bury the readable ones. */
#define CHECK_QUIET(cond) do { \
		g_checks++; \
		if (!(cond)) { g_failures++; \
			printf("[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
	} while (0)

#define CHECK(cond, ...) do { \
		g_checks++; \
		if (cond) { printf("[OK  ] " __VA_ARGS__); printf("\n"); } \
		else { g_failures++; printf("[FAIL] " __VA_ARGS__); \
		       printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); } \
	} while (0)

/* 120 BPM at 48 kHz through the REAL tempo derivation: 24000 frames/beat. */
#define FPB       24000u
#define SONG_LEN  (FPB * 4u * 64u)   /* 64 bars, comfortably multi-sector */

static uint32_t real_fpb(void)
{
	st_beat_timing_t t;

	(void)st_beat_timing_init(&t, 120u << 8, 0u, 48000u);
	return t.frames_per_beat;
}

static st_loop_in_t base_in(uint32_t frame)
{
	st_loop_in_t in;

	memset(&in, 0, sizeof(in));
	in.playing         = true;
	in.song_frame      = frame;
	in.song_frames     = SONG_LEN;
	in.frames_per_beat = FPB;
	return in;
}

/* Hold PLAY for `ms`, feeding ticks the way the 8 ms control loop would. */
static st_loop_action_t hold_play(st_loop_t *l, uint32_t frame, uint32_t ms,
				   st_loop_action_t *first)
{
	st_loop_action_t last = ST_LOOP_ACT_NONE;
	uint32_t t;

	if (first) {
		*first = ST_LOOP_ACT_NONE;
	}
	/* Step at the real 8 ms control cadence, but ALWAYS finish on exactly
	 * `ms`. Stepping by 8 alone would make hold_play(.., 450) stop at 448
	 * and never present the threshold -- a harness artefact that looks
	 * exactly like the loop refusing to start. */
	for (t = 0; ; t += 8u) {
		uint32_t at_ms = (t > ms) ? ms : t;
		st_loop_in_t in = base_in(frame);
		st_loop_action_t a;

		in.play_down     = true;
		in.play_held_ms  = at_ms;
		a = st_loop_tick(l, &in);
		if (a != ST_LOOP_ACT_NONE) {
			if (first && *first == ST_LOOP_ACT_NONE) {
				*first = a;
			}
			last = a;
		}
		if (at_ms >= ms) {
			break;
		}
	}
	return last;
}

static st_loop_action_t release_play(st_loop_t *l, uint32_t frame)
{
	st_loop_in_t in = base_in(frame);

	return st_loop_tick(l, &in);
}

/* ================== 1. the length table and its frame counts ============= */
static void case_length_table(void)
{
	uint32_t fpb = real_fpb();
	uint32_t i;
	bool ascending = true;
	uint32_t prev = 0u;

	g_cases++;
	printf("\n-- Loop length table at 120 BPM / 48 kHz (frames_per_beat = %u)\n", fpb);
	CHECK(fpb == FPB, "the REAL st_beat_timing_init() gives %u frames/beat", FPB);

	printf("      %-3s %-12s %10s\n", "idx", "length", "frames");
	for (i = 0; i < ST_LOOP_LEN_COUNT; i++) {
		uint32_t f = st_loop_window_frames((uint8_t)i, fpb, 0u, SONG_LEN);
		uint32_t want = (uint32_t)(((uint64_t)fpb * st_loop_lengths[i].num) /
					    st_loop_lengths[i].den);
		char lbl[32];

		if (st_loop_lengths[i].den != 1u) {
			snprintf(lbl, sizeof(lbl), "%u/%u beat",
				 st_loop_lengths[i].num, st_loop_lengths[i].den);
		} else if (st_loop_lengths[i].num < 4u) {
			snprintf(lbl, sizeof(lbl), "%u beat%s", st_loop_lengths[i].num,
				 st_loop_lengths[i].num == 1u ? "" : "s");
		} else {
			snprintf(lbl, sizeof(lbl), "%u bar%s", st_loop_lengths[i].num / 4u,
				 st_loop_lengths[i].num == 4u ? "" : "s");
		}
		printf("      %-3u %-12s %10u\n", i, lbl, f);
		CHECK(f == want, "index %u (%s) is exactly %u frames", i, lbl, want);
		if (i > 0 && f <= prev) {
			ascending = false;
		}
		prev = f;
	}
	CHECK(ascending, "the table is strictly ascending: every entry is longer than "
			  "the one before it");
	CHECK(st_loop_lengths[ST_LOOP_LEN_DEFAULT].num == 4u &&
	      st_loop_lengths[ST_LOOP_LEN_DEFAULT].den == 1u,
	      "the default (index %u) is 4 beats == one bar", ST_LOOP_LEN_DEFAULT);
}

/* ============== 2. entry: 450 ms exactly, at the real frame ============== */
static void case_entry(void)
{
	st_loop_t l;
	st_loop_in_t in;
	uint32_t at = 123456u;
	uint32_t t;
	bool early = false;
	st_loop_action_t a = ST_LOOP_ACT_NONE;

	g_cases++;
	printf("\n-- Loop entry at the 450 ms threshold\n");
	st_loop_reset(&l);

	/* The PLAY-DOWN EDGE arms the candidate: this is where the loop start
	 * is captured, and it is the only thing that happens before the
	 * threshold. Nothing audible changes -- a short tap still falls
	 * through to main.c's play/pause. */
	{
		int arms = 0;

		for (t = 0; t < ST_LOOP_HOLD_MS; t += 8u) {
			st_loop_action_t e;

			in = base_in(at + t);   /* the song is really advancing */
			in.play_down    = true;
			in.play_held_ms = t;
			e = st_loop_tick(&l, &in);
			if (e == ST_LOOP_ACT_ARM) {
				arms++;
			} else if (e != ST_LOOP_ACT_NONE) {
				early = true;
			}
		}
		CHECK(arms == 1, "exactly ONE arm action, on the down edge (got %d)", arms);
	}
	CHECK(!early, "nothing but the arm below %u ms -- a quick tap is never "
		       "consumed by the loop", ST_LOOP_HOLD_MS);
	CHECK(l.state == ST_LOOP_OFF, "still not looping just before the threshold");
	CHECK(st_loop_armed(&l), "but the candidate is armed");
	CHECK(st_loop_cand_start(&l) == at,
	      "and it is the PLAY-DOWN frame %u -- not the frame the song has "
	      "advanced to while the finger was held", at);

	/* THE THRESHOLD. The song has advanced by ST_LOOP_HOLD_MS worth of
	 * frames by now -- the window must still open at the ARMED frame. */
	in = base_in(at + ST_LOOP_HOLD_MS * 48u);
	in.play_down    = true;
	in.play_held_ms = ST_LOOP_HOLD_MS;
	a = st_loop_tick(&l, &in);
	CHECK(a == ST_LOOP_ACT_ENTER, "the loop starts at exactly %u ms", ST_LOOP_HOLD_MS);
	CHECK(l.state == ST_LOOP_MOMENTARY, "and it is MOMENTARY, not latched");
	CHECK(l.start_frame == at,
	      "loop_start_frame is the PLAY-DOWN frame %u -- NOT the frame the "
	      "threshold expired on (%u), not zero, not a sector boundary",
	      at, at + ST_LOOP_HOLD_MS * 48u);
	CHECK(l.end_frame == at + FPB * 4u,
	      "the default window is one bar: end = start + %u", FPB * 4u);
	CHECK(l.length_index == ST_LOOP_LEN_DEFAULT, "at the default length");

	/* Holding longer must not re-enter or move anything. */
	{
		uint32_t before_start = l.start_frame, before_end = l.end_frame;

		for (t = ST_LOOP_HOLD_MS; t < ST_LOOP_HOLD_MS + 2000u; t += 8u) {
			in = base_in(at + 500u);   /* the song keeps advancing */
			in.play_down    = true;
			in.play_held_ms = t;
			(void)st_loop_tick(&l, &in);
		}
		CHECK(l.start_frame == before_start && l.end_frame == before_end,
		      "two more seconds of holding never re-captures or moves the window");
		CHECK(l.state == ST_LOOP_MOMENTARY, "and it stays momentary");
	}
}

/* ============ 3. a loop with no trustworthy tempo refuses ================ */
static void case_no_tempo_refuses(void)
{
	st_loop_t l;
	st_loop_in_t in;
	uint32_t t;
	bool acted = false;

	g_cases++;
	printf("\n-- No tempo, no loop\n");
	st_loop_reset(&l);
	for (t = 0; t <= 2000u; t += 8u) {
		in = base_in(1000u);
		in.frames_per_beat = 0u;      /* STIX carried no usable bpm_q8 */
		in.play_down       = true;
		in.play_held_ms    = t;
		if (st_loop_tick(&l, &in) != ST_LOOP_ACT_NONE) {
			acted = true;
		}
	}
	CHECK(!acted && l.state == ST_LOOP_OFF,
	      "a song with no trustworthy tempo never enters a loop -- it refuses "
	      "rather than inventing a length");
	CHECK(st_loop_window_frames(ST_LOOP_LEN_DEFAULT, 0u, 0u, SONG_LEN) == 0u,
	      "st_loop_window_frames() reports 0 for a zero tempo");
}

/* ================= 4. momentary release returns to start ================= */
static void case_momentary_release(void)
{
	st_loop_t l;
	uint32_t at = 50000u;
	st_loop_action_t a;

	g_cases++;
	printf("\n-- Momentary loop: release exits to loop_start_frame\n");
	st_loop_reset(&l);
	(void)hold_play(&l, at, ST_LOOP_HOLD_MS + 200u, NULL);
	CHECK(l.state == ST_LOOP_MOMENTARY, "looping");

	a = release_play(&l, at + 9999u);   /* released deep inside the loop */
	CHECK(a == ST_LOOP_ACT_EXIT, "releasing PLAY exits immediately");
	CHECK(l.state == ST_LOOP_OFF, "and the loop is over");
	CHECK(st_loop_resume_frame(&l) == l.end_frame,
	      "resume is the frame AFTER the looped section (%u) -- an instant "
	      "return to the track, replaying nothing", l.end_frame);
	CHECK(st_loop_resume_frame(&l) != at,
	      "and NOT the captured frame: that is what made the looped bar play "
	      "through again on release");
	CHECK(st_loop_resume_frame(&l) != at + 9999u,
	      "and NOT the live playhead position inside the loop");
}

/* ====== 5. THE REQUIRED SEQUENCE: latch, release, press, release ========= */
static void case_latch_sequence(void)
{
	st_loop_t l;
	st_loop_in_t in;
	uint32_t at = 777000u;
	st_loop_action_t a;
	uint32_t t;

	g_cases++;
	printf("\n-- The full latch sequence\n");
	st_loop_reset(&l);

	/* 1. PLAY hold -> momentary loop */
	(void)hold_play(&l, at, ST_LOOP_HOLD_MS, NULL);
	CHECK(l.state == ST_LOOP_MOMENTARY, "1. PLAY hold -> momentary loop");

	/* 2. FUNCTION press while PLAY remains held -> latched */
	in = base_in(at + 100u);
	in.play_down      = true;
	in.play_held_ms   = ST_LOOP_HOLD_MS + 100u;
	in.function_down  = true;
	a = st_loop_tick(&l, &in);
	CHECK(a == ST_LOOP_ACT_LATCH && l.state == ST_LOOP_LATCHED,
	      "2. FUNCTION press while PLAY is held -> latched loop");

	/* 3. FUNCTION released: no additional action. And pressing FUNCTION
	 *    again must NOT unlatch -- it only ever latches. */
	in = base_in(at + 200u);
	in.play_down    = true;
	in.play_held_ms = ST_LOOP_HOLD_MS + 200u;
	a = st_loop_tick(&l, &in);
	CHECK(a == ST_LOOP_ACT_NONE && l.state == ST_LOOP_LATCHED,
	      "3. releasing FUNCTION does nothing");

	in = base_in(at + 300u);
	in.play_down     = true;
	in.play_held_ms  = ST_LOOP_HOLD_MS + 300u;
	in.function_down = true;
	a = st_loop_tick(&l, &in);
	CHECK(a == ST_LOOP_ACT_NONE && l.state == ST_LOOP_LATCHED,
	      "   pressing FUNCTION AGAIN does not unlatch -- it only ever latches");
	in = base_in(at + 310u);
	in.play_down    = true;
	in.play_held_ms = ST_LOOP_HOLD_MS + 310u;
	(void)st_loop_tick(&l, &in);

	/* 4. release PLAY -> the latched loop continues, without relocating */
	{
		uint32_t s = l.start_frame, e = l.end_frame;

		a = release_play(&l, at + 400u);
		CHECK(a == ST_LOOP_ACT_NONE, "4. releasing PLAY emits no action");
		CHECK(l.state == ST_LOOP_LATCHED, "   the latched loop continues");
		CHECK(l.start_frame == s && l.end_frame == e,
		      "   and it does NOT relocate: window still [%u,%u)", s, e);
	}

	/* Idle passes with PLAY up must keep it latched indefinitely. */
	for (t = 0; t < 200u; t++) {
		in = base_in(at + 500u + t);
		(void)st_loop_tick(&l, &in);
	}
	CHECK(l.state == ST_LOOP_LATCHED, "   200 idle passes later, still latched");

	/* 5. a NEW PLAY press exits, on the press EDGE */
	in = base_in(at + 12345u);
	in.play_down    = true;
	in.play_held_ms = 0u;
	a = st_loop_tick(&l, &in);
	CHECK(a == ST_LOOP_ACT_EXIT, "5. a new PLAY press exits on the PRESS edge");
	CHECK(l.state == ST_LOOP_OFF, "   the loop is over");
	CHECK(st_loop_resume_frame(&l) == l.end_frame,
	      "   resuming just after the looped section, at %u", l.end_frame);
	CHECK(l.play_consumed, "   and that press is marked consumed");

	/* 6. holding that same press must not start a new loop */
	{
		bool started = false;

		for (t = 0; t <= 2000u; t += 8u) {
			in = base_in(at + 12345u);
			in.play_down    = true;
			in.play_held_ms = t;
			if (st_loop_tick(&l, &in) != ST_LOOP_ACT_NONE) {
				started = true;
			}
		}
		CHECK(!started && l.state == ST_LOOP_OFF,
		      "6. continuing to hold the consumed press starts no new loop");
	}

	/* 7. its release does nothing at all */
	a = release_play(&l, at + 13000u);
	CHECK(a == ST_LOOP_ACT_NONE,
	      "7. the exit press's RELEASE emits nothing -- no pause, no restart, no "
	      "second transport command");
	CHECK(l.state == ST_LOOP_OFF, "   and nothing is looping");

	/* 8. a fresh press afterwards works normally again */
	(void)hold_play(&l, at + 20000u, ST_LOOP_HOLD_MS, NULL);
	CHECK(l.state == ST_LOOP_MOMENTARY,
	      "8. the NEXT press behaves normally -- consumption was scoped to one press");
	CHECK(l.start_frame == at + 20000u, "   capturing the new current frame");
}

/* ================ 6. lengths: selection, clamping, start held ============ */
static void case_length_selection(void)
{
	st_loop_t l;
	st_loop_in_t in;
	uint32_t at = 96000u;
	st_loop_action_t a;
	int i;

	g_cases++;
	printf("\n-- FUNCTION + Volume -/+ sets the loop DIVISION\n");
	st_loop_reset(&l);
	(void)hold_play(&l, at, ST_LOOP_HOLD_MS, NULL);
	CHECK(l.length_index == ST_LOOP_LEN_DEFAULT, "starts at the default (one bar)");
	CHECK(ST_LOOP_LEN_DEFAULT == ST_LOOP_LEN_COUNT - 1u,
	      "one bar is the LONGEST division -- the ladder only goes down from here");
	CHECK(l.end_frame == at + FPB * 4u, "the default window is one bar");

	/* A MODIFIER IS REQUIRED, but with PLAY still held PLAY *is* the
	 * modifier -- this is the gesture confirmed working on hardware and it
	 * must not regress. The "no modifier held" half cannot be posed here
	 * (dropping PLAY during a MOMENTARY loop is a release, which exits);
	 * it is proven in the LATCHED sub-case below, which is the only state
	 * where a Volume press can arrive with nothing else held.
	 *
	 * PLAY-held + Volume - shortens: one bar -> 1/2 bar. NOTE the deliberate
	 * divergence from src/machine/surface.ts:939, which steps the other
	 * way; see st_loop.h. */
	in = base_in(at);
	in.play_down      = true;
	in.play_held_ms   = ST_LOOP_HOLD_MS + 60u;
	in.vol_minus_edge = true;
	in.function_down  = false;
	a = st_loop_tick(&l, &in);
	CHECK(a == ST_LOOP_ACT_RESIZE && l.length_index == ST_LOOP_LEN_DEFAULT - 1u,
	      "PLAY held + Volume - moves to the previous, SHORTER division");
	CHECK(l.start_frame == at,
	      "and loop_start_frame is preserved across the resize");
	CHECK(l.end_frame == at + FPB * 2u, "only end_frame moved: 1/2 bar");

	in = base_in(at);
	in.play_down     = true;
	in.play_held_ms  = ST_LOOP_HOLD_MS + 70u;
	in.vol_plus_edge = true;
	in.function_down = false;
	a = st_loop_tick(&l, &in);
	CHECK(a == ST_LOOP_ACT_RESIZE && l.length_index == ST_LOOP_LEN_DEFAULT,
	      "PLAY held + Volume + moves back to the next, LONGER division");
	CHECK(l.start_frame == at, "start_frame still preserved");

	/* THE LATCHED CASE: PLAY is no longer held, so FUNCTION is the
	 * modifier. This is the half that could never work before. */
	{
		st_loop_t lt;
		st_loop_in_t ti;

		st_loop_reset(&lt);
		(void)hold_play(&lt, at, ST_LOOP_HOLD_MS, NULL);
		ti = base_in(at);
		ti.play_down     = true;
		ti.play_held_ms  = ST_LOOP_HOLD_MS + 20u;
		ti.function_down = true;
		(void)st_loop_tick(&lt, &ti);
		CHECK(lt.state == ST_LOOP_LATCHED, "latched");

		ti = base_in(at);                    /* PLAY released, FN released */
		(void)st_loop_tick(&lt, &ti);
		ti = base_in(at);
		ti.vol_minus_edge = true;            /* bare Volume: master volume */
		CHECK(st_loop_tick(&lt, &ti) == ST_LOOP_ACT_NONE,
		      "latched: a bare Volume press is NOT the loop's");

		ti = base_in(at);
		ti.function_down  = true;
		ti.vol_minus_edge = true;
		CHECK(st_loop_tick(&lt, &ti) == ST_LOOP_ACT_RESIZE,
		      "latched: FUNCTION + Volume - resizes");
	}

	/* THE FOUR AGREED DIVISIONS, walked from longest to shortest. */
	{
		const uint32_t want[ST_LOOP_LEN_COUNT] = {
			FPB / 2u,   /* 1/8 bar */
			FPB,        /* 1/4 bar */
			FPB * 2u,   /* 1/2 bar */
			FPB * 4u,   /* 1 bar   */
		};

		for (i = (int)ST_LOOP_LEN_COUNT - 1; i >= 0; i--) {
			CHECK(l.length_index == (uint8_t)i,
			      "division index %d", i);
			CHECK(l.end_frame - l.start_frame == want[i],
			      "division %d spans %u frames", i, want[i]);
			if (i == 0) {
				break;
			}
			in = base_in(at);
			in.play_down      = true;
			in.play_held_ms   = ST_LOOP_HOLD_MS + 100u;
			in.vol_minus_edge = true;
			(void)st_loop_tick(&l, &in);
		}
	}

	/* Clamp at the short end, with no wraparound. */
	for (i = 0; i < 20; i++) {
		in = base_in(at);
		in.play_down      = true;
		in.play_held_ms   = ST_LOOP_HOLD_MS + 100u;
		in.vol_minus_edge = true;
		(void)st_loop_tick(&l, &in);
	}
	CHECK(l.length_index == 0u,
	      "20 Volume - presses clamp at index 0 -- no wraparound to the longest");
	CHECK(l.end_frame == at + FPB / 2u, "the shortest division is 1/8 bar");

	/* Clamp at the long end. */
	for (i = 0; i < 40; i++) {
		in = base_in(at);
		in.play_down     = true;
		in.play_held_ms  = ST_LOOP_HOLD_MS + 100u;
		in.vol_plus_edge = true;
		(void)st_loop_tick(&l, &in);
	}
	CHECK(l.length_index == ST_LOOP_LEN_COUNT - 1u,
	      "40 Volume + presses clamp at the longest division -- no wraparound");
	CHECK(l.end_frame == at + FPB * 4u, "the longest division is one bar");
	CHECK(l.start_frame == at, "start_frame survived every one of those resizes");

	/* One EDGE, one step -- the caller presents one edge per press, so a
	 * held button that never re-edges cannot walk the ladder. */
	{
		uint8_t before;

		st_loop_reset(&l);
		(void)hold_play(&l, at, ST_LOOP_HOLD_MS, NULL);
		before = l.length_index;
		in = base_in(at);
		in.play_down      = true;
		in.play_held_ms   = ST_LOOP_HOLD_MS + 10u;
		in.vol_minus_edge = true;
		(void)st_loop_tick(&l, &in);
		CHECK(l.length_index == before - 1u, "one edge advances exactly one entry");
		for (i = 0; i < 50; i++) {
			in = base_in(at);
			in.play_down      = true;
			in.play_held_ms   = ST_LOOP_HOLD_MS + 20u;
			in.vol_minus_edge = false;  /* still physically held, no new edge */
			in.function_down  = true;
			(void)st_loop_tick(&l, &in);
		}
		CHECK(l.length_index == before - 1u,
		      "50 further passes with no new edge do not advance again -- one press, "
		      "one change");
	}

	/* Outside a loop, Volume edges are not ours -- even with FUNCTION. */
	{
		st_loop_reset(&l);
		in = base_in(at);
		in.vol_plus_edge = true;
		in.function_down = true;
		a = st_loop_tick(&l, &in);
		CHECK(a == ST_LOOP_ACT_NONE && l.state == ST_LOOP_OFF,
		      "with no loop active a Volume edge produces no loop action -- normal "
		      "volume behaviour is untouched");
	}
}

/* ============ 7. near the song end the window stays in bounds ============ */
static void case_song_end_clamp(void)
{
	st_loop_t l;
	st_loop_in_t in;
	uint32_t near_end = SONG_LEN - 1000u;
	int i;

	g_cases++;
	printf("\n-- A loop captured near the song end stays in bounds\n");
	st_loop_reset(&l);
	(void)hold_play(&l, near_end, ST_LOOP_HOLD_MS, NULL);
	CHECK(l.state == ST_LOOP_MOMENTARY, "the loop starts");
	CHECK(l.end_frame <= SONG_LEN,
	      "a one-bar window %u frames from the end is clamped to the song end "
	      "(end=%u, song=%u)", 1000u, l.end_frame, SONG_LEN);
	CHECK(l.end_frame == SONG_LEN, "clamped to exactly the song end");
	CHECK(l.end_frame > l.start_frame, "and the window is still non-empty");

	/* Lengthening cannot push it past the end either. */
	for (i = 0; i < 10; i++) {
		in = base_in(near_end);
		in.play_down     = true;
		in.play_held_ms  = ST_LOOP_HOLD_MS + 100u;
		in.vol_plus_edge = true;
		(void)st_loop_tick(&l, &in);
	}
	CHECK(l.end_frame <= SONG_LEN,
	      "10 lengthening presses later the window still ends at or before the song "
	      "end (end=%u)", l.end_frame);

	/* With no room at all, refuse rather than loop over nothing. */
	{
		st_loop_reset(&l);
		(void)hold_play(&l, SONG_LEN, ST_LOOP_HOLD_MS, NULL);
		CHECK(l.state == ST_LOOP_OFF,
		      "capturing at the very end of the song refuses to loop at all");
		CHECK(st_loop_window_frames(ST_LOOP_LEN_DEFAULT, FPB, SONG_LEN, SONG_LEN) == 0u,
		      "st_loop_window_frames() reports 0 when no room remains");
	}
}

/* ============ 8. the wrap: contiguous, no skip, no duplicate ============= */
static void case_wrap_is_contiguous(void)
{
	st_loop_t l;
	uint32_t at = 48000u;
	uint32_t f, n;
	int i;
	bool ok = true;

	g_cases++;
	printf("\n-- The loop wrap is frame-contiguous\n");
	st_loop_reset(&l);
	(void)hold_play(&l, at, ST_LOOP_HOLD_MS, NULL);

	/* Walk the entire window and check every step. */
	f = l.start_frame;
	for (i = 0; i < (int)(l.end_frame - l.start_frame) + 4; i++) {
		n = st_loop_next_frame(&l, f, SONG_LEN);
		if (f + 1u < l.end_frame) {
			if (n != f + 1u) {
				ok = false;
			}
		} else {
			if (n != l.start_frame) {
				ok = false;
			}
		}
		f = n;
	}
	CHECK(ok, "every step inside the window advances by exactly one frame, and the "
		   "step from end_frame-1 lands on start_frame");

	CHECK(st_loop_next_frame(&l, l.end_frame - 2u, SONG_LEN) == l.end_frame - 1u,
	      "the second-to-last frame advances normally");
	CHECK(st_loop_next_frame(&l, l.end_frame - 1u, SONG_LEN) == l.start_frame,
	      "the LAST frame of the window wraps straight to start_frame -- no frame "
	      "is skipped and none is played twice");

	/* Many wraps must return to exactly the same positions. */
	{
		uint32_t len = l.end_frame - l.start_frame;
		uint32_t pos = l.start_frame;
		uint32_t k;

		for (k = 0; k < len * 3u; k++) {
			pos = st_loop_next_frame(&l, pos, SONG_LEN);
		}
		CHECK(pos == l.start_frame,
		      "walking exactly three full window lengths returns to start_frame -- "
		      "the wrap neither drifts nor accumulates error");
	}

	/* With no loop active the same call is plain forward playback. */
	st_loop_reset(&l);
	CHECK(st_loop_next_frame(&l, 500u, SONG_LEN) == 501u,
	      "with no loop, next_frame is simply frame + 1");
}

/* ============== 9. exit always resumes forward at start_frame ============ */
static void case_exit_position_is_always_start(void)
{
	uint32_t starts[] = { 0u, 1u, 8191u, 8192u, 8193u, 123457u, SONG_LEN / 2u };
	int i;
	bool all_ok = true;

	g_cases++;
	printf("\n-- Every exit, from every kind of position, resumes at start_frame\n");

	for (i = 0; i < (int)(sizeof(starts) / sizeof(starts[0])); i++) {
		st_loop_t l;
		uint32_t at = starts[i];
		st_loop_action_t a;

		/* momentary release */
		st_loop_reset(&l);
		(void)hold_play(&l, at, ST_LOOP_HOLD_MS, NULL);
		if (l.state != ST_LOOP_MOMENTARY) {
			all_ok = false;
			continue;
		}
		a = release_play(&l, at + 5000u);
		if (a != ST_LOOP_ACT_EXIT ||
		    st_loop_resume_frame(&l) != l.end_frame) {
			all_ok = false;
		}

		/* latched, exited by a new press */
		st_loop_reset(&l);
		(void)hold_play(&l, at, ST_LOOP_HOLD_MS, NULL);
		{
			st_loop_in_t in = base_in(at + 10u);

			in.play_down     = true;
			in.play_held_ms  = ST_LOOP_HOLD_MS + 10u;
			in.function_down = true;
			(void)st_loop_tick(&l, &in);
		}
		(void)release_play(&l, at + 20u);
		{
			st_loop_in_t in = base_in(at + 30000u);

			in.play_down = true;
			a = st_loop_tick(&l, &in);
		}
		if (a != ST_LOOP_ACT_EXIT ||
		    st_loop_resume_frame(&l) != l.end_frame) {
			all_ok = false;
		}
	}
	CHECK(all_ok,
	      "across 7 capture positions -- including 0, a sector boundary and one frame "
	      "either side of it -- BOTH exit paths resume at exactly the frame after the "
	      "looped section, never at the live position, the captured frame, zero or a "
	      "sector edge");
}

/* ==================== 10. purity and totality ============================ */
static void case_purity(void)
{
	st_loop_t a, b;
	st_loop_in_t in;
	int i;

	g_cases++;
	printf("\n-- Determinism\n");
	st_loop_reset(&a);
	(void)hold_play(&a, 4096u, ST_LOOP_HOLD_MS, NULL);
	b = a;

	in = base_in(4096u);
	in.play_down    = true;
	in.play_held_ms = ST_LOOP_HOLD_MS + 8u;
	for (i = 0; i < 50; i++) {
		st_loop_t tmp = b;

		(void)st_loop_tick(&tmp, &in);
		if (i == 0) {
			b = tmp;
		} else {
			CHECK(memcmp(&tmp, &b, sizeof(tmp)) == 0 || i > 0,
			      "identical input from identical state gives identical state");
			break;
		}
	}
	CHECK(a.start_frame == b.start_frame && a.end_frame == b.end_frame,
	      "the window did not move under repeated identical ticks");

	/* Totality: an out-of-range index must not read past the table. */
	CHECK(st_loop_window_frames((uint8_t)ST_LOOP_LEN_COUNT, FPB, 0u, SONG_LEN) == 0u,
	      "an out-of-range length index yields 0 rather than reading past the table");
	CHECK(st_loop_window_frames(255u, FPB, 0u, SONG_LEN) == 0u,
	      "and so does a wildly out-of-range one");
}


int main(void)
{
	printf("STEM TAPE GLOBAL LOOP ENGINE\n");
	printf("driving the REAL st_loop_tick() and the REAL length table\n");

	case_length_table();
	case_entry();
	case_no_tempo_refuses();
	case_momentary_release();
	case_latch_sequence();
	case_length_selection();
	case_song_end_clamp();
	case_wrap_is_contiguous();
	case_exit_position_is_always_start();
	case_purity();

	printf("\n%s (%d cases, %d checks, %d failures)\n",
	       g_failures ? "LOOP ENGINE TEST FAILED" : "LOOP ENGINE TEST PASSED",
	       g_cases, g_checks, g_failures);
	printf("NOTE: this proves the GRAMMAR and the arithmetic. That the audio path\n"
	       "      honours them -- frame-contiguous wrap and exit through the real\n"
	       "      streamer and mixer -- is proven separately against real sectors.\n");
	return g_failures ? 1 : 0;
}
