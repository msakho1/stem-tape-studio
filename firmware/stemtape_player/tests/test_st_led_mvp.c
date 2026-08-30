/*
 * test_st_led_mvp.c — the Stem Tape LED acceptance matrix.
 *
 * Drives the REAL production decision, st_led_mvp_decide(), and the REAL
 * st_beat_pulse() -- the same object files linked into the firmware, the
 * same functions main.c's led_service() calls. Not a reference model.
 *
 * WHAT THIS PROVES: given a described runtime state, the production decision
 * assigns the eight LEDs the levels the product behaviour calls for.
 *
 * WHAT IT DOES NOT PROVE: that the physical device looks right. No GPIO, no
 * TIMER3, no eye. Brightness and fade timing are a human check on hardware.
 *
 *   cc -std=c11 -Wall -Wextra -I../src ../src/st_led_mvp.c ../src/st_beat_phase.c \
 *      test_st_led_mvp.c -o test_st_led_mvp && ./test_st_led_mvp
 */

#include <stdio.h>
#include <string.h>

#include "st_led_mvp.h"
#include "st_ladder.h"   /* ST_LADDER_T1..T4: the SAME bit order */

static int g_checks, g_failures, g_cases;

#define CHECK(cond, ...) do { \
		g_checks++; \
		if (cond) { printf("[OK  ] " __VA_ARGS__); printf("\n"); } \
		else { g_failures++; printf("[FAIL] " __VA_ARGS__); \
		       printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); } \
	} while (0)

static void show(const char *label, const st_led_frame_t *f)
{
	printf("      %-30s T[%3u %3u %3u %3u]  S[%3u %3u %3u %3u]\n", label,
	       f->level[0], f->level[1], f->level[2], f->level[3],
	       f->level[ST_LED_S1], f->level[ST_LED_S2],
	       f->level[ST_LED_S3], f->level[ST_LED_S4]);
}

static bool tracks_all(const st_led_frame_t *f, uint8_t v)
{
	return f->level[0] == v && f->level[1] == v &&
	       f->level[2] == v && f->level[3] == v;
}
static bool side_all(const st_led_frame_t *f, uint8_t v)
{
	return f->level[ST_LED_S1] == v && f->level[ST_LED_S2] == v &&
	       f->level[ST_LED_S3] == v && f->level[ST_LED_S4] == v;
}

/* The side row lights exactly S1..S<level>, everything above it dark. */
static bool side_gauge(const st_led_frame_t *f, int level)
{
	int i;

	for (i = 0; i < (int)ST_LED_SIDE_COUNT; i++) {
		uint8_t want = (i < level) ? ST_LED_MAX : 0u;

		if (f->level[ST_LED_SIDE_FIRST + i] != want) {
			return false;
		}
	}
	return true;
}

/* A song, playing, with a valid tempo. 120 BPM at 48 kHz = 24000 frames/beat. */
static void make_playing(st_led_inputs_t *in, uint32_t song_frame, uint8_t activity)
{
	st_beat_timing_t timing;
	int i;

	memset(in, 0, sizeof(*in));
	in->song_selected = true;
	in->playing = true;
	(void)st_beat_timing_init(&timing, 120u << 8, 0u, 48000u);
	st_beat_pulse(&timing, song_frame, &in->beat);
	for (i = 0; i < (int)ST_LED_TRACK_COUNT; i++) {
		in->stem_activity[i] = activity;
	}
}

/* ======================= 1. boot sequence ================================ */
static void case_boot(void)
{
	st_led_inputs_t in; st_led_frame_t f;

	g_cases++;
	printf("\n-- Power-on sequence\n");
	memset(&in, 0, sizeof(in));
	in.sequence = ST_LED_SEQ_BOOT;
	in.batt_state = ST_LED_BATT_CHARGER_ABSENT;
	in.batt_level = 3u;

	in.sequence_ms = 0u;
	st_led_mvp_decide(&in, &f);
	show("t=0 (blink + side on)", &f);
	CHECK(tracks_all(&f, ST_LED_MAX), "all four Track LEDs blink together at t=0");
	CHECK(side_all(&f, ST_LED_MAX), "all four side LEDs illuminate together at t=0");

	in.sequence_ms = ST_LED_TRACK_BLINK_MS;
	st_led_mvp_decide(&in, &f);
	show("t=100 (blink done)", &f);
	CHECK(tracks_all(&f, 0u), "the Track blink finishes and they go completely dark");

	in.sequence_ms = ST_LED_SIDE_HOLD_MS + (ST_LED_SIDE_FADE_MS / 2u);
	st_led_mvp_decide(&in, &f);
	show("mid-fade", &f);
	CHECK(f.level[ST_LED_S1] > 0u && f.level[ST_LED_S1] < ST_LED_MAX,
	      "the side row is mid-fade: partially lit, which needs real brightness");
	CHECK(side_all(&f, f.level[ST_LED_S1]), "all four fade together");

	in.sequence_ms = ST_LED_BOOT_FADE_END_MS;
	st_led_mvp_decide(&in, &f);
	show("battery preview lv3", &f);
	CHECK(tracks_all(&f, 0u), "Track LEDs stay dark through the battery preview");
	CHECK(f.level[ST_LED_S1] == ST_LED_MAX && f.level[ST_LED_S2] == ST_LED_MAX &&
	      f.level[ST_LED_S3] == ST_LED_MAX && f.level[ST_LED_S4] == 0u,
	      "battery level 3 of 4 previews as S1-S3");
}

/* ==================== 2. battery preview levels ========================== */
static void case_preview_levels(void)
{
	st_led_inputs_t in; st_led_frame_t f;
	const uint8_t expect[5][4] = {
		{ 0, 0, 0, 0 },                                  /* unused */
		{ ST_LED_MAX, 0, 0, 0 },                          /* level 1: S1 */
		{ ST_LED_MAX, ST_LED_MAX, 0, 0 },                 /* level 2: S1-S2 */
		{ ST_LED_MAX, ST_LED_MAX, ST_LED_MAX, 0 },        /* level 3: S1-S3 */
		{ ST_LED_MAX, ST_LED_MAX, ST_LED_MAX, ST_LED_MAX },/* full: S1-S4 */
	};
	int lv;

	g_cases++;
	printf("\n-- Battery preview, each level\n");
	for (lv = 1; lv <= 4; lv++) {
		char lbl[40];

		memset(&in, 0, sizeof(in));
		in.sequence = ST_LED_SEQ_BOOT;
		in.sequence_ms = ST_LED_BOOT_FADE_END_MS + 10u;
		in.batt_state = ST_LED_BATT_CHARGER_ABSENT;
		in.batt_level = (uint8_t)lv;
		st_led_mvp_decide(&in, &f);
		snprintf(lbl, sizeof(lbl), "preview level %d", lv);
		show(lbl, &f);
		CHECK(f.level[ST_LED_S1] == expect[lv][0] && f.level[ST_LED_S2] == expect[lv][1] &&
		      f.level[ST_LED_S3] == expect[lv][2] && f.level[ST_LED_S4] == expect[lv][3],
		      "level %d lights exactly S1..S%d", lv, lv);
		CHECK(tracks_all(&f, 0u), "Track LEDs dark during the preview");
	}

	/* Untrustworthy data must not fabricate a level. */
	memset(&in, 0, sizeof(in));
	in.sequence = ST_LED_SEQ_BOOT;
	in.sequence_ms = ST_LED_BOOT_FADE_END_MS + 10u;
	in.batt_state = ST_LED_BATT_UNAVAILABLE;
	in.batt_level = 0u;
	st_led_mvp_decide(&in, &f);
	show("preview, no reading", &f);
	CHECK(side_all(&f, 0u), "an unavailable reading previews as darkness, never a level");
}

/* ============ 3. stopped: the PERSISTENT battery gauge ==================
 *
 * The product rule: whenever the SP-1 is powered on and no song is playing,
 * the side row continuously shows the current trustworthy battery level.
 * There is no "idle, therefore dark" state. An earlier revision of this
 * matrix asserted the opposite -- that the side row went dark once the boot
 * preview elapsed -- and so proved the wrong requirement; every such
 * assertion is gone.
 */
static void case_stopped_gauge(void)
{
	st_led_inputs_t in; st_led_frame_t f;
	int lv;

	g_cases++;
	printf("\n-- Stopped: persistent battery gauge on the side row\n");

	/* Rows 1-4 of the matrix: stopped with a song loaded, each level. */
	for (lv = 1; lv <= 4; lv++) {
		char lbl[48];

		memset(&in, 0, sizeof(in));
		in.song_selected = true;            /* a song IS loaded, stopped */
		in.batt_state = ST_LED_BATT_CHARGER_ABSENT;
		in.batt_level = (uint8_t)lv;
		st_led_mvp_decide(&in, &f);
		snprintf(lbl, sizeof(lbl), "stopped, battery level %d", lv);
		show(lbl, &f);
		CHECK(side_gauge(&f, lv),
		      "stopped at level %d lights exactly S1..S%d, continuously -- not for a "
		      "preview window, and not only while charging", lv, lv);
		CHECK(tracks_all(&f, 0u),
		      "level %d: Track LEDs are completely off -- a loaded but stopped song "
		      "does NOT light them", lv);
	}

	/* Matrix row: no song selected at all. */
	memset(&in, 0, sizeof(in));
	in.song_selected = false;
	in.batt_state = ST_LED_BATT_CHARGER_ABSENT;
	in.batt_level = 3u;
	st_led_mvp_decide(&in, &f);
	show("no song selected, level 3", &f);
	CHECK(side_gauge(&f, 3), "with NO song selected the gauge still shows S1-S3");
	CHECK(tracks_all(&f, 0u), "no song selected: Track LEDs dark");

	/* Matrix row: low battery renders its measured level continuously, and
	 * is emphatically not a reason to darken the row. */
	memset(&in, 0, sizeof(in));
	in.song_selected = true;
	in.batt_state = ST_LED_BATT_LOW;
	in.batt_level = 1u;
	st_led_mvp_decide(&in, &f);
	show("stopped, LOW battery", &f);
	CHECK(side_gauge(&f, 1), "a LOW battery renders S1 solid and continuous");
	CHECK(tracks_all(&f, 0u), "low battery: Track LEDs dark");

	/* Matrix row: untrustworthy readings darken the side row rather than
	 * inventing a level. This is the ONLY stopped-state darkness. */
	memset(&in, 0, sizeof(in));
	in.song_selected = true;
	in.batt_state = ST_LED_BATT_UNAVAILABLE;
	in.batt_level = 0u;
	st_led_mvp_decide(&in, &f);
	show("stopped, no reading yet", &f);
	CHECK(side_all(&f, 0u),
	      "an UNAVAILABLE reading leaves the side row dark -- never a fabricated level");

	memset(&in, 0, sizeof(in));
	in.song_selected = true;
	in.batt_state = ST_LED_BATT_FAULT;
	in.batt_level = 3u;                 /* a stale level, deliberately nonzero */
	st_led_mvp_decide(&in, &f);
	show("stopped, FAULTed reading", &f);
	CHECK(side_all(&f, 0u),
	      "a FAULTed reading darkens the row even though a previous level exists -- a "
	      "distrusted gauge is never shown");

	/* Never-seeded level with an otherwise-plausible state. */
	memset(&in, 0, sizeof(in));
	in.song_selected = true;
	in.batt_state = ST_LED_BATT_CHARGER_ABSENT;
	in.batt_level = 0u;                 /* never seeded */
	st_led_mvp_decide(&in, &f);
	show("stopped, unseeded level", &f);
	CHECK(side_all(&f, 0u), "an unseeded level (0) is never rendered as a charge");

	/* Matrix row: an untrusted gauge darkens the SIDE row only. The Track
	 * row keeps whatever it would normally be doing -- here, a held solo. */
	memset(&in, 0, sizeof(in));
	in.song_selected = true;
	in.batt_state = ST_LED_BATT_FAULT;
	in.batt_level = 2u;
	in.solo_mask = ST_LADDER_T2;
	st_led_mvp_decide(&in, &f);
	show("faulted gauge + solo", &f);
	CHECK(side_all(&f, 0u), "faulted gauge: side row dark");
	CHECK(f.level[1] == ST_LED_MAX && f.level[0] == 0u && f.level[2] == 0u &&
	      f.level[3] == 0u,
	      "and the Track row is in its NORMAL state -- the held solo still shows, because "
	      "a distrusted battery reading is a side-row fact and nothing more");
}

/* ================= 4. playing: the Track row IS the audio ================= *
 *
 * REWRITTEN AGAINST THE CURRENT CONTRACT, and worth saying why rather than
 * quietly editing the numbers. This case used to assert the opposite of what
 * it asserts now: a shared beat envelope on all four Track LEDs, a full-
 * brightness chase accent on the beat's own lane, and everything dark between
 * pulses. That display told the player what the clock was doing -- which they
 * can hear -- instead of what each stem was doing, which they cannot see any
 * other way. The Track row now carries each stem's own enveloped level with no
 * beat gate and no accent; S4 keeps the beat, and is the only light that has
 * it. tests/test_led_audio_reactive.c is the behavioural proof against real
 * synthesised material; this case pins the frame-level contract.
 */
static void case_playing(void)
{
	st_led_inputs_t in; st_led_frame_t f;
	uint32_t fpb = 24000u;   /* 120 BPM @ 48 kHz */
	int b;

	g_cases++;
	printf("\n-- Playing: the Track row is per-stem audio; S4 alone is the beat\n");

	for (b = 0; b < 4; b++) {
		char lbl[48];
		/* A little way into beat b, near the envelope peak. */
		uint32_t frame = (uint32_t)b * fpb + (fpb / ST_BEAT_PULSE_DEN) / 2u;
		int k;

		make_playing(&in, frame, 128u);
		st_led_mvp_decide(&in, &f);
		snprintf(lbl, sizeof(lbl), "beat %d, all stems at 128", b + 1);
		show(lbl, &f);

		CHECK(in.beat.beat_index == (uint8_t)b,
		      "beat index %d derives from STIX timing and song_frame", b);
		/* NO ACCENT. With every stem at the same activity, every Track
		 * LED must read the same -- the bar position must not leak into
		 * the row at all. */
		for (k = 0; k < 4; k++) {
			CHECK(f.level[k] == in.stem_activity[k],
			      "T%d carries its own stem's activity exactly (%u, "
			      "expected %u) -- beat %d must not accent it",
			      k + 1, f.level[k], in.stem_activity[k], b + 1);
		}
		CHECK(f.level[ST_LED_S4] == in.beat.envelope,
		      "S4 still pulses with the beat envelope -- the tempo was "
		      "removed from the Track row, not from the device");
		CHECK(f.level[ST_LED_S1] == 0u && f.level[ST_LED_S2] == 0u &&
		      f.level[ST_LED_S3] == 0u, "S1-S3 are off during playback");
	}

	/* BETWEEN BEATS: the Track row is UNCHANGED, because the audio is. */
	{
		st_led_frame_t on_beat, off_beat;
		uint32_t at   = 1u * fpb + (fpb / ST_BEAT_PULSE_DEN) / 2u;
		uint32_t away = (fpb / ST_BEAT_PULSE_DEN) + (fpb / 2u);

		make_playing(&in, at, 200u);
		st_led_mvp_decide(&in, &on_beat);
		make_playing(&in, away, 200u);
		st_led_mvp_decide(&in, &off_beat);
		show("on the beat,  activity 200", &on_beat);
		show("between beats, activity 200", &off_beat);

		CHECK(!in.beat.in_pulse, "the second frame is outside the pulse window");
		CHECK(tracks_all(&off_beat, 200u),
		      "between beats the Track row still shows the audio -- going "
		      "dark here is the beat-indicator display this replaced");
		CHECK(on_beat.level[0] == off_beat.level[0] &&
		      on_beat.level[1] == off_beat.level[1] &&
		      on_beat.level[2] == off_beat.level[2] &&
		      on_beat.level[3] == off_beat.level[3],
		      "and it is IDENTICAL on and off the beat for identical "
		      "audio: the beat must not reach the Track row at all");
		CHECK(off_beat.level[ST_LED_S4] == 0u,
		      "S4, which IS the beat light, is dark between pulses");
	}

	/* Louder stems read brighter, on or off the beat. */
	{
		st_led_frame_t loud, quiet;
		uint32_t frame = 1u * fpb + (fpb / ST_BEAT_PULSE_DEN) / 2u;   /* beat 2 */

		make_playing(&in, frame, 255u);
		st_led_mvp_decide(&in, &loud);
		make_playing(&in, frame, 40u);
		st_led_mvp_decide(&in, &quiet);
		show("beat 2, loud stems", &loud);
		show("beat 2, quiet stems", &quiet);
		CHECK(loud.level[0] > quiet.level[0],
		      "a louder stem reads brighter than a quiet one (T1)");
		CHECK(loud.level[1] > quiet.level[1],
		      "and so does T2 -- there is no accented lane exempt from "
		      "the audio any more");
	}

	/* No tempo: no fabricated pulse. */
	{
		st_beat_timing_t bad;

		memset(&in, 0, sizeof(in));
		in.song_selected = true;
		in.playing = true;
		(void)st_beat_timing_init(&bad, 0u, 0u, 48000u);   /* absent tempo */
		st_beat_pulse(&bad, 12345u, &in.beat);
		st_led_mvp_decide(&in, &f);
		show("playing, no tempo", &f);
		CHECK(!in.beat.valid, "an absent tempo yields an invalid pulse, not a guess");
		CHECK(tracks_all(&f, 0u), "and no fabricated animation");
	}
}

/* ================ 4b. the loop chase, locked to the song BPM ==============
 *
 * While a loop runs, the Track row becomes a POSITION display rather than a
 * beat pulse: exactly one LED at full brightness, advancing T1 -> T2 -> T3 ->
 * T4 in tempo, T1 on the downbeat, and HELD between beats rather than pulsed.
 *
 * The frames below are computed from the real 120 BPM @ 48 kHz timing the
 * other playing cases use -- 24000 frames per beat -- and fed through the
 * real st_beat_pulse(), so the chase is proven to derive from bpm_q8 and the
 * downbeat rather than from a counter of the LED layer's own.
 */
static void case_loop_chase(void)
{
	st_led_inputs_t in; st_led_frame_t f;
	const uint32_t fpb = 24000u;   /* 120 BPM @ 48 kHz */
	const st_led_loop_t states[2] = { ST_LED_LOOP_MOMENTARY,
					   ST_LED_LOOP_LATCHED };
	int b, s;

	g_cases++;
	printf("\n-- Looping: bright T1->T2->T3->T4 chase locked to the song BPM\n");

	for (s = 0; s < 2; s++) {
		for (b = 0; b < 4; b++) {
			char lbl[64];
			int k, lit = 0;
			/* Deep BETWEEN beats -- three quarters of the way into
			 * the beat, far outside the pulse window -- which is
			 * exactly where a pulse display would be dark. */
			uint32_t frame = (uint32_t)b * fpb + (fpb * 3u) / 4u;

			make_playing(&in, frame, 0u);
			in.loop_state = states[s];
			st_led_mvp_decide(&in, &f);
			snprintf(lbl, sizeof(lbl), "%s loop, beat %d, between pulses",
				 s ? "latched" : "momentary", b + 1);
			show(lbl, &f);

			CHECK(!in.beat.in_pulse,
			      "the frame is outside the pulse window, where a "
			      "pulse display would be dark");
			CHECK(in.beat.beat_index == (uint8_t)b,
			      "beat index %d derives from the STIX bpm_q8/downbeat "
			      "and the song frame", b);
			CHECK(f.level[b] == ST_LED_MAX,
			      "T%d is FULLY bright on beat %d", b + 1, b + 1);
			for (k = 0; k < (int)ST_LED_TRACK_COUNT; k++) {
				if (f.level[k]) {
					lit++;
				}
			}
			CHECK(lit == 1,
			      "exactly one Track LED is lit -- a position, not a "
			      "pulse (%d lit)", lit);
		}
	}

	/* T1 IS THE DOWNBEAT, at the downbeat frame itself. */
	make_playing(&in, 0u, 0u);
	in.loop_state = ST_LED_LOOP_MOMENTARY;
	st_led_mvp_decide(&in, &f);
	show("looping, exactly on the downbeat", &f);
	CHECK(f.level[0] == ST_LED_MAX && f.level[1] == 0u &&
	      f.level[2] == 0u && f.level[3] == 0u,
	      "T1 represents the downbeat");

	/* ACTIVITY DOES NOT TOUCH IT. The chase is a position display; a loud
	 * or silent stem must not change which lane is lit or how brightly. */
	{
		st_led_frame_t loud, silent;
		uint32_t frame = 2u * fpb + (fpb * 3u) / 4u;   /* beat 3 */

		make_playing(&in, frame, 255u);
		in.loop_state = ST_LED_LOOP_LATCHED;
		st_led_mvp_decide(&in, &loud);
		make_playing(&in, frame, 0u);
		in.loop_state = ST_LED_LOOP_LATCHED;
		st_led_mvp_decide(&in, &silent);
		CHECK(memcmp(loud.level, silent.level, ST_LED_TRACK_COUNT) == 0,
		      "stem activity does not modulate the chase");
		CHECK(loud.level[2] == ST_LED_MAX, "and beat 3 lights T3");
	}

	/* THE LATCHED MARKER SURVIVES. S1 solid is the shipped, physically
	 * verified indication and the chase must not displace it. */
	make_playing(&in, 1u * fpb + (fpb * 3u) / 4u, 0u);
	in.loop_state = ST_LED_LOOP_LATCHED;
	st_led_mvp_decide(&in, &f);
	CHECK(f.level[ST_LED_S1] == ST_LED_MAX,
	      "a latched loop still shows S1 solid alongside the chase");

	/* NO TEMPO, NO CHASE. A song with no bpm_q8 must not fabricate one. */
	{
		st_beat_timing_t bad;

		memset(&in, 0, sizeof(in));
		in.song_selected = true;
		in.playing = true;
		in.loop_state = ST_LED_LOOP_MOMENTARY;
		(void)st_beat_timing_init(&bad, 0u, 0u, 48000u);
		st_beat_pulse(&bad, 99999u, &in.beat);
		st_led_mvp_decide(&in, &f);
		show("looping, no tempo", &f);
		CHECK(tracks_all(&f, 0u),
		      "an absent tempo yields no chase, not a guessed one");
	}

	/* AND WITHOUT A LOOP, the row is the audio -- including between
	 * pulses. This used to assert the row was DARK here, which was the
	 * beat-indicator contract; see case_playing()'s own note. The scoping
	 * claim it was really making -- that the chase belongs to looping and
	 * changes nothing else -- is still checked, just against the display
	 * that now exists. */
	{
		st_led_frame_t before;

		make_playing(&in, 1u * fpb + (fpb * 3u) / 4u, 200u);
		st_led_mvp_decide(&in, &before);
		show("NOT looping, between pulses", &before);
		CHECK(tracks_all(&before, 200u),
		      "without a loop the Track row shows each stem's activity, "
		      "between pulses as much as on them");
	}

	/* NO TEMPO, BUT STILL AUDIO. A song whose STIX record carries no usable
	 * BPM has no beat to show and never did -- but it still has sound, and
	 * the Track row must still show it. The old beat-driven row went dark
	 * here and had nothing else to draw; this is the case that records the
	 * difference. */
	{
		st_beat_timing_t bad;
		st_led_frame_t f2;
		int k;

		memset(&in, 0, sizeof(in));
		in.song_selected = true;
		in.playing = true;
		for (k = 0; k < 4; k++) {
			in.stem_activity[k] = (uint8_t)(60u + 40u * (unsigned)k);
		}
		(void)st_beat_timing_init(&bad, 0u, 0u, 48000u);
		st_beat_pulse(&bad, 99999u, &in.beat);
		st_led_mvp_decide(&in, &f2);
		show("no usable tempo, stems sounding", &f2);
		CHECK(!in.beat.valid, "the tempo really is unusable");
		for (k = 0; k < 4; k++) {
			CHECK(f2.level[k] == in.stem_activity[k],
			      "T%d still shows its stem (%u) with no tempo at all",
			      k + 1, f2.level[k]);
		}
		CHECK(f2.level[ST_LED_S4] == 0u,
		      "and S4 shows nothing rather than a fabricated beat");
	}
}

/* ================= 4c. the FX overlay display ============================
 *
 * Ranked above solo and the loop chase: while the overlay is open the Track
 * buttons are effects, so solo feedback would be showing something they no
 * longer do.
 */
static void case_fx_overlay(void)
{
	st_led_inputs_t in; st_led_frame_t f;
	const uint32_t fpb = 24000u;
	int i, lit;

	g_cases++;
	printf("\n-- FX overlay: track row = effects, side row = where the rack is\n");

	/* STEM scope, one latched effect, flash phase ON. */
	make_playing(&in, 1u * fpb + (fpb * 3u) / 4u, 200u);
	in.fx_open = true;
	in.fx_global = false;
	in.fx_target = 2u;                       /* Bass */
	in.fx_latched = 1u << 0;                 /* T1 Filter */
	in.fx_flash_on = true;
	st_led_mvp_decide(&in, &f);
	show("FX stem scope, Filter latched, flash ON, target Bass", &f);
	CHECK(f.level[0] == ST_LED_MAX, "T1 lit on the flash's ON phase");
	CHECK(f.level[1] == 0u && f.level[2] == 0u && f.level[3] == 0u,
	      "and the other three effects are dark");
	CHECK(f.level[ST_LED_SIDE_FIRST + 2] == ST_LED_MAX,
	      "the target stem's side LED is lit");
	lit = 0;
	for (i = 0; i < (int)ST_LED_SIDE_COUNT; i++) {
		if (f.level[ST_LED_SIDE_FIRST + i]) lit++;
	}
	CHECK(lit == 1, "and ONLY that one -- a single stem reads as a single light");

	/* THE FLASH ACTUALLY FLASHES. The reported hardware symptom was "no LED
	 * indication" while using an effect, so the assertion that matters is
	 * that the two phases DIFFER -- a steady light is the failure. */
	{
		st_led_frame_t on_f, off_f;

		make_playing(&in, 1u * fpb + (fpb * 3u) / 4u, 200u);
		in.fx_open = true;
		in.fx_target = 1u;
		in.fx_momentary = 1u << 1;           /* T2 held */
		in.fx_flash_on = true;
		st_led_mvp_decide(&in, &on_f);
		in.fx_flash_on = false;
		st_led_mvp_decide(&in, &off_f);
		show("FX flash ON ", &on_f);
		show("FX flash OFF", &off_f);
		CHECK(on_f.level[1] == ST_LED_MAX, "the held effect is full on the ON phase");
		CHECK(off_f.level[1] == 0u, "and dark on the OFF phase -- it flashes");
		CHECK(on_f.level[ST_LED_SIDE_FIRST + 1] != off_f.level[ST_LED_SIDE_FIRST + 1],
		      "the stem being processed flashes in step with it");
	}

	/* A SOUNDING EFFECT NEVER SITS STEADY, whatever the beat envelope is
	 * doing. The previous design rode beat.envelope, which is dark for three
	 * quarters of every beat, and read as "nothing is happening". */
	{
		uint32_t k;
		int steady = 0;

		for (k = 0; k < 8u; k++) {
			st_led_frame_t a, b;

			make_playing(&in, 1u * fpb + (fpb * k) / 8u, 200u);
			in.fx_open = true;
			in.fx_latched = 1u << 3;
			in.fx_flash_on = true;
			st_led_mvp_decide(&in, &a);
			in.fx_flash_on = false;
			st_led_mvp_decide(&in, &b);
			if (a.level[3] == b.level[3]) steady++;
		}
		CHECK(steady == 0,
		      "at no beat phase does an active effect render the same on "
		      "both flash phases (%d of 8 were steady)", steady);
	}

	/* Latched and momentary read the SAME while sounding -- deliberate, and
	 * asserted so it cannot drift back by accident. */
	make_playing(&in, 1u * fpb + (fpb / ST_BEAT_PULSE_DEN) / 2u, 200u);
	in.fx_open = true;
	in.fx_target = 0u;
	in.fx_latched = 1u << 2;                 /* T3 latched */
	in.fx_momentary = 1u << 1;               /* T2 held */
	in.fx_flash_on = true;
	st_led_mvp_decide(&in, &f);
	show("FX: T3 latched, T2 momentary, flash ON", &f);
	CHECK(f.level[2] == ST_LED_MAX, "the latched effect is lit");
	CHECK(f.level[1] == ST_LED_MAX, "the momentary one is lit the same way");
	CHECK(f.level[0] == 0u && f.level[3] == 0u, "the untouched effects stay dark");

	/* NO TEMPO: the caller holds the phase true, so an active effect is
	 * SOLID rather than invisible. Nothing here invents a grid. */
	{
		st_beat_timing_t bad;

		memset(&in, 0, sizeof(in));
		in.song_selected = true;
		in.playing = true;
		in.fx_open = true;
		in.fx_momentary = 1u << 3;
		in.fx_flash_on = true;               /* main.c's fail-closed value */
		(void)st_beat_timing_init(&bad, 0u, 0u, 48000u);
		st_beat_pulse(&bad, 999u, &in.beat);
		st_led_mvp_decide(&in, &f);
		CHECK(f.level[3] == ST_LED_MAX,
		      "a held effect is fully visible even with no tempo to flash on");
	}

	/* OVERLAY OPEN, NOTHING SOUNDING: the target sits STEADY. That steady
	 * light is the "FX mode is open" indication and must not be confused
	 * with an effect, so it must NOT depend on the flash phase. */
	{
		st_led_frame_t on_f, off_f;

		make_playing(&in, 1u * fpb + (fpb * 3u) / 4u, 200u);
		in.fx_open = true;
		in.fx_target = 3u;
		in.fx_flash_on = true;
		st_led_mvp_decide(&in, &on_f);
		in.fx_flash_on = false;
		st_led_mvp_decide(&in, &off_f);
		CHECK(on_f.level[ST_LED_SIDE_FIRST + 3] == ST_LED_MAX &&
		      off_f.level[ST_LED_SIDE_FIRST + 3] == ST_LED_MAX,
		      "an open overlay with no effect shows a STEADY target light");
		for (i = 0; i < (int)ST_LED_TRACK_COUNT; i++) {
			CHECK(on_f.level[i] == 0u,
			      "and no effect LED is lit when none is active (T%d)", i + 1);
		}
	}

	/* GLOBAL scope lights the whole side row, and not like a single stem. */
	make_playing(&in, 1u * fpb + (fpb * 3u) / 4u, 200u);
	in.fx_open = true;
	in.fx_global = true;
	in.fx_latched = 0x0Fu;                   /* all four latched */
	in.fx_flash_on = true;
	st_led_mvp_decide(&in, &f);
	show("FX global scope, all four latched, flash ON", &f);
	for (i = 0; i < (int)ST_LED_TRACK_COUNT; i++) {
		CHECK(f.level[i] == ST_LED_MAX, "T%d lit", i + 1);
	}
	lit = 0;
	for (i = 0; i < (int)ST_LED_SIDE_COUNT; i++) {
		if (f.level[ST_LED_SIDE_FIRST + i]) lit++;
	}
	CHECK(lit == (int)ST_LED_SIDE_COUNT, "the whole side row is lit for GLOBAL");
	CHECK(f.level[ST_LED_SIDE_FIRST] < ST_LED_MAX,
	      "at a LOWER level than a single selected stem, so GLOBAL and STEM "
	      "never read the same");

	/* FX OUTRANKS SOLO: the Track buttons are effects while it is open. */
	make_playing(&in, 1u * fpb + (fpb * 3u) / 4u, 200u);
	in.fx_open = true;
	in.fx_latched = 1u << 0;
	in.fx_flash_on = true;
	in.solo_mask = 0x0Fu;                    /* would light all four */
	st_led_mvp_decide(&in, &f);
	CHECK(f.level[1] == 0u && f.level[2] == 0u && f.level[3] == 0u,
	      "a solo mask does NOT light the track row while the overlay is open");

	/* AND CLOSING IT RESTORES THE UNDERLYING MODE IMMEDIATELY. */
	{
		st_led_frame_t open_f, closed_f, never_f;

		make_playing(&in, 1u * fpb + (fpb * 3u) / 4u, 200u);
		in.loop_state = ST_LED_LOOP_LATCHED;
		in.fx_open = true;
		in.fx_latched = 1u << 0;
		in.fx_flash_on = true;
		st_led_mvp_decide(&in, &open_f);

		in.fx_open = false;
		st_led_mvp_decide(&in, &closed_f);

		make_playing(&in, 1u * fpb + (fpb * 3u) / 4u, 200u);
		in.loop_state = ST_LED_LOOP_LATCHED;
		st_led_mvp_decide(&in, &never_f);

		show("overlay open", &open_f);
		show("overlay closed", &closed_f);
		CHECK(memcmp(closed_f.level, never_f.level, ST_LED_COUNT) == 0,
		      "closing the overlay gives EXACTLY the frame a device that "
		      "never opened it would show -- the loop chase resumes at the "
		      "correct musical phase because nothing here owns a clock");
		CHECK(memcmp(open_f.level, closed_f.level, ST_LED_COUNT) != 0,
		      "and the two really were different, so the check has teeth");
	}
}

/* ==================== 5. immediate momentary solo ======================== */
static void case_solo(void)
{
	st_led_inputs_t in; st_led_frame_t f, before;
	uint32_t fpb = 24000u;
	uint32_t frame = 2u * fpb + (fpb / ST_BEAT_PULSE_DEN) / 2u;   /* beat 3 */

	g_cases++;
	printf("\n-- Immediate momentary solo (Track 2 held)\n");

	make_playing(&in, frame, 200u);
	st_led_mvp_decide(&in, &before);
	show("before press", &before);

	in.solo_mask = ST_LADDER_T2;    /* Track 2 */
	st_led_mvp_decide(&in, &f);
	show("Track 2 held", &f);
	CHECK(f.level[1] == ST_LED_MAX,
	      "the held stem is at MAXIMUM brightness -- never faint, never ghosted");
	CHECK(f.level[0] == 0u && f.level[2] == 0u && f.level[3] == 0u,
	      "every other Track LED is completely off");
	CHECK(f.level[ST_LED_S4] == before.level[ST_LED_S4],
	      "S4 keeps its tempo pulse under a solo -- solo overrides only the track row");

	in.solo_mask = 0u;
	st_led_mvp_decide(&in, &f);
	show("released", &f);
	CHECK(memcmp(&f, &before, sizeof(f)) == 0,
	      "release restores the beat/chase at the SAME song position -- the decision "
	      "holds no state, so nothing restarts");

	/* Solo outranks even a stopped transport -- and touches ONLY the track
	 * row, so the stopped-state battery gauge stays lit underneath it. */
	memset(&in, 0, sizeof(in));
	in.song_selected = true;
	in.solo_mask = ST_LADDER_T4;
	in.batt_state = ST_LED_BATT_CHARGER_ABSENT;
	in.batt_level = 3u;
	st_led_mvp_decide(&in, &f);
	show("solo while stopped, lv3", &f);
	CHECK(f.level[3] == ST_LED_MAX && f.level[0] == 0u,
	      "solo shows immediately even with the transport stopped");
	CHECK(side_gauge(&f, 3),
	      "and the stopped-state battery gauge stays on S1-S3 underneath it -- solo is a "
	      "Track-row override, never a side-row one");
}

/* ============ 5b. MULTI-STEM SOLO CHORDS on the Track row ================
 *
 * The LED row is driven by the SAME mask main.c hands the mixer, so a chord
 * is not a special case here -- more bits are set, more LEDs are lit, and
 * every Track that is not held is dark. ALL FIFTEEN masks are exercised:
 * physical measurement (docs/ladder-measured.json) shows the ladder resolves
 * every one of them, including the four containing both Track 3 and Track 4
 * that st15 wrongly declared undecodable.
 */
static void case_solo_chords(void)
{
	st_led_inputs_t in; st_led_frame_t f;
	uint32_t fpb = 24000u;
	uint32_t frame = (fpb / ST_BEAT_PULSE_DEN) / 2u;
	int i;
	/* Every multi-button mask the ladder resolves: 2 through 15 with more
	 * than one bit set. */
	const uint8_t chords[] = {
		0x3u, 0x5u, 0x6u, 0x7u, 0x9u, 0xAu, 0xBu,
		0xCu, 0xDu, 0xEu, 0xFu,
	};

	g_cases++;
	printf("\n-- Multi-stem solo chords (every decodable mask)\n");

	for (i = 0; i < (int)(sizeof(chords) / sizeof(chords[0])); i++) {
		uint8_t m = chords[i];
		char lbl[48];
		bool exact = true;
		int k, held = 0;

		make_playing(&in, frame, 128u);
		in.solo_mask = m;
		st_led_mvp_decide(&in, &f);
		snprintf(lbl, sizeof(lbl), "chord %c%c%c%c",
			 (m & ST_LADDER_T1) ? '1' : '0', (m & ST_LADDER_T2) ? '1' : '0',
			 (m & ST_LADDER_T3) ? '1' : '0', (m & ST_LADDER_T4) ? '1' : '0');
		show(lbl, &f);
		for (k = 0; k < (int)ST_LED_TRACK_COUNT; k++) {
			uint8_t want = ((m >> k) & 1u) ? ST_LED_MAX : 0u;

			if (f.level[k] != want) {
				exact = false;
			}
			if ((m >> k) & 1u) {
				held++;
			}
		}
		CHECK(exact,
		      "%s: exactly the %d held Track LEDs are at MAXIMUM and the rest are "
		      "completely dark -- no partial brightness, no beat pulse bleeding "
		      "through", lbl, held);
		CHECK(f.level[ST_LED_S4] == in.beat.envelope,
		      "%s: S4 keeps its tempo pulse -- a chord overrides the Track row only",
		      lbl);
	}

	/* ADD a finger: the existing member must not move. */
	make_playing(&in, frame, 128u);
	in.solo_mask = ST_LADDER_T1;
	st_led_mvp_decide(&in, &f);
	CHECK(f.level[0] == ST_LED_MAX && f.level[1] == 0u,
	      "T1 alone: only T1 lit");
	in.solo_mask = ST_LADDER_T1 | ST_LADDER_T2;
	st_led_mvp_decide(&in, &f);
	show("added T2 to T1", &f);
	CHECK(f.level[0] == ST_LED_MAX && f.level[1] == ST_LED_MAX &&
	      f.level[2] == 0u && f.level[3] == 0u,
	      "adding Track 2 lights T2 and leaves T1 exactly where it was");

	/* REMOVE one member: the survivor stays lit, the leaver goes dark. */
	in.solo_mask = ST_LADDER_T2;
	st_led_mvp_decide(&in, &f);
	show("released T1, T2 held", &f);
	CHECK(f.level[0] == 0u && f.level[1] == ST_LED_MAX,
	      "releasing Track 1 darkens only T1 -- Track 2 is still soloed and still lit");

	/* RELEASE ALL: straight back to the beat/chase at the current phase, not
	 * to some restart or a blank row. */
	{
		st_led_frame_t normal;

		make_playing(&in, frame, 128u);
		in.solo_mask = 0u;
		st_led_mvp_decide(&in, &normal);
		in.solo_mask = ST_LADDER_T1 | ST_LADDER_T2 | ST_LADDER_T4;
		st_led_mvp_decide(&in, &f);
		in.solo_mask = 0u;
		st_led_mvp_decide(&in, &f);
		show("all released", &f);
		CHECK(memcmp(&f, &normal, sizeof(f)) == 0,
		      "releasing every Track restores the normal playback animation at the "
		      "SAME musical phase -- the decision holds no state, so nothing restarts");
	}
}

/* ======================= 6. charging gauge =============================== */
static void case_charging(void)
{
	st_led_inputs_t in; st_led_frame_t f;

	g_cases++;
	printf("\n-- Charging gauge (stopped)\n");
	memset(&in, 0, sizeof(in));
	in.song_selected = true;
	in.batt_state = ST_LED_BATT_CHARGING;
	in.batt_level = 2u;

	in.batt_blink_on = true;
	st_led_mvp_decide(&in, &f);
	show("charging lv2, blink on", &f);
	CHECK(f.level[ST_LED_S1] == ST_LED_MAX, "completed step S1 stays solid");
	CHECK(f.level[ST_LED_S2] == ST_LED_MAX, "the current step S2 is lit on the on-phase");

	in.batt_blink_on = false;
	st_led_mvp_decide(&in, &f);
	show("charging lv2, blink off", &f);
	CHECK(f.level[ST_LED_S1] == ST_LED_MAX, "S1 does NOT blink with it");
	CHECK(f.level[ST_LED_S2] == 0u, "only the current step blinks");
	CHECK(tracks_all(&f, 0u), "Track LEDs stay off while charging and stopped");

	in.batt_state = ST_LED_BATT_CHARGE_COMPLETE;
	st_led_mvp_decide(&in, &f);
	show("fully charged", &f);
	CHECK(side_all(&f, ST_LED_MAX), "fully charged leaves all four side LEDs solid");

	/* Playback outranks the charging display. */
	{
		uint32_t fpb = 24000u;

		make_playing(&in, (fpb / ST_BEAT_PULSE_DEN) / 2u, 128u);
		in.batt_state = ST_LED_BATT_CHARGING;
		in.batt_level = 2u;
		st_led_mvp_decide(&in, &f);
		show("playing while charging", &f);
		CHECK(f.level[ST_LED_S1] == 0u && f.level[ST_LED_S2] == 0u &&
		      f.level[ST_LED_S3] == 0u,
		      "playback outranks charging: S1-S3 off");
		CHECK(f.level[ST_LED_S4] == in.beat.envelope, "only S4 pulses with tempo");
	}
}

/* ========================== 7. transfer ================================== */
static void case_transfer(void)
{
	st_led_inputs_t in; st_led_frame_t f;

	g_cases++;
	printf("\n-- Transfer\n");
	memset(&in, 0, sizeof(in));
	in.song_selected = true;
	in.transfer_active = true;
	in.batt_state = ST_LED_BATT_CHARGING;   /* would otherwise paint the side row */
	in.batt_level = 3u;

	in.transfer_blink_on = true;
	in.batt_blink_on = true;
	st_led_mvp_decide(&in, &f);
	show("transfer, blink on", &f);
	CHECK(tracks_all(&f, ST_LED_MAX), "all four Track LEDs blink together");
	CHECK(f.level[ST_LED_S1] == ST_LED_MAX && f.level[ST_LED_S2] == ST_LED_MAX &&
	      f.level[ST_LED_S3] == ST_LED_MAX && f.level[ST_LED_S4] == 0u,
	      "the side row KEEPS the battery gauge through a transfer (S1-S3 at level 3) -- "
	      "the four blinking Track LEDs are the transfer indication, and a transfer is a "
	      "powered-on not-playing state like any other");

	in.transfer_blink_on = false;
	st_led_mvp_decide(&in, &f);
	show("transfer, blink off", &f);
	CHECK(tracks_all(&f, 0u), "and go dark together");
	CHECK(f.level[ST_LED_S1] == ST_LED_MAX && f.level[ST_LED_S3] == ST_LED_MAX,
	      "the gauge does NOT blink with the Track row -- the two are independent");

	/* Charging through a transfer: the current step keeps blinking. ADC
	 * sampling is paused during a transfer, so this is the STICKY last
	 * trustworthy reading being displayed, which is the intent. */
	in.batt_blink_on = false;
	st_led_mvp_decide(&in, &f);
	show("transfer, charging off-phase", &f);
	CHECK(f.level[ST_LED_S3] == 0u && f.level[ST_LED_S2] == ST_LED_MAX,
	      "mid-transfer the charging step S3 still blinks while completed steps stay "
	      "solid -- charge progress is not frozen by the transfer");

	/* Not charging, mid-transfer: a continuous, non-blinking gauge. */
	in.batt_state = ST_LED_BATT_CHARGER_ABSENT;
	st_led_mvp_decide(&in, &f);
	show("transfer, on battery", &f);
	CHECK(side_gauge(&f, 3), "on battery mid-transfer the gauge is solid S1-S3, no blink");

	/* An untrusted reading is still never fabricated, transfer or not. */
	in.batt_state = ST_LED_BATT_UNAVAILABLE;
	in.batt_level = 0u;
	st_led_mvp_decide(&in, &f);
	show("transfer, no reading", &f);
	CHECK(side_all(&f, 0u), "an untrusted gauge stays dark through a transfer too");
}

/* ======================= 8. power-off sequence =========================== */
static void case_shutdown(void)
{
	st_led_inputs_t in; st_led_frame_t f;

	g_cases++;
	printf("\n-- Power-off sequence\n");
	memset(&in, 0, sizeof(in));
	in.sequence = ST_LED_SEQ_SHUTDOWN;
	/* Deliberately assert states that must NOT show through: shutdown is
	 * the highest priority there is. */
	in.playing = true;
	in.song_selected = true;
	in.transfer_active = true;
	in.solo_mask = ST_LADDER_MASK_ALL;
	in.batt_state = ST_LED_BATT_CHARGING;
	in.batt_level = 2u;

	in.sequence_ms = 0u;
	st_led_mvp_decide(&in, &f);
	show("t=0 flash + blink", &f);
	CHECK(side_all(&f, ST_LED_MAX), "all four side LEDs flash together");
	CHECK(tracks_all(&f, ST_LED_MAX),
	      "all four Track LEDs blink once -- outranking playing, transfer AND solo");

	in.sequence_ms = ST_LED_TRACK_BLINK_MS;
	st_led_mvp_decide(&in, &f);
	CHECK(tracks_all(&f, 0u), "the Track blink ends dark");

	in.sequence_ms = ST_LED_SIDE_HOLD_MS + (ST_LED_SIDE_FADE_MS / 2u);
	st_led_mvp_decide(&in, &f);
	show("mid-fade", &f);
	CHECK(f.level[ST_LED_S1] > 0u && f.level[ST_LED_S1] < ST_LED_MAX,
	      "the side row fades rather than snapping off");

	in.sequence_ms = ST_LED_SHUTDOWN_TOTAL_MS;
	st_led_mvp_decide(&in, &f);
	show("sequence end", &f);
	CHECK(tracks_all(&f, 0u) && side_all(&f, 0u),
	      "the sequence ends with every LED dark, before SYSTEM_OFF may be entered");
}

/* ====================== 9. priority + invariants ========================= */
static void case_priority(void)
{
	st_led_inputs_t in; st_led_frame_t f;

	g_cases++;
	printf("\n-- Priority order and invariants\n");

	/* Boot outranks transfer and playing. */
	memset(&in, 0, sizeof(in));
	in.sequence = ST_LED_SEQ_BOOT;
	in.sequence_ms = 0u;
	in.transfer_active = true;
	in.playing = true;
	in.song_selected = true;
	st_led_mvp_decide(&in, &f);
	CHECK(side_all(&f, ST_LED_MAX),
	      "boot outranks transfer and playing (side row full at t=0)");

	/* Transfer outranks solo and playing. */
	memset(&in, 0, sizeof(in));
	in.transfer_active = true;
	in.transfer_blink_on = false;
	in.solo_mask = ST_LADDER_T1;
	in.playing = true;
	in.song_selected = true;
	st_led_mvp_decide(&in, &f);
	CHECK(tracks_all(&f, 0u), "transfer outranks solo (blink-off phase wins over T1)");

	/* No LED left unassigned. A zeroed input is the never-seeded gauge
	 * (batt_level 0, state UNAVAILABLE), so the correct frame really is
	 * all-dark -- and every one of the eight must be WRITTEN to get there,
	 * not merely left holding the 0xEE this pre-fills. */
	memset(&in, 0, sizeof(in));
	memset(&f, 0xEE, sizeof(f));
	st_led_mvp_decide(&in, &f);
	CHECK(side_all(&f, 0u) && tracks_all(&f, 0u),
	      "an unseeded, stopped device assigns all eight explicitly -- no LED carries a "
	      "previous value");

	/* Stopped with a TRUSTWORTHY gauge is the common case, and it must not
	 * be reachable by accident from a stale frame either. */
	memset(&in, 0, sizeof(in));
	in.batt_state = ST_LED_BATT_CHARGER_ABSENT;
	in.batt_level = 2u;
	memset(&f, 0xEE, sizeof(f));
	st_led_mvp_decide(&in, &f);
	CHECK(side_gauge(&f, 2) && tracks_all(&f, 0u),
	      "stopped at level 2 writes S1-S2 lit, S3-S4 dark and all four Track LEDs dark, "
	      "overwriting every stale byte");

	/* Purity. */
	{
		st_led_frame_t a, b;
		int n;

		make_playing(&in, 24000u + 1000u, 90u);
		st_led_mvp_decide(&in, &a);
		for (n = 0; n < 50; n++) {
			st_led_mvp_decide(&in, &b);
		}
		CHECK(memcmp(&a, &b, sizeof(a)) == 0,
		      "the decision is pure: 50 identical calls give an identical frame");
	}
}

int main(void)
{
	printf("STEM TAPE LED ACCEPTANCE MATRIX (st13 behaviour)\n");
	printf("driving the REAL st_led_mvp_decide() and the REAL st_beat_pulse()\n");
	printf("T[..] = Track 1-4 levels; S[..] = side S1-S4 levels, 0..255\n");
	printf("st13: the side row shows the battery gauge CONTINUOUSLY whenever the\n"
	       "      device is powered on and not playing -- stopped, no song, mid-\n"
	       "      transfer, soloing, charging or full. Only an untrusted reading\n"
	       "      darkens it. Playing is the one state that takes the row over.\n");

	case_boot();
	case_preview_levels();
	case_stopped_gauge();
	case_playing();
	case_loop_chase();
	case_fx_overlay();
	case_solo();
	case_solo_chords();
	case_charging();
	case_transfer();
	case_shutdown();
	case_priority();

	printf("\n%s (%d cases, %d checks, %d failures)\n",
	       g_failures ? "LED ACCEPTANCE MATRIX FAILED" : "LED ACCEPTANCE MATRIX PASSED",
	       g_cases, g_checks, g_failures);
	printf("NOTE: this proves the production DECISION assigns these levels. It does NOT\n"
	       "      prove the device looks right -- brightness and fade timing must be\n"
	       "      verified physically on real hardware.\n");
	return g_failures ? 1 : 0;
}
