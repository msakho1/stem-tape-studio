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
 *     cc -std=c11 -Wall -Wextra -I../src \
 *        ../src/st_beat_phase.c ../src/st_stem_mix.c ../src/st_sector_v11.c \
 *        test_beat_phase.c -o test_beat_phase -lm && ./test_beat_phase
 *
 * Does not need the repository-root working directory (no fixture
 * files).
 */

#include <stdio.h>

#include "st_beat_phase.h"
#include "st_stem_mix.h"

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

static void test_invalid_timing_never_reports_on_beat(void)
{
	st_beat_timing_t t;

	(void)st_beat_timing_init(&t, 0u, 0u, SR_48K); /* invalid */

	CHECK(!st_beat_phase_on_beat(&t, 0u, 100000u), "invalid timing: on_beat is always false, even with "
							 "a huge window and song_frame 0 -- never fabricates a beat");
}

/* ---- DOWNBEAT OFFSET: phase is anchored to downbeat_frame, not frame 0. ---- */
static void test_downbeat_offset_anchors_phase(void)
{
	st_beat_timing_t t;

	(void)st_beat_timing_init(&t, 120u * 256u, 1000u, SR_48K); /* frames_per_beat=24000, downbeat=1000 */

	CHECK(st_beat_phase_on_beat(&t, 1000u, 500u), "downbeat offset: song_frame exactly AT downbeat_frame "
						       "is on-beat (phase 0)");
	CHECK(st_beat_phase_on_beat(&t, 1499u, 500u), "downbeat offset: song_frame 499 frames past downbeat, "
						       "window 500 -- still on-beat");
	CHECK(!st_beat_phase_on_beat(&t, 1500u, 500u), "downbeat offset: song_frame 500 frames past downbeat, "
							"window 500 -- just off-beat");
	CHECK(st_beat_phase_on_beat(&t, 1000u + 24000u, 500u), "downbeat offset: exactly one full beat later "
								 "(frame 25000) is on-beat again");
}

/* ---- FRAMES BEFORE DOWNBEAT: no pulse yet -- caller's steady display,
 * not a fabricated pre-roll pattern. ---- */
static void test_frames_before_downbeat_never_on_beat(void)
{
	st_beat_timing_t t;

	(void)st_beat_timing_init(&t, 120u * 256u, 5000u, SR_48K);

	CHECK(!st_beat_phase_on_beat(&t, 0u, 24000u), "before downbeat: song_frame 0, downbeat_frame 5000 -- "
						       "never on-beat even with a window covering the whole beat");
	CHECK(!st_beat_phase_on_beat(&t, 4999u, 24000u), "before downbeat: one frame before downbeat_frame -- "
							  "still never on-beat");
	CHECK(st_beat_phase_on_beat(&t, 5000u, 500u), "before downbeat: exactly AT downbeat_frame -- on-beat "
						       "begins here, not one frame later or earlier");
}

/* ---- LOOP BOUNDARIES: song_frame wraps to 0 (st_stream_t's own LOOPED
 * tick) -- phase must re-derive correctly from the new, wrapped position,
 * with no separate clock to desynchronize. ---- */
static void test_loop_wrap_resumes_correctly(void)
{
	st_beat_timing_t t;

	(void)st_beat_timing_init(&t, 120u * 256u, 1000u, SR_48K); /* frames_per_beat=24000, downbeat=1000 */

	/* Just before a loop wrap: deep into a late beat, on-beat. */
	CHECK(st_beat_phase_on_beat(&t, 1000u + 24000u * 3u, 500u), "loop boundary: on-beat late in the song, "
								      "just before a hypothetical wrap");

	/* The instant song_frame wraps to 0 (st_stream_t resets it there):
	 * 0 < downbeat_frame (1000), so this is the "before downbeat" case
	 * again, briefly -- steady display, not a fabricated pattern. */
	CHECK(!st_beat_phase_on_beat(&t, 0u, 500u), "loop boundary: the instant of wrap (song_frame 0) is "
						     "before this song's own downbeat_frame -- no pulse yet, "
						     "same fallback as any other pre-downbeat moment");

	/* Once song_frame reaches downbeat_frame again post-wrap, pulsing
	 * resumes exactly as it did the first time -- same formula, same
	 * clock, no drift introduced by the wrap. */
	CHECK(st_beat_phase_on_beat(&t, 1000u, 500u), "loop boundary: post-wrap, once song_frame reaches "
						       "downbeat_frame again, on-beat resumes identically");
}

/* ---- MUTE: a muted (not audible) stem is always ST_TRACK_LED_GHOST,
 * regardless of transport state or beat phase. ---- */
static void test_muted_stem_is_always_ghost(void)
{
	CHECK(st_beat_led_decide(false, true, true) == ST_TRACK_LED_GHOST,
	      "mute: not audible, playing, on-beat -- still ghost");
	CHECK(st_beat_led_decide(false, true, false) == ST_TRACK_LED_GHOST,
	      "mute: not audible, playing, off-beat -- still ghost");
	CHECK(st_beat_led_decide(false, false, false) == ST_TRACK_LED_GHOST,
	      "mute: not audible, stopped -- still ghost");
}

/* ---- MOMENTARY SOLO: an audible stem (soloed, or the only one left
 * after another's momentary solo cleared) pulses exactly like any other
 * audible stem -- the display decision only ever sees the already-
 * computed `audible` boolean, so solo's own momentary/latched distinction
 * is entirely handled upstream (st_track_hold.c/st_stem_mix_channel_
 * audible()) and has no separate code path here. */
static void test_momentary_solo_audible_pulses_like_any_audible_stem(void)
{
	st_stem_mix_channel_t channels[ST11_STEM_COUNT];

	for (uint32_t s = 0; s < ST11_STEM_COUNT; s++) {
		channels[s].gain_q8 = ST_STEM_MIX_GAIN_UNITY_Q8;
		channels[s].mute = false;
		channels[s].solo = false;
	}
	channels[1].solo = true; /* momentary solo currently active on stem 1 */

	bool stem1_audible = st_stem_mix_channel_audible(channels, 1u);
	bool stem0_audible = st_stem_mix_channel_audible(channels, 0u);

	CHECK(stem1_audible && !stem0_audible, "momentary solo: stem 1 (soloed) audible, stem 0 silenced -- "
						"setup check");
	CHECK(st_beat_led_decide(stem1_audible, true, true) == ST_TRACK_LED_ON,
	      "momentary solo: the soloed, audible stem is ON on-beat, exactly like unsoloed audible content");
	CHECK(st_beat_led_decide(stem0_audible, true, true) == ST_TRACK_LED_GHOST,
	      "momentary solo: the silenced-by-solo stem is ghost even though this same tick is on-beat");

	/* Solo releases (momentary -- st_track_hold.c's own behavior,
	 * exercised there): channels[1].solo now false, nothing soloed. */
	channels[1].solo = false;
	bool stem0_after_release = st_stem_mix_channel_audible(channels, 0u);

	CHECK(stem0_after_release, "momentary solo: after release, stem 0 is audible again (no solo pending)");
	CHECK(st_beat_led_decide(stem0_after_release, true, false) == ST_TRACK_LED_OFF,
	      "momentary solo: after release, stem 0 follows the normal audible+off-beat -> OFF rule");
}

/* ---- MULTIPLE AUDIBLE STEMS: two stems soloed together are BOTH
 * audible and BOTH pulse together on the same on_beat tick. ---- */
static void test_multiple_audible_stems_pulse_together(void)
{
	st_stem_mix_channel_t channels[ST11_STEM_COUNT];

	for (uint32_t s = 0; s < ST11_STEM_COUNT; s++) {
		channels[s].gain_q8 = ST_STEM_MIX_GAIN_UNITY_Q8;
		channels[s].mute = false;
		channels[s].solo = false;
	}
	channels[0].solo = true;
	channels[2].solo = true;

	bool a0 = st_stem_mix_channel_audible(channels, 0u);
	bool a1 = st_stem_mix_channel_audible(channels, 1u);
	bool a2 = st_stem_mix_channel_audible(channels, 2u);
	bool a3 = st_stem_mix_channel_audible(channels, 3u);

	CHECK(a0 && a2 && !a1 && !a3, "multiple audible: stems 0 and 2 audible, 1 and 3 silenced -- setup check");

	bool on_beat = true;

	CHECK(st_beat_led_decide(a0, true, on_beat) == ST_TRACK_LED_ON &&
		      st_beat_led_decide(a2, true, on_beat) == ST_TRACK_LED_ON,
	      "multiple audible: both soloed stems (0 and 2) are ON together on the SAME on-beat tick");
	CHECK(st_beat_led_decide(a1, true, on_beat) == ST_TRACK_LED_GHOST &&
		      st_beat_led_decide(a3, true, on_beat) == ST_TRACK_LED_GHOST,
	      "multiple audible: the two non-soloed stems (1 and 3) stay ghost on that same tick");

	on_beat = false;
	CHECK(st_beat_led_decide(a0, true, on_beat) == ST_TRACK_LED_OFF &&
		      st_beat_led_decide(a2, true, on_beat) == ST_TRACK_LED_OFF,
	      "multiple audible: both audible stems go OFF together on the same off-beat tick");
}

/* ---- Stopped transport: audible content is solid, never pulsed, even
 * with a valid on_beat computation available -- matches the classic
 * engine's own "stopped: content reads solid" precedent. ---- */
static void test_stopped_transport_is_solid_not_pulsed(void)
{
	CHECK(st_beat_led_decide(true, false, true) == ST_TRACK_LED_ON,
	      "stopped: audible + not playing + (would-be) on-beat -- solid ON");
	CHECK(st_beat_led_decide(true, false, false) == ST_TRACK_LED_ON,
	      "stopped: audible + not playing + (would-be) off-beat -- STILL solid ON, not OFF");
}

int main(void)
{
	RUN(test_bpm_120_gives_exact_frames_per_beat);
	RUN(test_bpm_non_integer_divisor_rounds_not_truncates);
	RUN(test_bpm_zero_is_invalid);
	RUN(test_sample_rate_zero_is_invalid);
	RUN(test_invalid_timing_never_reports_on_beat);
	RUN(test_downbeat_offset_anchors_phase);
	RUN(test_frames_before_downbeat_never_on_beat);
	RUN(test_loop_wrap_resumes_correctly);
	RUN(test_muted_stem_is_always_ghost);
	RUN(test_momentary_solo_audible_pulses_like_any_audible_stem);
	RUN(test_multiple_audible_stems_pulse_together);
	RUN(test_stopped_transport_is_solid_not_pulsed);

	printf("\n%d distinct test cases, %d assertion checks\n", g_test_cases, g_checks);
	if (g_failures) {
		printf("BEAT PHASE TEST FAILED (%d/%d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("BEAT PHASE TEST PASSED (%d test cases, %d checks)\n", g_test_cases, g_checks);
	return 0;
}
