/*
 * test_led_audio_reactive.c -- the Track LEDs follow the AUDIO, not the clock.
 *
 * ======================================================================
 * THE CLAIM UNDER TEST
 * ======================================================================
 * Each of the four Track lights is a very small VU meter for its own stem.
 * The brief drew the line itself:
 *
 *     stem audio -> activity/envelope detection -> LED brightness
 *
 *     NOT
 *
 *     tempo clock -> blink LED
 *
 * and named the failure that must not survive: "There should NOT be an
 * artificial flash on every quarter note, eighth note, beat, or bar unless
 * the audio itself causes that response."
 *
 * So this file does not check that a function was called. It synthesises real
 * stem material -- silence, a sustained note, isolated drum hits, a vocal with
 * phrases and gaps -- pushes it through the SAME peak extraction, the SAME
 * envelope follower and the SAME st_led_mvp_decide() the firmware runs, WITH A
 * VALID TEMPO RUNNING THROUGHOUT, and measures the light that comes out.
 *
 * The tempo is the point. Every case below has a real BPM and a real beat
 * pulse available; if any light still tracks it, the implementation has failed
 * whatever else it does. Case 6 measures that correlation directly.
 *
 * ======================================================================
 * WHAT IS AND IS NOT MIRRORED
 * ======================================================================
 * The rig reproduces main.c's data path exactly: per-block peak of each stem
 * separately, peak-HELD across blocks, read-and-cleared by the LED service at
 * its own slower interval, into one st_stem_meter_t per stem. It never sums
 * the stems -- an implementation that metered the mix would give all four
 * lights one value, and case 5 is what catches that.
 *
 * Build (from the repo root):
 *   cc -std=c11 -Wall -Wextra -Werror -Ifirmware/stemtape_player/src \
 *      firmware/stemtape_player/src/st_stem_meter.c \
 *      firmware/stemtape_player/src/st_beat_phase.c \
 *      firmware/stemtape_player/src/st_led_mvp.c \
 *      firmware/stemtape_player/tests/test_led_audio_reactive.c \
 *      -lm -o test_led_audio_reactive && ./test_led_audio_reactive
 */

#include <math.h>

/* -std=c11 is strict ISO, which does not expose M_PI. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "st_beat_phase.h"
#include "st_led_mvp.h"
#include "st_stem_meter.h"
#include "st_v11_format.h"   /* ST11_PCM_BIT_DEPTH -- the material's domain */

static int g_cases, g_checks, g_failures;

#define CHECK(cond, fmt, ...)                                                  \
	do {                                                                   \
		g_checks++;                                                    \
		if (!(cond)) {                                                 \
			g_failures++;                                          \
			printf("  FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,  \
			       ##__VA_ARGS__);                                 \
		}                                                              \
	} while (0)

#define SR          48000u
#define BLK_FRAMES  256u          /* one audio block, 5.33 ms */
#define LED_MS      10u           /* the control thread's LED service period */
#define BPM         120u
#define STEMS       4u

/* 120 BPM: one beat is 24000 frames, one bar 96000. */
#define FRAMES_PER_BEAT (SR * 60u / BPM)

/* ======================================================================
 * THE MATERIAL. Four generators, each a function of absolute song frame,
 * returning a magnitude in the STORED sample domain -- the same domain
 * stem_render_run() meters and st_stem_meter.c is calibrated against.
 * Deliberately written as plain waveform maths rather than canned tables so
 * the shapes are visible and adjustable here.
 *
 * EVERY LEVEL BELOW IS A FRACTION OF FULL SCALE, NOT A MAGNITUDE. It used to
 * be 24-bit magnitudes, and when the stored width moved to 16 bits the whole
 * programme became 48 dB of material sitting under an 8-count noise floor.
 * The fractions are stated against the 24-bit numbers they replace so the
 * levels this gate asserts are unchanged in decibels at any width.
 * ====================================================================== */
#define FS_MAG ((double)((1u << (ST11_PCM_BIT_DEPTH - 1u)) - 1u))
#define AMP (FS_MAG * (4000000.0 / 8388608.0))   /* about -6 dBFS */

/*
 * SILENCE, as it really arrives: the residual noise floor of a recorded and
 * encoded stem, not a mathematical zero.
 *
 * This matters. A generator that returns exactly 0.0 would let ANY
 * implementation pass the silence case, including one with no noise gate at
 * all -- zero in gives zero out however the curve is shaped. Real stored
 * material has dither and encoder noise sitting a long way down but not at
 * nothing, and the gate is what turns that into a dark LED instead of a
 * permanently faintly-lit one. So the "silent" stem here sits just UNDER
 * ST_STEM_METER_FLOOR, which is the only material that tests the gate.
 */
#define HISS_MAG (FS_MAG * (1024.0 / 8388608.0))   /* -78 dBFS, see below */
static double gen_silence(uint32_t f)
{
	/*
	 * The level is a fixed fraction of FULL SCALE and NOT derived from
	 * ST_STEM_METER_FLOOR, which is the whole point. Writing it as
	 * FLOOR/2 was the first attempt and it made the case worthless:
	 * lowering the floor to zero to break the gate also lowered this
	 * material to zero, so the test moved with the bug and passed. The
	 * material has to be fixed while the constant varies -- and a fraction
	 * of full scale still is: the floor can be moved anywhere without
	 * moving this. What it is no longer fixed against is the STORED WIDTH,
	 * because a hiss floor is a level in dBFS, not a count of LSBs, and
	 * pinning it to 24-bit counts is what broke it at v1.3. main() asserts
	 * the two are still in the right order.
	 */
	return HISS_MAG * sin(2.0 * M_PI * 3000.0 * f / SR);
}

/*
 * QUIET BUT PRESENT: real audio about 18 dB below the meter's reference. The
 * brief's own warning is what this exists for -- "a linear amplitude-to-
 * brightness mapping may make LEDs appear too dim most of the time" -- and
 * this is the level at which a linear map collapses to near-black while a
 * logarithmic one still shows a clearly readable mid brightness.
 */
static double gen_quiet(uint32_t f)
{
	return AMP * 0.125 * 0.6 * sin(2.0 * M_PI * 440.0 * f / SR);
}

/* HALF WAY BETWEEN the loud and quiet stems: 9 dB under one, 9 dB over the
 * other. The three together are an evenly spaced ladder in decibels, which is
 * what TEST 5b measures the curve against. */
static double gen_mid(uint32_t f)
{
	return AMP * 0.354 * 0.6 * sin(2.0 * M_PI * 330.0 * f / SR);
}

/*
 * A CONSTANT TONE. No attacks, no decay, no dynamics at all -- an organ note
 * held forever. Used for the cases that need a level to hold still: the
 * "sustained material does not blink" check, and the on-beat/off-beat
 * comparison, which is only meaningful if the audio itself is not moving.
 *
 * It is NOT a stand-in for a piano or a bass. Those are articulated: they have
 * an attack and a decay on every note, and using a flat tone to represent them
 * was how the first version of this display came to look static and still
 * passed its own tests. gen_piano() and gen_bass() below are the honest
 * material, and case_expressive_animation() is what measures against them.
 */
static double gen_sustain(uint32_t f)
{
	return AMP * 0.6 * sin(2.0 * M_PI * 220.0 * f / SR);
}

/*
 * PIANO. A chord on every beat: sharp attack, then a long decay that is still
 * clearly sounding when the next chord lands. Continuously active -- the LED
 * must never go out -- but full of internal dynamics, which is exactly the
 * case the brief says currently looks like a solid light.
 */
static double gen_piano(uint32_t f)
{
	const uint32_t into = f % FRAMES_PER_BEAT;
	const double   t    = (double)into / SR;
	/* Long decay: ~35% of the chord is still ringing at the next beat. */
	const double   env  = exp(-t / 0.48);
	const double   tone = 0.6 * sin(2.0 * M_PI * 262.0 * f / SR) +
			       0.3 * sin(2.0 * M_PI * 330.0 * f / SR) +
			       0.3 * sin(2.0 * M_PI * 392.0 * f / SR);

	return AMP * 0.55 * env * tone;
}

/*
 * BASS. Notes on the half-beat, each swelling in and decaying -- a softer
 * attack than the piano and a fatter body, so it exercises the accent path
 * with something less percussive than a drum.
 */
static double gen_bass(uint32_t f)
{
	const uint32_t period = FRAMES_PER_BEAT * 2u;
	const uint32_t into   = f % period;
	const double   t      = (double)into / SR;
	const double   env    = (1.0 - exp(-t / 0.03)) * exp(-t / 0.55);

	return AMP * 0.7 * env * sin(2.0 * M_PI * 65.0 * f / SR);
}

/* PERCUSSION. Isolated hits on the beat, each an exponentially decaying
 * ~60 ms burst -- a hard transient followed by near-silence. The gaps between
 * hits are what the display has to show as gaps. */
static double gen_drums(uint32_t f)
{
	const uint32_t into = f % FRAMES_PER_BEAT;
	const double   t    = (double)into / SR;
	const double   env  = exp(-t / 0.02);

	if (t > 0.06) {
		return 0.0;
	}
	return AMP * env * sin(2.0 * M_PI * 90.0 * f / SR);
}

/*
 * VOCAL. Three syllables in quick succession -- "for me no" -- then a silent
 * gap of two beats, repeating. Each syllable is a raised-cosine burst, so the
 * envelope rises and falls the way a sung syllable does rather than switching.
 */
#define VOX_PERIOD (5u * FRAMES_PER_BEAT)
static double gen_vocal(uint32_t f)
{
	const uint32_t into = f % VOX_PERIOD;
	const uint32_t syl  = FRAMES_PER_BEAT * 3u / 4u;   /* syllable length */
	uint32_t s;

	for (s = 0; s < 3u; s++) {
		const uint32_t start = s * FRAMES_PER_BEAT;

		if (into >= start && into < start + syl) {
			const double x = (double)(into - start) / (double)syl;
			const double env = 0.5 - 0.5 * cos(2.0 * M_PI * x);

			return AMP * 0.8 * env *
			       sin(2.0 * M_PI * 300.0 * f / SR);
		}
	}
	return 0.0;   /* the two-beat gap between phrases */
}

typedef double (*gen_fn)(uint32_t);

/* ======================================================================
 * THE RIG. main.c's path, in the same order and the same domains.
 * ====================================================================== */
#define MAXSTEP 40000u

static uint8_t  g_led[MAXSTEP][STEMS];   /* the Track LED levels, per service */
static bool     g_pulse[MAXSTEP];        /* was the beat pulse lit that pass */
/*
 * Song time at each service, in ms. Recorded rather than inferred: a service
 * fires on the first block boundary at or past its due time, so the real
 * interval is a multiple of the 5.33 ms block (about 10.67 ms), not LED_MS.
 * Any case that windows on musical position has to use THIS, or the window
 * walks off the material within a couple of bars.
 */
static uint32_t g_ms[MAXSTEP];
static uint32_t g_nstep;

/*
 * Run `ms` of song through the whole chain and record every LED service.
 * `solo_mask` is a physically-held Track chord (0 == none).
 */
static void run(gen_fn gen[STEMS], uint32_t ms, uint8_t solo_mask)
{
	st_stem_meter_t meter[STEMS];
	st_beat_timing_t timing;
	uint32_t held[STEMS];        /* the peak-HOLD the audio thread publishes */
	uint32_t frame = 0, s;
	uint32_t next_led_ms = LED_MS, elapsed_ms = 0, last_led_ms = 0;

	g_nstep = 0;
	for (s = 0; s < STEMS; s++) {
		st_stem_meter_reset(&meter[s]);
		held[s] = 0u;
	}
	if (!st_beat_timing_init(&timing, BPM << 8, 0u, SR)) {
		printf("  FATAL: beat timing refused %u BPM\n", BPM);
		exit(2);
	}

	while (elapsed_ms < ms && g_nstep < MAXSTEP) {
		uint32_t k;

		/* ---- ONE AUDIO BLOCK. Per-stem peak, merged into the hold. */
		for (s = 0; s < STEMS; s++) {
			uint32_t peak = 0u;

			/* Sampled one frame in 32, exactly as stem_render_run()
			 * meters -- so the test cannot pass on a resolution the
			 * firmware does not actually have. */
			for (k = 0; k < BLK_FRAMES; k += 32u) {
				const double v = gen[s](frame + k);
				const uint32_t mag = (uint32_t)fabs(v);

				if (mag > peak) {
					peak = mag;
				}
			}
			if (peak > held[s]) {
				held[s] = peak;   /* peak HOLD, not last-wins */
			}
		}
		frame += BLK_FRAMES;
		elapsed_ms = (uint32_t)((uint64_t)frame * 1000u / SR);

		/* ---- THE LED SERVICE, at its own slower rate. */
		if (elapsed_ms >= next_led_ms) {
			st_led_inputs_t in;
			st_led_frame_t out;
			const uint32_t dt = elapsed_ms - last_led_ms;

			memset(&in, 0, sizeof(in));
			in.song_selected = true;
			in.playing       = true;
			in.solo_mask     = solo_mask;
			in.batt_state    = ST_LED_BATT_CHARGER_ABSENT;
			in.batt_level    = 3u;
			st_beat_pulse(&timing, frame, &in.beat);

			for (s = 0; s < STEMS; s++) {
				const uint32_t peak = held[s];

				held[s] = 0u;   /* READ AND CLEAR */
				st_stem_meter_update(&meter[s], peak, dt);
				in.stem_activity[s] =
					st_stem_meter_brightness(&meter[s]);
			}

			st_led_mvp_decide(&in, &out);
			for (s = 0; s < STEMS; s++) {
				g_led[g_nstep][s] = out.level[s];
			}
			g_pulse[g_nstep] = in.beat.valid && in.beat.in_pulse;
			g_ms[g_nstep] = elapsed_ms;
			g_nstep++;
			last_led_ms  = elapsed_ms;
			next_led_ms  = elapsed_ms + LED_MS;
		}
	}
}

/* ---- small summaries over the recorded run ---------------------------- */
static uint8_t peak_of(uint32_t stem)
{
	uint32_t i;
	uint8_t  hi = 0;

	for (i = 0; i < g_nstep; i++) {
		if (g_led[i][stem] > hi) hi = g_led[i][stem];
	}
	return hi;
}

static uint8_t min_of(uint32_t stem, uint32_t from, uint32_t to)
{
	uint32_t i;
	uint8_t  lo = 255;

	for (i = from; i < to && i < g_nstep; i++) {
		if (g_led[i][stem] < lo) lo = g_led[i][stem];
	}
	return lo;
}

static double mean_of(uint32_t stem, uint32_t from, uint32_t to)
{
	uint32_t i, n = 0;
	double   acc = 0.0;

	for (i = from; i < to && i < g_nstep; i++) {
		acc += g_led[i][stem];
		n++;
	}
	return n ? acc / n : 0.0;
}

/* How many services this stem spent completely dark. */
static uint32_t dark_count(uint32_t stem)
{
	uint32_t i, n = 0;

	for (i = 0; i < g_nstep; i++) {
		if (g_led[i][stem] == 0u) n++;
	}
	return n;
}

/*
 * Spread of one LED over a window: mean, standard deviation, and the range
 * actually travelled. The standard deviation is the number that matters --
 * peak-to-trough can be produced by a single glitch, but a large sigma means
 * the light is moving CONTINUOUSLY, which is what "alive" means to the eye.
 */
static void spread(uint32_t stem, uint32_t from, uint32_t to,
		    double *mean, double *sd, uint8_t *lo, uint8_t *hi)
{
	uint32_t i, n = 0;
	double   acc = 0.0, var = 0.0;

	*lo = 255; *hi = 0;
	for (i = from; i < to && i < g_nstep; i++) {
		const uint8_t v = g_led[i][stem];

		acc += v; n++;
		if (v < *lo) *lo = v;
		if (v > *hi) *hi = v;
	}
	*mean = n ? acc / n : 0.0;
	for (i = from; i < to && i < g_nstep; i++) {
		const double d = g_led[i][stem] - *mean;

		var += d * d;
	}
	*sd = n ? sqrt(var / n) : 0.0;
}

/* ======================================================================
 * TEST 1: SILENCE. A silent stem is dark for the whole run -- and the
 * tempo is running the entire time, so this is also the first and
 * bluntest proof that the clock does not reach the light.
 * ====================================================================== */
static void case_silence(void)
{
	gen_fn g[STEMS] = { gen_silence, gen_sustain, gen_sustain, gen_sustain };
	uint32_t pulses = 0, i;

	g_cases++;
	printf("\n-- TEST 1: a silent stem stays dark, beats or no beats\n");

	run(g, 4000u, 0u);
	for (i = 0; i < g_nstep; i++) {
		if (g_pulse[i]) pulses++;
	}
	printf("     %u LED services over 4 s, %u of them on a beat pulse\n",
	       g_nstep, pulses);
	CHECK(pulses > 20u,
	      "the tempo must actually be running for this to prove anything "
	      "(only %u pulses seen)", pulses);
	CHECK(peak_of(0) == 0u,
	      "the silent stem reached brightness %u -- it must be 0 for the "
	      "whole run", peak_of(0));
	CHECK(peak_of(1) > 0u,
	      "sanity: the sounding stems must not also be dark");
}

/* ======================================================================
 * TEST 2: SUSTAINED TONE. The light stays up across beats. This is the
 * case that a beat-driven display fails outright: it would go dark in
 * every gap between pulses (3/4 of every beat, by ST_BEAT_PULSE_DEN).
 * ====================================================================== */
static void case_sustained(void)
{
	gen_fn g[STEMS] = { gen_sustain, gen_silence, gen_silence, gen_silence };
	uint32_t settle;

	g_cases++;
	printf("\n-- TEST 2: a sustained note stays lit, it does not blink\n");

	run(g, 3000u, 0u);
	settle = g_nstep / 10u;   /* skip the initial rise */

	printf("     after settling: min %u, mean %.1f, peak %u\n",
	       min_of(0, settle, g_nstep), mean_of(0, settle, g_nstep),
	       peak_of(0));
	CHECK(min_of(0, settle, g_nstep) > 0u,
	      "the sustained stem went fully dark at some point (min %u) -- a "
	      "sustained note must not blink", min_of(0, settle, g_nstep));
	CHECK(dark_count(0) < g_nstep / 8u,
	      "the sustained stem was dark for %u of %u services",
	      dark_count(0), g_nstep);
	/*
	 * THE DECISIVE NUMBER. A beat-gated row is dark for three quarters of
	 * every beat (ST_BEAT_PULSE_NUM/DEN = 1/4), so its mean would sit near
	 * a quarter of its peak. A level display holds near its peak.
	 */
	{
		/*
		 * MEASURED AFTER SETTLING, both of them. A tone starting from
		 * silence IS an attack, and the accent path is supposed to pop
		 * on it -- comparing the settled mean against that onset peak
		 * was measuring the feature and calling it a fault. What this
		 * case is really about is that once the tone is established
		 * and unchanging, the light stops moving; so the peak it is
		 * compared against has to come from the same settled window.
		 */
		double mean, sd;
		uint8_t lo, hi;

		spread(0, settle, g_nstep, &mean, &sd, &lo, &hi);
		printf("     settled window: %u..%u, sd %.2f\n", lo, hi, sd);
		CHECK(mean > 0.8 * (double)hi,
		      "mean %.1f against a settled peak of %u: that ratio is a "
		      "pulse, not a sustained level", mean, hi);
		CHECK(sd < 4.0,
		      "a genuinely unchanging tone still wobbles by %.2f -- the "
		      "movement must come from the audio, and this audio has "
		      "none", sd);
	}
}

/* ======================================================================
 * TEST 3: PERCUSSION. Each hit rises fast and decays smoothly, and the
 * gaps between hits are visible.
 * ====================================================================== */
static void case_percussion(void)
{
	gen_fn g[STEMS] = { gen_drums, gen_silence, gen_silence, gen_silence };
	uint32_t i, rises = 0, falls = 0;
	uint8_t  hi, lo;

	g_cases++;
	printf("\n-- TEST 3: drum hits punch and decay\n");

	run(g, 3000u, 0u);
	hi = peak_of(0);
	lo = min_of(0, g_nstep / 10u, g_nstep);

	for (i = 1; i < g_nstep; i++) {
		if (g_led[i][0] > g_led[i - 1][0] + 20u) rises++;
		if (g_led[i][0] + 2u < g_led[i - 1][0])  falls++;
	}
	printf("     peak %u, trough %u, %u sharp rises, %u decay steps over "
	       "%u services\n", hi, lo, rises, falls, g_nstep);
	CHECK(hi > 150u, "a drum hit only reached %u -- too dim to read", hi);
	CHECK(rises >= 4u,
	      "only %u sharp rises in 3 s at 120 BPM: the hits are not "
	      "producing distinct punches", rises);
	/* A smooth decay means several small downward steps per hit, not one
	 * cliff. The bar is per-hit rather than a fixed count because the
	 * release time is a tuning knob: shortening it legitimately produces
	 * fewer intermediate steps, and this must keep measuring "is there a
	 * staircase" rather than "is the release still 250 ms". */
	CHECK(falls > rises * 3u,
	      "%u decay steps across %u hits -- under three steps per hit is a "
	      "cliff, not a decay", falls, rises);
	CHECK(lo < hi / 2u,
	      "the trough between hits (%u) is not clearly below the peak (%u); "
	      "the gaps have to be visible", lo, hi);
}

/* ======================================================================
 * TEST 4: VOCAL. Three syllables, then a gap, and the light follows both.
 * ====================================================================== */
static void case_vocal(void)
{
	gen_fn g[STEMS] = { gen_vocal, gen_silence, gen_silence, gen_silence };
	uint32_t period_ms, voice_ends_ms, i, cyc;
	double   in_phrase = 0.0, in_gap = 0.0;
	uint32_t n_phrase = 0, n_gap = 0;

	g_cases++;
	printf("\n-- TEST 4: the vocal light follows phrases and rests\n");

	run(g, 6000u, 0u);
	/*
	 * One vocal cycle is 5 beats. The three syllables start on beats 0, 1
	 * and 2 and each lasts 3/4 of a beat, so the LAST SOUND ends partway
	 * through beat 2 -- not at the end of beat 2. Windowing on "three
	 * beats" would put 125 ms of real silence inside the phrase window and
	 * start the rest late; the boundary has to be where the audio actually
	 * stops.
	 */
	period_ms      = (VOX_PERIOD * 1000u) / SR;
	voice_ends_ms  = ((2u * FRAMES_PER_BEAT + FRAMES_PER_BEAT * 3u / 4u) *
			   1000u) / SR;

	/*
	 * TWO WINDOWS, and the distinction matters.
	 *
	 * `in_phrase` is the three syllables. `in_gap` is the LAST THIRD of the
	 * two-beat rest -- not the whole rest. The earlier part of a rest is
	 * the release envelope still falling, which is exactly what it is
	 * supposed to be doing and is the difference between a tape-like decay
	 * and a gate slamming shut. What the brief asks for is that the light
	 * "become inactive during those two beats", so the question is where
	 * the level has GOT TO by the end of the rest, not what it averaged on
	 * the way down.
	 */
	{
		const uint32_t tail_start = voice_ends_ms +
					     ((period_ms - voice_ends_ms) * 2u) / 3u;

		for (i = 0; i < g_nstep; i++) {
			const uint32_t into = g_ms[i] % period_ms;

			if (into < voice_ends_ms) {
				in_phrase += g_led[i][0];
				n_phrase++;
			} else if (into >= tail_start) {
				in_gap += g_led[i][0];
				n_gap++;
			}
		}
	}
	in_phrase /= (n_phrase ? n_phrase : 1u);
	in_gap    /= (n_gap ? n_gap : 1u);
	cyc = (g_nstep ? g_ms[g_nstep - 1u] : 0u) / period_ms;

	printf("     %u phrases: mean %.1f while the words sound, %.1f by the "
	       "end of the rest\n", cyc, in_phrase, in_gap);
	CHECK(cyc >= 2u, "only %u vocal cycles ran; need at least 2", cyc);
	CHECK(in_phrase > 100.0,
	      "the vocal only averaged %.1f while words were sounding",
	      in_phrase);
	/* "Inactive" means at the bottom of the visible range -- the meter's
	 * own minimum-visible step, not merely dimmer than the words. */
	CHECK(in_gap < (double)ST_STEM_METER_MIN_ON * 2.0,
	      "by the end of a two-beat rest the vocal light still averaged "
	      "%.1f (minimum visible is %u) -- it has not gone inactive",
	      in_gap, ST_STEM_METER_MIN_ON);
	CHECK(in_gap < in_phrase / 4.0,
	      "the rest (%.1f) is not clearly below the words (%.1f)",
	      in_gap, in_phrase);
}

/* ======================================================================
 * TEST 5: FOUR STEMS TOGETHER. Drums, piano, bass and vocals at once, and
 * the four lights must NOT move together. This is also what catches an
 * implementation that metered the mix and fed one value to all four.
 * ====================================================================== */
static void case_four_stems_are_independent(void)
{
	gen_fn g[STEMS] = { gen_drums, gen_sustain, gen_sustain, gen_vocal };
	uint32_t i, identical = 0, drums_v_vox_diff = 0;

	g_cases++;
	printf("\n-- TEST 5: four stems, four different lights\n");

	run(g, 4000u, 0u);
	for (i = 0; i < g_nstep; i++) {
		if (g_led[i][0] == g_led[i][1] && g_led[i][1] == g_led[i][2] &&
		    g_led[i][2] == g_led[i][3]) {
			identical++;
		}
		if (g_led[i][0] != g_led[i][3]) {
			drums_v_vox_diff++;
		}
	}
	printf("     drums %.1f | piano %.1f | bass %.1f | vocals %.1f "
	       "(mean level)\n",
	       mean_of(0, 0, g_nstep), mean_of(1, 0, g_nstep),
	       mean_of(2, 0, g_nstep), mean_of(3, 0, g_nstep));
	printf("     all four identical on %u of %u services\n",
	       identical, g_nstep);
	CHECK(identical < g_nstep / 10u,
	      "all four lights read the same on %u of %u services -- they are "
	      "not independent", identical, g_nstep);
	CHECK(drums_v_vox_diff > (g_nstep * 3u) / 4u,
	      "the drum and vocal lights agreed too often (%u of %u differ)",
	      drums_v_vox_diff, g_nstep);
	/* The two sustained stems SHOULD look alike -- they are the same
	 * material. That is the control: difference must come from the audio,
	 * not from the stems being arbitrarily decorrelated. */
	CHECK(fabs(mean_of(1, 0, g_nstep) - mean_of(2, 0, g_nstep)) < 8.0,
	      "the two identical sustained stems read differently (%.1f vs "
	      "%.1f) -- the difference must come from the audio",
	      mean_of(1, 0, g_nstep), mean_of(2, 0, g_nstep));
}

/* ======================================================================
 * TEST 5b: THE CURVE IS LOGARITHMIC, MEASURED AS SUCH.
 *
 * The brief asked for four distinguishable states -- "silence = LED off /
 * quiet = dim / normal = medium / strong = bright" -- and warned why a linear
 * map does not deliver them: music lives in the top 20-30 dB of the scale, so
 * a linear map crowds nearly everything into the bottom of the range and the
 * row reads as on/off.
 *
 * The property that actually produces those four states is that EQUAL STEPS
 * IN DECIBELS PRODUCE EQUAL STEPS IN BRIGHTNESS. So three stems are run 9 dB
 * apart and the two brightness gaps between them are compared. A logarithmic
 * curve makes those gaps the same size; a linear one makes the top gap
 * several times the bottom one, which is the same thing as saying it wastes
 * most of its range on the loudest few dB.
 *
 * Stated this way the case does not depend on any particular constant, so
 * retuning the sensitivity or the span by eye on hardware cannot break it.
 * ====================================================================== */
static void case_curve_is_logarithmic(void)
{
	gen_fn g[STEMS] = { gen_sustain, gen_mid, gen_quiet, gen_silence };
	double loud, mid, quiet, hi_step, lo_step, ratio;
	uint32_t settle;

	g_cases++;
	printf("\n-- TEST 5b: equal steps in dB are equal steps in brightness\n");

	run(g, 2000u, 0u);
	settle = g_nstep / 4u;
	loud  = mean_of(0, settle, g_nstep);
	mid   = mean_of(1, settle, g_nstep);
	quiet = mean_of(2, settle, g_nstep);

	hi_step = loud - mid;
	lo_step = mid - quiet;
	ratio   = (lo_step > 0.0) ? (hi_step / lo_step) : 999.0;

	printf("     0 dB %.1f | -9 dB %.1f | -18 dB %.1f  (steps %.1f and "
	       "%.1f, ratio %.2f)\n", loud, mid, quiet, hi_step, lo_step,
	       ratio);
	printf("     sub-floor noise reads %.1f\n",
	       mean_of(3, settle, g_nstep));

	CHECK(mean_of(3, settle, g_nstep) == 0.0,
	      "sub-floor noise lit the LED to %.1f -- the noise gate is not "
	      "holding", mean_of(3, settle, g_nstep));
	CHECK(hi_step > 20.0 && lo_step > 20.0,
	      "9 dB should be a clearly visible step; got %.1f and %.1f",
	      hi_step, lo_step);
	CHECK(ratio > 0.6 && ratio < 1.7,
	      "the two 9 dB steps differ by %.2fx (%.1f vs %.1f) -- the curve "
	      "is not logarithmic, so most of the brightness range is being "
	      "spent on the loudest few dB", ratio, hi_step, lo_step);
	/*
	 * THE THRESHOLD HERE WAS LOWERED, DELIBERATELY. It used to demand that
	 * material 18 dB down read at twice the minimum visible step, which
	 * followed from a 30 dB body window. The window is now 24 dB and the
	 * body is capped at under half the brightness range, both on purpose:
	 * the brief asked for quiet moments to "visibly fall back" and for
	 * sustained material not to sit near the top.
	 *
	 * Quiet material is therefore expected to sit LOW -- and it is not
	 * expected to sit STILL, because its animation now comes from the
	 * accent rather than from the body. case_modest_accent_is_visible()
	 * is the case that guarantees that, at a level even quieter than this
	 * one.
	 */
	CHECK(quiet > (double)ST_STEM_METER_MIN_ON,
	      "material 18 dB down reads %.1f, at or under the minimum visible "
	      "step of %u -- quiet should be dim, not off", quiet,
	      ST_STEM_METER_MIN_ON);
}

/* ======================================================================
 * TEST 5c: THE ACCEPTANCE TEST -- COVER THREE LEDS AND WATCH THE FOURTH.
 *
 * The brief's own criterion: "If I cover the other three LEDs, the remaining
 * LED should still visibly animate throughout the performance. Even a
 * relatively continuous piano or bass stem should not look like a static
 * light."
 *
 * So each stem is measured ALONE, against articulated material -- a piano
 * chording on every beat, a bass note every half bar, drums, a vocal -- and
 * three things are required of every one of them:
 *
 *   it never goes out       (the stem is continuously active)
 *   it moves a lot          (large travel AND large standard deviation, so
 *                            the movement is continuous rather than one jump)
 *   it is not pinned high   (a light sitting at 90% has nowhere to animate,
 *                            which was the first version's real failure)
 *
 * The standard deviation is the honest measure of "looks alive". A display
 * that sat at a constant 60% would pass a peak-to-trough check on one stray
 * sample; it cannot pass a sigma check.
 * ====================================================================== */
static void case_expressive_animation(void)
{
	static const char *names[STEMS] = { "drums", "piano", "bass", "vocals" };
	gen_fn g[STEMS] = { gen_drums, gen_piano, gen_bass, gen_vocal };
	uint32_t s, settle;

	g_cases++;
	printf("\n-- TEST 5c: each stem alone must still visibly animate\n");

	run(g, 6000u, 0u);
	settle = g_nstep / 10u;

	printf("     stem      mean    sd   range      travel\n");
	for (s = 0; s < STEMS; s++) {
		double mean, sd;
		uint8_t lo, hi;

		spread(s, settle, g_nstep, &mean, &sd, &lo, &hi);
		printf("     %-8s  %5.1f  %4.1f  %3u..%-3u  %3u\n",
		       names[s], mean, sd, lo, hi, (unsigned)(hi - lo));

		CHECK((unsigned)(hi - lo) > 70u,
		      "%s travels only %u of 255 -- that reads as a static "
		      "light", names[s], (unsigned)(hi - lo));
		CHECK(sd > 18.0,
		      "%s has a standard deviation of only %.1f: it may reach "
		      "two levels but it is not continuously moving",
		      names[s], sd);
		/* ANTI-SATURATION. This is the one that catches "everything
		 * looks solid because everything is at 90%". */
		CHECK(mean < 190.0,
		      "%s sits at a mean of %.1f of 255 -- too high to have "
		      "anywhere left to animate", names[s], mean);
		CHECK(mean > 40.0,
		      "%s sits at a mean of %.1f -- too dim to read", names[s],
		      mean);
	}
}

/* ======================================================================
 * TEST 5d: A NOTE ATTACK PRODUCES A VISIBLE POP ABOVE THE GLOW.
 *
 * The mechanism, measured directly rather than inferred from the spread: for
 * a stem that is already sounding, a new attack must push the light
 * substantially ABOVE where it was sitting a moment earlier. That is the
 * body-plus-accent design working -- and a single-envelope display, which is
 * what this replaced, cannot do it, because with the glow already up at the
 * sustained level there is nothing left for the attack to add.
 * ====================================================================== */
static void case_attacks_pop_above_the_glow(void)
{
	gen_fn g[STEMS] = { gen_piano, gen_silence, gen_silence, gen_silence };
	uint32_t i, pops = 0, beats;
	double   trough_sum = 0.0, peak_sum = 0.0;
	uint32_t n_win = 0;
	const uint32_t beat_ms = 60000u / BPM;

	g_cases++;
	printf("\n-- TEST 5d: a new chord pops above the ringing one\n");

	run(g, 6000u, 0u);
	beats = (g_nstep ? g_ms[g_nstep - 1u] : 0u) / beat_ms;

	/* For each beat after the first, compare the light just BEFORE the
	 * chord (the ringing tail of the previous one) with its highest point
	 * just after. */
	for (i = 2u; i < beats; i++) {
		const uint32_t at = i * beat_ms;
		uint8_t before = 0, after = 0;
		uint32_t k;

		for (k = 0; k < g_nstep; k++) {
			if (g_ms[k] + 12u >= at && g_ms[k] < at) {
				before = g_led[k][0];
			}
			if (g_ms[k] >= at && g_ms[k] < at + 90u &&
			    g_led[k][0] > after) {
				after = g_led[k][0];
			}
		}
		if (before == 0u && after == 0u) {
			continue;
		}
		trough_sum += before;
		peak_sum   += after;
		n_win++;
		if (after > before + 40u) {
			pops++;
		}
	}
	trough_sum /= (n_win ? n_win : 1u);
	peak_sum   /= (n_win ? n_win : 1u);

	printf("     %u chords: glow %.1f just before, %.1f at the attack "
	       "(+%.1f), %u clear pops\n",
	       n_win, trough_sum, peak_sum, peak_sum - trough_sum, pops);
	CHECK(n_win >= 6u, "only %u chord windows measured", n_win);
	CHECK(pops >= n_win - 1u,
	      "only %u of %u chords produced a clear pop above the ringing "
	      "glow", pops, n_win);
	CHECK(peak_sum - trough_sum > 50.0,
	      "the average attack only lifts the light by %.1f of 255 -- an "
	      "accent has to read as an event", peak_sum - trough_sum);
	CHECK(trough_sum > (double)ST_STEM_METER_MIN_ON,
	      "the glow between chords fell to %.1f -- the piano is still "
	      "ringing and the light should show it", trough_sum);
}

/* ======================================================================
 * TEST 5e: A MODEST ACCENT IS VISIBLE, wherever it sits in the range.
 *
 * The brief: "If a stem goes 0.30 -> 0.45, that musical accent may deserve a
 * visible pulse even though neither value is particularly loud compared with
 * the maximum possible signal."
 *
 * That is a 3.5 dB step at a level nowhere near full scale, and it is exactly
 * what an absolute-level display cannot show -- both values map to almost the
 * same place on the curve. Measured here at a QUIET absolute level on purpose,
 * so that only a relative detector can pass it.
 * ====================================================================== */
static void case_modest_accent_is_visible(void)
{
	st_stem_meter_t m;
	uint8_t steady, accented = 0;
	uint32_t i;
	/* Both levels sit well down the range: 0.30 and 0.45 of a signal that
	 * is itself 18 dB below the meter's reference. */
	const uint32_t quiet_base = (uint32_t)(ST_STEM_METER_REF * 0.125 * 0.30);
	const uint32_t quiet_acc  = (uint32_t)(ST_STEM_METER_REF * 0.125 * 0.45);

	g_cases++;
	printf("\n-- TEST 5e: a 0.30 -> 0.45 accent is visible even when quiet\n");

	st_stem_meter_reset(&m);
	for (i = 0; i < 200u; i++) {          /* settle at the steady level */
		st_stem_meter_update(&m, quiet_base, LED_MS);
	}
	steady = st_stem_meter_brightness(&m);
	for (i = 0; i < 6u; i++) {            /* the accent arrives */
		st_stem_meter_update(&m, quiet_acc, LED_MS);
		if (st_stem_meter_brightness(&m) > accented) {
			accented = st_stem_meter_brightness(&m);
		}
	}
	printf("     steady %u -> accented %u (+%u)\n", steady, accented,
	       (unsigned)(accented - steady));
	CHECK(accented > steady + 25u,
	      "a 3.5 dB accent moved the light from %u to %u: not a visible "
	      "pulse", steady, accented);
	CHECK(steady < 170u,
	      "quiet material already sits at %u -- there is no room above it "
	      "for an accent", steady);
}

/* ======================================================================
 * TEST 6: THE LIGHT IS NOT THE CLOCK.
 *
 * Every case above runs with a live tempo. This one measures the thing
 * directly: for a stem whose audio is perfectly steady, compare the mean
 * LED level on beat-pulse services against off-pulse ones. A tempo-driven
 * row separates them completely. An audio-driven row cannot tell them
 * apart, because the audio cannot.
 * ====================================================================== */
static void case_not_a_beat_indicator(void)
{
	gen_fn g[STEMS] = { gen_sustain, gen_sustain, gen_sustain, gen_sustain };
	uint32_t i, n_on = 0, n_off = 0, settle;
	double   on = 0.0, off = 0.0;

	g_cases++;
	printf("\n-- TEST 6: the row does not follow the beat\n");

	run(g, 4000u, 0u);
	settle = g_nstep / 10u;
	for (i = settle; i < g_nstep; i++) {
		if (g_pulse[i]) { on  += g_led[i][0]; n_on++; }
		else            { off += g_led[i][0]; n_off++; }
	}
	on  /= (n_on  ? n_on  : 1u);
	off /= (n_off ? n_off : 1u);

	printf("     steady audio: %.1f on the beat, %.1f off the beat "
	       "(%u/%u services)\n", on, off, n_on, n_off);
	CHECK(n_on > 10u && n_off > 10u,
	      "need both on-beat and off-beat samples (%u/%u)", n_on, n_off);
	CHECK(off > 0.0,
	      "the light is FULLY DARK off the beat: that is a beat indicator, "
	      "which is exactly what this must not be");
	CHECK(fabs(on - off) < 0.05 * (on > off ? on : off),
	      "on-beat %.1f vs off-beat %.1f for perfectly steady audio -- the "
	      "beat is reaching the Track row", on, off);

	/* And the side row must STILL show the tempo: removing the beat from
	 * the Track row is not the same as removing it from the device. */
	{
		st_led_inputs_t in;
		st_led_frame_t out;
		st_beat_timing_t t;
		uint32_t f, lit = 0;

		memset(&in, 0, sizeof(in));
		in.song_selected = true;
		in.playing = true;
		st_beat_timing_init(&t, BPM << 8, 0u, SR);
		for (f = 0; f < FRAMES_PER_BEAT * 4u; f += 512u) {
			st_beat_pulse(&t, f, &in.beat);
			st_led_mvp_decide(&in, &out);
			if (out.level[ST_LED_S4] > 0u) lit++;
		}
		CHECK(lit > 0u,
		      "S4 no longer shows the beat -- the tempo indication was "
		      "removed from the device, not just from the Track row");
		printf("     S4 still carries the beat (%u lit samples)\n", lit);
	}
}

/* ======================================================================
 * TEST 7: A HELD TRACK BUTTON STILL CONFIRMS ITSELF.
 *
 * Momentary solo overrides the Track row with an unambiguous held/not-held
 * readout, and that is left exactly as it shipped: it is button feedback
 * for as long as a finger is down, not a playback display. Asserted here so
 * that making the row audio-reactive cannot quietly swallow it -- a solo
 * that showed the audio could read dark at the instant the stem is silent,
 * which is a button that appears not to have worked.
 * ====================================================================== */
static void case_solo_still_confirms(void)
{
	gen_fn g[STEMS] = { gen_silence, gen_sustain, gen_sustain, gen_sustain };
	uint32_t i, wrong = 0;

	g_cases++;
	printf("\n-- TEST 7: holding a Track button still reads as held\n");

	/* Stem 0 is SILENT and soloed: the worst case for the override. */
	run(g, 1000u, 0x1u);
	for (i = 0; i < g_nstep; i++) {
		if (g_led[i][0] != ST_LED_MAX) wrong++;
		if (g_led[i][1] != 0u || g_led[i][2] != 0u ||
		    g_led[i][3] != 0u) {
			wrong++;
		}
	}
	CHECK(wrong == 0u,
	      "%u services showed the wrong solo readout: a held Track button "
	      "must confirm itself even when its stem is silent", wrong);
	printf("     silent stem soloed: held light full, others dark, all "
	       "%u services\n", g_nstep);
}

int main(void)
{
	printf("== Stem Tape AUDIO-REACTIVE TRACK LEDS ==\n");
	printf("%u Hz, %u-frame blocks, %u ms LED service, %u BPM running "
	       "throughout\n", SR, BLK_FRAMES, LED_MS, BPM);
	printf("meter: attack %u ms | fast release %u ms | body %u/%u ms\n",
	       ST_STEM_METER_ATTACK_MS, ST_STEM_METER_FAST_RELEASE_MS,
	       ST_STEM_METER_BODY_ATTACK_MS, ST_STEM_METER_BODY_RELEASE_MS);
	printf("       floor %u, ref %u, body span %u oct, accent span %u oct, "
	       "body share %u/256, visible %u..%u\n",
	       ST_STEM_METER_FLOOR, ST_STEM_METER_REF,
	       ST_STEM_METER_BODY_SPAN_OCTAVES,
	       ST_STEM_METER_ACCENT_SPAN_OCTAVES,
	       ST_STEM_METER_BODY_SHARE_Q8,
	       ST_STEM_METER_MIN_ON, ST_STEM_METER_MAX);

	/* The silence generator's level is a literal; if a retune moves the
	 * floor below it, every "silent" case here would quietly start
	 *measuring audible material instead. Fail loudly rather than pass
	 * vacuously. */
	if (HISS_MAG >= (double)ST_STEM_METER_FLOOR) {
		printf("FATAL: the test's residual-hiss level (%.0f) is no "
		       "longer below ST_STEM_METER_FLOOR (%u). Lower "
		       "HISS_MAG, or the silence cases prove nothing.\n",
		       HISS_MAG, ST_STEM_METER_FLOOR);
		return 2;
	}

	case_silence();
	case_sustained();
	case_percussion();
	case_vocal();
	case_four_stems_are_independent();
	case_curve_is_logarithmic();
	case_expressive_animation();
	case_attacks_pop_above_the_glow();
	case_modest_accent_is_visible();
	case_not_a_beat_indicator();
	case_solo_still_confirms();

	printf("\n");
	if (g_failures) {
		printf("AUDIO-REACTIVE LED GATE FAILED (%d cases, %d checks, "
		       "%d failures)\n", g_cases, g_checks, g_failures);
		return 1;
	}
	printf("AUDIO-REACTIVE LED GATE PASSED (%d cases, %d checks, "
	       "0 failures)\n", g_cases, g_checks);
	printf("NOTE: this proves the LIGHT FOLLOWS THE AUDIO. Whether the "
	       "brightness feels right is a look-at-it test on hardware.\n");
	return 0;
}
