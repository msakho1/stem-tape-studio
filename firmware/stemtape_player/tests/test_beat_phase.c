/*
 * test_beat_phase.c — st_beat_phase.c: pure, RAM-only beat-phase
 * computation for stem playback LED feedback, host-tested.
 *
 * All song_frame/bpm_q8/downbeat_frame values below are fabricated
 * TIMING inputs (integers), not fabricated AUDIO content -- st_beat_
 * phase.h has no audio concept at all, so this is the standard way to
 * test a pure timing/arithmetic state machine, not the fabrication this
 * suite's own non-fabrication rule forbids.
 *
 * SCOPE: timing only. This module no longer exports any LED decision --
 * st_beat_phase_on_beat()/st_beat_led_decide() and their on/off/ghost
 * vocabulary were deleted when the eight LEDs got a single semantic
 * owner (see st_beat_phase.h's own doc comment for why). The display
 * decisions those two functions used to make are covered by
 * tests/test_st_led_mvp.c against the real st_led_mvp_decide(), and the
 * mute/solo audibility rule they consumed is covered by
 * tests/test_stem_mix.c against the real st_stem_mix_channel_audible().
 * The downbeat-anchoring, pre-downbeat and loop-wrap cases below were
 * NOT dropped with those functions: they are re-expressed against
 * st_beat_pulse(), which is what the firmware actually calls, so the
 * coverage now follows the live code path instead of a retired one.
 *
 *     cc -std=c11 -Wall -Wextra -I../src \
 *        ../src/st_beat_phase.c test_beat_phase.c -o test_beat_phase -lm \
 *        && ./test_beat_phase
 *
 * Does not need the repository-root working directory (no fixture
 * files).
 */

#include <stdio.h>

#include "st_beat_phase.h"

static int g_checks;
static int g_failures;
static int g_test_cases;

#define CHECK(cond, ...) do { \
		g_checks++; \
		if (cond) { \
			printf("[OK  ] " __VA_ARGS__); \
			printf("\n"); \
		} else { \
			g_failures++; \
			printf("[FAIL] " __VA_ARGS__); \
			printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)
#define RUN(fn) do { g_test_cases++; fn(); } while (0)

#define SR_48K 48000u

/* ---- BPM: frames_per_beat is computed correctly (and rounded, not
 * truncated) from bpm_q8/sample_rate. ---- */
static void test_bpm_120_gives_exact_frames_per_beat(void)
{
	st_beat_timing_t t;

	/* 120.0 BPM at 48000 Hz: 48000 * 60 / 120 = 24000 frames/beat exactly. */
	bool ok = st_beat_timing_init(&t, 120u * 256u, 0u, SR_48K);

	CHECK(ok, "bpm 120: init succeeds");
	CHECK(t.frames_per_beat == 24000u, "bpm 120: frames_per_beat == 24000 exactly (got %u)",
	      t.frames_per_beat);
}

static void test_bpm_non_integer_divisor_rounds_not_truncates(void)
{
	st_beat_timing_t t;

	/* 100.5 BPM: 48000*60*256/(100*256+128) = 737280000/25728 = 28656.71...
	 * -- must round to 28657, not truncate to 28656. */
	bool ok = st_beat_timing_init(&t, 100u * 256u + 128u, 0u, SR_48K);

	CHECK(ok, "bpm 100.5: init succeeds");
	CHECK(t.frames_per_beat == 28657u, "bpm 100.5: frames_per_beat rounds to nearest (28657), not "
					    "truncates (28656) -- got %u",
	      t.frames_per_beat);
}

static void test_bpm_zero_is_invalid(void)
{
	st_beat_timing_t t;

	bool ok = st_beat_timing_init(&t, 0u, 0u, SR_48K);

	CHECK(!ok, "bpm 0 (absent/unknown): init reports invalid");
	CHECK(t.frames_per_beat == 0u, "bpm 0: frames_per_beat left at 0 -- caller's steady display "
					"fallback, never a fabricated tempo");
}

static void test_sample_rate_zero_is_invalid(void)
{
	st_beat_timing_t t;

	bool ok = st_beat_timing_init(&t, 120u * 256u, 0u, 0u);

	CHECK(!ok, "sample_rate 0 (invalid): init reports invalid even with a real bpm_q8");
	CHECK(t.frames_per_beat == 0u, "sample_rate 0: frames_per_beat left at 0");
}

/* At 120 BPM / 48 kHz the beat is 24000 frames and the pulse window is
 * ST_BEAT_PULSE_NUM/ST_BEAT_PULSE_DEN of it. Derived here from the header's
 * own constants rather than hardcoded, so retuning the window retunes these
 * expectations instead of breaking them. */
#define FPB_120  24000u
#define WIN_120  ((FPB_120 * ST_BEAT_PULSE_NUM) / ST_BEAT_PULSE_DEN)

static st_beat_pulse_t pulse_at(const st_beat_timing_t *t, uint32_t song_frame)
{
	st_beat_pulse_t p;

	st_beat_pulse(t, song_frame, &p);
	return p;
}

static void test_invalid_timing_never_reports_a_pulse(void)
{
	st_beat_timing_t t;
	st_beat_pulse_t p;

	(void)st_beat_timing_init(&t, 0u, 0u, SR_48K); /* invalid */
	p = pulse_at(&t, 0u);

	CHECK(!p.valid, "invalid timing: pulse reports valid=false -- never fabricates a tempo");
	CHECK(!p.in_pulse && p.envelope == 0u && p.beat_index == 0u,
	      "invalid timing: every output field is the fail-closed value (dark, no bar position), "
	      "not a stale or invented one");
}

/* ---- DOWNBEAT OFFSET: phase is anchored to downbeat_frame, not frame 0. ---- */
static void test_downbeat_offset_anchors_phase(void)
{
	st_beat_timing_t t;

	(void)st_beat_timing_init(&t, 120u * 256u, 1000u, SR_48K); /* frames_per_beat=24000, downbeat=1000 */
	CHECK(t.frames_per_beat == FPB_120, "downbeat offset: setup check, frames_per_beat == %u", FPB_120);

	CHECK(pulse_at(&t, 1000u).in_pulse, "downbeat offset: song_frame exactly AT downbeat_frame is inside "
					     "the pulse window (phase 0)");
	CHECK(pulse_at(&t, 1000u).beat_index == 0u, "downbeat offset: the frame at downbeat_frame is beat 0 "
						     "(the downbeat), not an arbitrary bar position");
	CHECK(pulse_at(&t, 1000u + WIN_120 - 1u).in_pulse, "downbeat offset: the last frame of the window is "
							    "still inside the pulse");
	CHECK(!pulse_at(&t, 1000u + WIN_120).in_pulse, "downbeat offset: one frame past the window is dark -- "
							"the window is half-open, exactly WIN frames wide");
	CHECK(pulse_at(&t, 1000u + WIN_120).valid, "downbeat offset: between pulses is still VALID timing -- "
						    "dark is an answer, not an absence of one");
	CHECK(pulse_at(&t, 1000u + FPB_120).in_pulse, "downbeat offset: exactly one full beat later, the pulse "
						       "returns");
	CHECK(pulse_at(&t, 1000u + FPB_120).beat_index == 1u, "downbeat offset: one beat past the downbeat is "
							       "beat 1 -- the bar position advances with the beat");
}

/* ---- FRAMES BEFORE DOWNBEAT: no pulse yet -- dark, not a fabricated
 * pre-roll pattern. ---- */
static void test_frames_before_downbeat_never_pulse(void)
{
	st_beat_timing_t t;

	(void)st_beat_timing_init(&t, 120u * 256u, 5000u, SR_48K);

	CHECK(!pulse_at(&t, 0u).valid, "before downbeat: song_frame 0, downbeat_frame 5000 -- invalid, so no "
					"pulse and no bar position");
	CHECK(!pulse_at(&t, 4999u).valid, "before downbeat: one frame before downbeat_frame -- still nothing");
	CHECK(pulse_at(&t, 5000u).valid && pulse_at(&t, 5000u).in_pulse,
	      "before downbeat: exactly AT downbeat_frame -- the pulse begins here, not one frame later or "
	      "earlier");
}

/* ---- LOOP BOUNDARIES: song_frame wraps to 0 (st_stream_t's own LOOPED
 * tick) -- phase must re-derive correctly from the new, wrapped position,
 * with no separate clock to desynchronize. ---- */
static void test_loop_wrap_resumes_correctly(void)
{
	st_beat_timing_t t;

	(void)st_beat_timing_init(&t, 120u * 256u, 1000u, SR_48K); /* frames_per_beat=24000, downbeat=1000 */

	/* Just before a loop wrap: the fourth beat of a bar, pulsing. */
	CHECK(pulse_at(&t, 1000u + FPB_120 * 3u).in_pulse, "loop boundary: pulsing late in the song, just "
							    "before a hypothetical wrap");
	CHECK(pulse_at(&t, 1000u + FPB_120 * 3u).beat_index == 3u, "loop boundary: that late beat is bar "
								    "position 3, the last of the four");

	/* The instant song_frame wraps to 0 (st_stream_t resets it there):
	 * 0 < downbeat_frame (1000), so this is the "before downbeat" case
	 * again, briefly -- dark, not a fabricated pattern. */
	CHECK(!pulse_at(&t, 0u).valid, "loop boundary: the instant of wrap (song_frame 0) is before this "
					"song's own downbeat_frame -- no pulse yet, same fallback as any "
					"other pre-downbeat moment");

	/* Once song_frame reaches downbeat_frame again post-wrap, pulsing
	 * resumes exactly as it did the first time -- same formula, same
	 * clock, no drift introduced by the wrap, and the bar restarts at 0
	 * rather than continuing from wherever it was. */
	CHECK(pulse_at(&t, 1000u).in_pulse && pulse_at(&t, 1000u).beat_index == 0u,
	      "loop boundary: post-wrap, once song_frame reaches downbeat_frame again, the pulse resumes "
	      "identically AND the bar restarts at beat 0");
}

/* ---- ENVELOPE: the window is a symmetric rise and fall, not a square
 * gate -- this is what makes the pulse read as a pulse. ---- */
static void test_envelope_rises_and_falls_within_the_window(void)
{
	st_beat_timing_t t;
	uint32_t mid;

	(void)st_beat_timing_init(&t, 120u * 256u, 0u, SR_48K);
	mid = WIN_120 / 2u;

	CHECK(pulse_at(&t, 0u).envelope == 0u, "envelope: the first frame of the window is 0 -- the pulse "
					        "rises INTO the beat rather than snapping on");
	CHECK(pulse_at(&t, mid).envelope == 255u, "envelope: the window's midpoint is full scale (got %u)",
	      pulse_at(&t, mid).envelope);
	CHECK(pulse_at(&t, mid / 2u).envelope < 255u && pulse_at(&t, mid / 2u).envelope > 0u,
	      "envelope: a quarter of the way in is partway up, neither dark nor full");
	CHECK(pulse_at(&t, mid + (mid / 2u)).envelope < 255u && pulse_at(&t, mid + (mid / 2u)).envelope > 0u,
	      "envelope: three quarters of the way in is partway back DOWN -- symmetric, not a sawtooth");
	CHECK(pulse_at(&t, WIN_120).envelope == 0u, "envelope: past the window it is 0, matching in_pulse");
}

/* ---- BAR POSITION CYCLES 0..3 and only 0..3, which is what makes the
 * 1->2->3->4 chase possible at all. ---- */
static void test_beat_index_cycles_through_the_bar(void)
{
	st_beat_timing_t t;
	bool all_in_range = true;
	uint32_t b;

	(void)st_beat_timing_init(&t, 120u * 256u, 0u, SR_48K);

	CHECK(pulse_at(&t, FPB_120 * 0u).beat_index == 0u, "bar: beat 0");
	CHECK(pulse_at(&t, FPB_120 * 1u).beat_index == 1u, "bar: beat 1");
	CHECK(pulse_at(&t, FPB_120 * 2u).beat_index == 2u, "bar: beat 2");
	CHECK(pulse_at(&t, FPB_120 * 3u).beat_index == 3u, "bar: beat 3");
	CHECK(pulse_at(&t, FPB_120 * 4u).beat_index == 0u, "bar: the fifth beat wraps back to 0 -- a four-beat "
							    "bar, not a free-running counter");

	for (b = 0u; b < 64u; b++) {
		if (pulse_at(&t, FPB_120 * b).beat_index > 3u) {
			all_in_range = false;
		}
	}
	CHECK(all_in_range, "bar: across 64 consecutive beats every beat_index stays within 0..3 -- the chase "
			     "can never index past the four Track LEDs");
}

int main(void)
{
	RUN(test_bpm_120_gives_exact_frames_per_beat);
	RUN(test_bpm_non_integer_divisor_rounds_not_truncates);
	RUN(test_bpm_zero_is_invalid);
	RUN(test_sample_rate_zero_is_invalid);
	RUN(test_invalid_timing_never_reports_a_pulse);
	RUN(test_downbeat_offset_anchors_phase);
	RUN(test_frames_before_downbeat_never_pulse);
	RUN(test_loop_wrap_resumes_correctly);
	RUN(test_envelope_rises_and_falls_within_the_window);
	RUN(test_beat_index_cycles_through_the_bar);

	printf("\n%d distinct test cases, %d assertion checks\n", g_test_cases, g_checks);
	if (g_failures) {
		printf("BEAT PHASE TEST FAILED (%d/%d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("BEAT PHASE TEST PASSED (%d test cases, %d checks)\n", g_test_cases, g_checks);
	return 0;
}
