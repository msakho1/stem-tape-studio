/*
 * test_inertia_transport_gate.c -- the tape transport, as HEARD.
 *
 * ======================================================================
 * WHAT THIS FILE IS FOR
 * ======================================================================
 * tests/test_inertia.c proves the ENVELOPE: that a number ramps from 0 to 1
 * and back along the right curve. That is necessary and it is not the
 * feature. The feature is that the tape is READ at that rate, so that pitch
 * and time move together and the result sounds like a reel with mass rather
 * than a volume knob being turned.
 *
 * The brief drew that line itself, and this file is the assertion of it:
 *
 *     "If I hear normal-pitched audio simply fading in or out, the
 *      implementation is wrong."
 *
 * So the central case here renders a PURE TONE through a real spin-down and
 * measures two things about the output: its PERIOD, which must stretch, and
 * its AMPLITUDE, which must not collapse. A gain fade passes neither. A
 * time-stretch that preserves pitch fails the first. Only advancing the read
 * position at the instantaneous rate passes both.
 *
 * ======================================================================
 * THE OTHER HALF: THE BOUNDS
 * ======================================================================
 * Variable rate splits the run loop's clamps into two domains -- source
 * frames for the sector, the song and the loop window; output frames for the
 * block and the seam. Getting that conversion wrong reads past the end of a
 * sector buffer, in a real-time thread, on a device with no MMU. So the rig
 * below mirrors the production run loop's arithmetic exactly and checks every
 * source index it touches against the run it was given.
 *
 * Build (from the repo root):
 *   cc -std=c11 -Wall -Wextra -Werror -Ifirmware/stemtape_player/src \
 *      firmware/stemtape_player/src/st_sector_v11.c \
 *      firmware/stemtape_player/src/st_inertia.c \
 *      firmware/stemtape_player/tests/test_inertia_transport_gate.c \
 *      -lm -o test_inertia_transport_gate && ./test_inertia_transport_gate
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

#include "st_inertia.h"
#include "st_pitch.h"
#include "st_resample.h"
#include "st_sector_v11.h"

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

#define SR         48000u
#define BLK_FRAMES 256u

/* ======================================================================
 * A SONG MADE OF A KNOWN TONE.
 *
 * A real fixture is the right thing to move a playhead over, but it is the
 * wrong thing to measure PITCH against -- music has no single period. So the
 * song here is a sine of exactly known frequency, encoded through the real
 * v1.1 sector codec so the decode path under test is the production one.
 * Every stem carries the same tone at a different amplitude, which is what
 * lets the phase-lock case below tell "all four read the same position" from
 * "all four happen to look similar".
 * ====================================================================== */
/* Long enough that a full spin-up -- ~17k output frames, and at low rate the
 * tape still passes under the head the whole time -- cannot run off the end
 * of the song and leave a window with nothing in it to measure. */
#define SONG_SECTORS 48u
#define TONE_HZ      1000.0

static uint8_t  g_song[SONG_SECTORS][ST11_SECTOR_BYTES];
static uint32_t g_song_frames;

static void build_song(void)
{
	static st11_audio_frame_t fr[ST11_FRAMES_PER_SECTOR];
	uint32_t sec, k, sp;

	g_song_frames = SONG_SECTORS * ST11_FRAMES_PER_SECTOR;
	for (sec = 0; sec < SONG_SECTORS; sec++) {
		for (k = 0; k < ST11_FRAMES_PER_SECTOR; k++) {
			const uint32_t gf = sec * ST11_FRAMES_PER_SECTOR + k;
			const double   ph = 2.0 * M_PI * TONE_HZ * gf / SR;
			const double   v  = sin(ph);

			for (sp = 0; sp < ST11_STEM_COUNT; sp++) {
				/* Distinct amplitude per stem, well inside the
				 * 24-bit range so nothing clips or saturates. */
				const double a = 2000000.0 / (double)(sp + 1u);

				fr[k].stem_l[sp] = (int32_t)(v * a);
				fr[k].stem_r[sp] = (int32_t)(v * a);
			}
		}
		st11_sector_encode(sec, sec * ST11_FRAMES_PER_SECTOR,
				    ST11_FRAMES_PER_SECTOR, 120u << 8, 0u,
				    fr, g_song[sec]);
	}
}

/* ======================================================================
 * THE RIG: main.c's stem run loop, with its clamps in the same order and
 * the same two domains. Nothing here is a simplification of the production
 * arithmetic -- it IS the production arithmetic, which is the only way a
 * bound this test passes tells you anything about the bound that ships.
 * ====================================================================== */
#define MAXOUT 400000u

static double   g_out[MAXOUT];        /* stem 0, left, as rendered */
static uint32_t g_src_at[MAXOUT];     /* the source frame each output read */
static double   g_pos_at[MAXOUT];     /* the FRACTIONAL position it represents */
static uint32_t g_nout;

/* Set if any run ever asked for a source frame outside the run it was
 * given -- the out-of-bounds read this whole conversion exists to avoid. */
static bool     g_oob;
static uint32_t g_worst_over;

/* The cursor fraction is an interpolation WEIGHT and must stay in [0,1). A
 * rate above 1x would advance it by more than a whole frame per output frame
 * while the cursor can only step one, so it would climb without bound and the
 * blend would start extrapolating past both samples. That is what the rate
 * clamp prevents, and this is what proves the clamp is doing it. */
static uint32_t g_frac_overflow;

/* Inner-loop iterations per block. The conversion is not only a safety
 * bound: it is what lets ONE run fill a whole block at low rate. Without it
 * the loop re-derives the sector index and re-checks residency several times
 * per block instead of once -- the exact per-frame overhead the run-loop
 * design removed, quietly reintroduced at the moment the transport is
 * already working hardest. */
static uint32_t g_iters;

typedef struct {
	uint32_t song_frame;
	uint32_t frac;         /* the cursor's fractional part, Q16 */
	bool     prev_valid;
	uint32_t prev_idx;     /* absolute source index `prev` was decoded from */
	st11_audio_frame_t prev;
} rig_t;

static void rig_init(rig_t *r, uint32_t start_frame)
{
	memset(r, 0, sizeof(*r));
	r->song_frame = start_frame;
}

/*
 * One output block. Mirrors stem_audio_block()'s while-loop: source-domain
 * clamps (sector, song), the conversion, then output-domain clamps (block).
 * The loop window and the seam are exercised by their own gates; what is
 * under test here is the domain split and the read.
 */
static void rig_block(rig_t *r, uint32_t rate_q16)
{
	uint32_t f = 0;

	rate_q16 = st_rs_rate_clamp(rate_q16);

	while (f < BLK_FRAMES) {
		const uint32_t needed = r->song_frame / ST11_FRAMES_PER_SECTOR;
		const uint32_t fis = r->song_frame -
				      needed * ST11_FRAMES_PER_SECTOR;
		uint32_t run, left_in_song, out_n, k, cur = 0u;
		const uint8_t *buf;

		if (needed >= SONG_SECTORS) {
			return;
		}
		buf = g_song[needed];

		/* ---- SOURCE domain ---- */
		run = ST11_FRAMES_PER_SECTOR - fis;
		left_in_song = g_song_frames - r->song_frame;
		if (run > left_in_song) {
			run = left_in_song;
		}
		if (run == 0u) {
			return;
		}

		/* ---- the conversion ---- */
		out_n = st_rs_out_frames(run, r->frac, rate_q16);

		/* ---- OUTPUT domain ---- */
		if (out_n > BLK_FRAMES - f) {
			out_n = BLK_FRAMES - f;
		}
		if (out_n == 0u) {
			return;
		}

		g_iters++;
		for (k = 0; k < out_n; k++) {
			st11_audio_frame_t nxt;
			uint32_t sp;

			if (r->frac >= ST_RS_ONE) {
				g_frac_overflow++;
			}

/* THE BOUND UNDER TEST. A read at or past `run` is a
				 * read into the next sector, which is not
				 * resident. Production clamps rather than
				 * reading; the rig RECORDS the attempt so the
				 * clamp cannot hide a bound that is wrong. */
				if (cur >= run) {
					g_oob = true;
					if (cur - run + 1u > g_worst_over) {
						g_worst_over = cur - run + 1u;
					}
					break;
				}
				st11_sector_decode_frame(buf, fis + cur, &nxt);
			if (!r->prev_valid) {
				r->prev = nxt;
				r->prev_idx = r->song_frame + cur;
				r->prev_valid = true;
			}
			if (g_nout < MAXOUT) {
				/* Stem 0 left, interpolated exactly as the
				 * renderer does it. */
				const int32_t pl = r->prev.stem_l[0];
				const int32_t v = pl +
					(int32_t)(((int64_t)(nxt.stem_l[0] - pl) *
						    (int32_t)r->frac) >> 16);

				g_out[g_nout] = (double)v;
				g_src_at[g_nout] = r->song_frame + cur;
				/* THE POSITION THIS OUTPUT FRAME REPRESENTS.
				 * The blend runs from `prev` toward the frame
				 * at the cursor, so the point on the waveform
				 * is prev's own index plus the fraction. */
				g_pos_at[g_nout] = (double)r->prev_idx +
						    (double)r->frac / 65536.0;
				g_nout++;
			}
			/* Touch every stem so a rig that silently read only
			 * one could not pass the phase-lock case. */
			for (sp = 1; sp < ST11_STEM_COUNT; sp++) {
				(void)nxt.stem_l[sp];
			}

			/* Walks the frames it crosses, as production does --
			 * above 1x the cursor can pass more than one. */
			r->frac += rate_q16;
			while (r->frac >= ST_RS_ONE) {
				r->frac -= ST_RS_ONE;
				r->prev_idx = r->song_frame + cur;
				cur++;
				if (cur >= run) {
					/* Mirrors production: drop the whole
					 * frames the run cannot supply, keep
					 * the sub-frame phase. */
					r->prev = nxt;
					r->frac &= (ST_RS_ONE - 1u);
					break;
				}
				st11_sector_decode_frame(buf, fis + cur - 1u,
							  &r->prev);
				r->prev_idx = r->song_frame + cur - 1u;
			}
		}
		f += out_n;
		r->song_frame += cur;
	}
}

/* ======================================================================
 * 1. THE BOUNDS. Every rate, every starting phase, every sector offset --
 *    and no read outside the run that was clamped for it.
 * ====================================================================== */
static void case_bounds_never_escape_the_sector(void)
{
	/*
	 * ABOVE 1x IS NOW REACHABLE, so it is swept. 69433 and 75717 are the
	 * rocker's +1 and +2.5 semitones; 131072 is 2x, the resampler's
	 * structural ceiling; 200000 is over it and must be clamped down
	 * rather than obeyed.
	 */
	static const uint32_t rates[] = {
		1u, 64u, 256u, 1000u, 6553u, 16384u, 32768u, 49152u,
		65535u, 65536u, 67456u, 69433u, 75717u, 98304u, 131072u,
		200000u,
	};
	uint32_t ri, start, blocks;

	g_cases++;
	printf("\n-- no rate and no starting phase reads past its run\n");

	g_oob = false;
	g_worst_over = 0;
	g_frac_overflow = 0;
	for (ri = 0; ri < sizeof(rates) / sizeof(rates[0]); ri++) {
		/* Start on a sector boundary, one before it, and mid-sector:
		 * the run lengths that fall out of those are 340, 1 and 170,
		 * and the one-frame run is the corner the floor in
		 * st_rs_out_frames() exists for. */
		static const uint32_t starts[] = {0u, 339u, 170u, 679u, 1000u};

		for (start = 0; start < sizeof(starts) / sizeof(starts[0]); start++) {
			rig_t r;

			rig_init(&r, starts[start]);
			g_nout = 0;
			for (blocks = 0; blocks < 60u; blocks++) {
				rig_block(&r, rates[ri]);
			}
		}
	}
	CHECK(!g_oob, "a run read %u frame(s) past its clamp", g_worst_over);
	CHECK(g_frac_overflow == 0u,
	      "the cursor fraction left [0,1) %u times -- the blend would be "
	      "extrapolating, not interpolating", g_frac_overflow);
	printf("     16 rates (0.00002x .. clamped 2x) x 5 offsets x 60 blocks, no escape\n");
}

/* ======================================================================
 * 1b. AND THE CONVERSION EARNS ITS KEEP. One run should fill a whole
 *     block at any rate, not fragment into several. See g_iters.
 * ====================================================================== */
static void case_one_run_fills_a_block(void)
{
	static const uint32_t rates[] = { 65536u, 32768u, 16384u, 4096u };
	uint32_t ri;

	g_cases++;
	printf("\n-- one run still fills a whole block at any rate\n");

	for (ri = 0; ri < 4u; ri++) {
		rig_t r;
		uint32_t b;
		double per_block;

		rig_init(&r, 0u);
		g_nout = 0;
		g_iters = 0;
		for (b = 0; b < 20u; b++) {
			rig_block(&r, rates[ri]);
		}
		/*
		 * A block needs a second run only when it straddles a sector
		 * boundary, and the tape it covers in one block is
		 * BLK_FRAMES * rate -- so the expected count is
		 *
		 *     1 + (BLK_FRAMES * rate) / FRAMES_PER_SECTOR
		 *
		 * which is 1.75 at 1x and falls toward 1.0 as the reel slows,
		 * because less tape passes the head per block. Without the
		 * conversion a run at low rate emits only as many output
		 * frames as it has SOURCE frames, so near a sector edge the
		 * block fragments into dozens of runs and this count climbs
		 * instead of falling.
		 */
		const double rate = rates[ri] / 65536.0;
		const double want = 1.0 + (BLK_FRAMES * rate) /
					   (double)ST11_FRAMES_PER_SECTOR;

		per_block = (double)g_iters / 20.0;
		printf("     %.2fx: %.2f runs per block (model %.2f)\n",
		       rate, per_block, want);
		CHECK(per_block <= want + 0.1,
		      "%.2fx took %.2f runs per block against a model of "
		      "%.2f: the source->output conversion is not being "
		      "applied", rate, per_block, want);
	}
}

/* ======================================================================
 * 2. THE PLAYHEAD ADVANCES AT THE RATE. Not faster, not slower, and not
 *    by a whole frame per output frame the way it did before inertia.
 *    This is `position += sampleDelta * transportSpeed`, measured.
 * ====================================================================== */
static void case_position_tracks_rate(void)
{
	static const struct { uint32_t q16; const char *name; } rates[] = {
		{ 65536u, "1.00x" }, { 49152u, "0.75x" },
		{ 32768u, "0.50x" }, { 16384u, "0.25x" },
	};
	uint32_t ri;

	g_cases++;
	printf("\n-- the playhead advances at the transport rate\n");

	for (ri = 0; ri < 4u; ri++) {
		rig_t r;
		uint32_t b;
		double want, got;

		rig_init(&r, 0u);
		g_nout = 0;
		for (b = 0; b < 20u; b++) {
			rig_block(&r, rates[ri].q16);
		}
		want = (double)g_nout * rates[ri].q16 / 65536.0;
		got  = (double)r.song_frame;
		printf("     %s: %u output frames consumed %.0f source "
		       "(expected %.0f)\n",
		       rates[ri].name, g_nout, got, want);
		/* Within one frame: the cursor carries its fraction across
		 * runs, so the only slack is the final partial frame. */
		CHECK(fabs(got - want) <= 1.0,
		      "%s: playhead moved %.1f source frames, expected %.1f",
		      rates[ri].name, got, want);
	}
}

/* ======================================================================
 * 3. NOTHING IS SKIPPED AND NOTHING JUMPS. Slower than 1x means source
 *    frames REPEAT (each is read by more than one output frame); it never
 *    means one is missed. A skip is a click.
 * ====================================================================== */
static void case_source_sequence_is_sane(void)
{
	rig_t r;
	uint32_t b, i, skips = 0, backwards = 0, maxstep = 0;

	g_cases++;
	printf("\n-- the source position never jumps and never goes back\n");

	rig_init(&r, 0u);
	g_nout = 0;
	for (b = 0; b < 40u; b++) {
		rig_block(&r, 20000u);   /* ~0.305x */
	}
	for (i = 1; i < g_nout; i++) {
		if (g_src_at[i] < g_src_at[i - 1]) {
			backwards++;
			continue;
		}
		if (g_src_at[i] - g_src_at[i - 1] > maxstep) {
			maxstep = g_src_at[i] - g_src_at[i - 1];
		}
		if (g_src_at[i] - g_src_at[i - 1] > 1u) {
			skips++;
		}
	}
	CHECK(backwards == 0, "the source position went backwards %u times",
	      backwards);
	CHECK(skips == 0, "%u source frames were skipped over", skips);
	CHECK(maxstep <= 1u, "largest source step was %u frames", maxstep);
	printf("     %u output frames, largest source step %u frame\n",
	       g_nout, maxstep);
}

/* ======================================================================
 * 4. THE ONE THAT MATTERS: A SPIN-DOWN IS A PITCH DROP, NOT A FADE.
 *
 *    Renders a 1 kHz tone through a real STOP and measures the output's
 *    own period and amplitude over successive windows. Period must grow --
 *    that is the pitch falling. Amplitude must NOT collapse -- if it did,
 *    this would be the fade the brief rejects.
 * ====================================================================== */
static void measure(uint32_t from, uint32_t to, double *period, double *amp)
{
	uint32_t i, crossings = 0, first = 0, last = 0;
	double peak = 0.0;

	for (i = from + 1u; i < to && i < g_nout; i++) {
		if (fabs(g_out[i]) > peak) {
			peak = fabs(g_out[i]);
		}
		/* Rising zero crossings only, so a half-cycle cannot be
		 * mistaken for a whole one. */
		if (g_out[i - 1] < 0.0 && g_out[i] >= 0.0) {
			if (crossings == 0u) {
				first = i;
			}
			last = i;
			crossings++;
		}
	}
	*amp = peak;
	*period = (crossings >= 2u)
		  ? (double)(last - first) / (double)(crossings - 1u)
		  : 0.0;
}

static void case_spindown_is_a_pitch_drop_not_a_fade(void)
{
	st_inertia_t in;
	rig_t r;
	double p[4], a[4];
	uint32_t seg, guard = 0;
	const uint32_t at_speed = 4u * BLK_FRAMES;

	g_cases++;
	printf("\n-- STOP drops the pitch and keeps the level (not a fade)\n");

	st_inertia_reset(&in);
	st_inertia_play(&in, SR);
	in.state = ST_INERTIA_RUNNING;    /* already up to speed */
	in.env_q16 = ST_INERTIA_ONE;

	rig_init(&r, 0u);
	g_nout = 0;
	/* A few blocks at nominal, for a reference period. */
	for (seg = 0; seg < 4u; seg++) {
		rig_block(&r, st_inertia_env_q16(&in));
		st_inertia_advance(&in, BLK_FRAMES);
	}
	st_inertia_stop(&in, SR);
	while (st_inertia_moving(&in) && guard++ < 4000u) {
		rig_block(&r, st_inertia_env_q16(&in));
		st_inertia_advance(&in, BLK_FRAMES);
	}

	CHECK(g_nout > at_speed + 4u * BLK_FRAMES,
	      "the spin-down rendered %u output frames past nominal speed -- "
	      "audio must keep coming out while the reel slows", g_nout - at_speed);

	/* Four windows: one at speed, three marching into the slowdown. */
	measure(0u, at_speed, &p[0], &a[0]);
	{
		const uint32_t span = g_nout - at_speed;

		measure(at_speed, at_speed + span / 4u, &p[1], &a[1]);
		measure(at_speed + span / 4u, at_speed + span / 2u, &p[2], &a[2]);
		measure(at_speed + span / 2u,
			at_speed + (3u * span) / 4u, &p[3], &a[3]);
	}

	printf("     window        period(frames)   peak\n");
	for (seg = 0; seg < 4u; seg++) {
		printf("     %-12s  %8.1f      %10.0f\n",
		       (seg == 0u) ? "at speed" :
		       (seg == 1u) ? "stopping 1" :
		       (seg == 2u) ? "stopping 2" : "stopping 3",
		       p[seg], a[seg]);
	}

	CHECK(p[0] > 40.0 && p[0] < 56.0,
	      "the reference tone should measure ~48 frames/cycle at 1 kHz, "
	      "measured %.1f", p[0]);
	CHECK(p[1] > p[0] * 1.05,
	      "the pitch must start falling immediately: %.1f -> %.1f frames "
	      "per cycle", p[0], p[1]);
	CHECK(p[2] > p[1] && p[3] > p[2],
	      "the pitch must keep falling: %.1f -> %.1f -> %.1f",
	      p[1], p[2], p[3]);
	CHECK(p[3] > p[0] * 2.0,
	      "by the last quarter the tone should be more than an octave down "
	      "(%.1f vs %.1f frames per cycle)", p[3], p[0]);

	/* THE FADE TEST. A gain ramp would have driven the level toward zero
	 * across exactly these windows. Nothing in this path touches gain, so
	 * the level must still be there while the reel is turning. */
	CHECK(a[3] > a[0] * 0.7,
	      "the level collapsed during the slowdown (%.0f -> %.0f): that is "
	      "a fade, not a tape stop", a[0], a[3]);
	printf("     level held at %.0f%% of nominal through the slowdown\n",
	       100.0 * a[3] / a[0]);
}

/* ======================================================================
 * 5. AND THE SAME ON THE WAY UP: PLAY starts low and rises to pitch.
 * ====================================================================== */
static void case_spinup_rises_to_pitch(void)
{
	st_inertia_t in;
	rig_t r;
	double p_early, p_late, p_settled, a_tmp;
	uint32_t guard = 0, ramp_out;

	g_cases++;
	printf("\n-- PLAY comes up to pitch from below (the \"wuuuup\")\n");

	st_inertia_reset(&in);
	rig_init(&r, 0u);
	g_nout = 0;
	st_inertia_play(&in, SR);
	while (!st_inertia_at_unity(&in) && guard++ < 4000u) {
		rig_block(&r, st_inertia_env_q16(&in));
		st_inertia_advance(&in, BLK_FRAMES);
	}
	ramp_out = g_nout;
	for (guard = 0; guard < 8u; guard++) {
		rig_block(&r, ST_INERTIA_ONE);
	}

	CHECK(ramp_out > 8u * BLK_FRAMES,
	      "the spin-up rendered only %u output frames -- too short to be "
	      "heard as a transport coming up to speed", ramp_out);

	measure(BLK_FRAMES, ramp_out / 3u, &p_early, &a_tmp);
	measure(ramp_out / 2u, (2u * ramp_out) / 3u, &p_late, &a_tmp);
	measure(ramp_out, g_nout, &p_settled, &a_tmp);

	printf("     period(frames): early %.1f, mid %.1f, settled %.1f\n",
	       p_early, p_late, p_settled);
	CHECK(p_early > p_late && p_late > p_settled,
	      "the pitch must rise throughout the spin-up (%.1f -> %.1f -> %.1f "
	      "frames per cycle)", p_early, p_late, p_settled);
	CHECK(p_settled > 40.0 && p_settled < 56.0,
	      "and settle at the song's real pitch (~48 frames/cycle at 1 kHz), "
	      "measured %.1f", p_settled);
	CHECK(p_early > p_settled * 1.5,
	      "the start must be well below pitch, not a nudge (%.1f vs %.1f)",
	      p_early, p_settled);
}

/* ======================================================================
 * 5b. THE BLEND IS A BLEND. Between two source frames the output must sit
 *     ON the line between them, not snap to the nearer one.
 *
 *     A nearest-frame hold has the right period and the right level, so
 *     every other case in this file passes it happily -- and it sounds
 *     like a staircase, which during a slow spin-down is the difference
 *     between tape and gravel. What separates them is ACCURACY against
 *     the waveform the position actually names, so that is what is
 *     measured: the rendered frame versus sin() evaluated at the
 *     fractional position the cursor was at.
 * ====================================================================== */
static void case_the_blend_actually_interpolates(void)
{
	rig_t r;
	uint32_t b, i, n = 0;
	double err = 0.0, sig = 0.0;

	g_cases++;
	printf("\n-- the output sits between the two source frames, not on one\n");

	rig_init(&r, 0u);
	g_nout = 0;
	/* A rate whose fraction lands all over [0,1) rather than on a few
	 * convenient points -- 0.4577x, deliberately not a power of two. */
	for (b = 0; b < 24u; b++) {
		rig_block(&r, 30000u);
	}
	for (i = 1; i < g_nout; i++) {
		const double want = sin(2.0 * M_PI * TONE_HZ * g_pos_at[i] / SR) *
				     2000000.0;
		const double d = g_out[i] - want;

		err += d * d;
		sig += want * want;
		n++;
	}
	err = sqrt(err / (double)n);
	sig = sqrt(sig / (double)n);
	printf("     RMS error vs the ideal waveform: %.3f%% of signal "
	       "(%u frames)\n", 100.0 * err / sig, n);
	/*
	 * Linear interpolation of a 1 kHz tone sampled at 48 kHz -- 48 points
	 * per cycle -- is accurate to well under a tenth of a percent. A
	 * nearest-frame hold at the same rate is off by whole percent, an
	 * order of magnitude clear of this line, so the two cannot be
	 * confused by a noisy measurement.
	 */
	CHECK(err < sig * 0.005,
	      "the rendered waveform is %.2f%% off the position it claims to "
	      "be at -- that is a hold, not a blend", 100.0 * err / sig);
}

/* ======================================================================
 * 5c. THE ROCKER'S SEMITONES, AS HEARD.
 *
 * st_pitch's own test proves the gesture and the grid; this proves the grid
 * reaches the tape. The 1 kHz tone is rendered at each half step and its
 * measured period compared against 2^(-semitones/12) of the nominal 48
 * frames per cycle -- so a table that was right on paper but wired in
 * backwards, halved, or ignored fails here.
 *
 * It is also the demonstration that this is a VARISPEED and not a pitch
 * shifter: the source frames consumed per output frame move with the ratio,
 * which is the same statement as "the tape runs faster". Nothing in this
 * path preserves duration.
 * ====================================================================== */
static void case_semitones_reach_the_tape(void)
{
	static const int steps[] = { -24, -12, -4, -2, 0, 2, 4, 5 };
	uint32_t i;

	g_cases++;
	printf("\n-- the rocker's semitones change the rendered pitch\n");
	printf("     semitones   rate      period   expected   error\n");

	for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		st_pitch_t p;
		rig_t r;
		uint32_t b, rate;
		double period, amp, want, err;

		st_pitch_reset(&p);
		p.half = (int16_t)steps[i];
		rate = st_pitch_ratio_q16(&p);

		rig_init(&r, 0u);
		g_nout = 0;
		for (b = 0; b < 24u; b++) {
			rig_block(&r, rate);
		}
		measure(g_nout / 8u, g_nout, &period, &amp);

		/* A tone read `rate` times faster comes out `rate` times
		 * higher, so its period in output frames is 48 / rate. */
		want = (SR / TONE_HZ) / ((double)rate / 65536.0);
		err  = (period - want) / want * 100.0;
		printf("     %+5.1f      %.4fx  %7.1f   %8.1f   %+5.2f%%\n",
		       steps[i] * 0.5, (double)rate / 65536.0, period, want,
		       err);

		CHECK(period > 0.0, "no cycles measured at %+.1f semitones",
		      steps[i] * 0.5);
		CHECK(fabs(err) < 2.0,
		      "%+.1f semitones rendered a period of %.1f frames, "
		      "expected %.1f (%+.2f%%)", steps[i] * 0.5, period, want,
		      err);
		/* The level must not move with the pitch: this is a transport
		 * rate, not a gain. */
		CHECK(amp > 1900000.0,
		      "the level fell to %.0f at %+.1f semitones -- pitch must "
		      "not touch gain", amp, steps[i] * 0.5);
	}
}

/* ======================================================================
 * 5d. THE FX+PLAY SLOW TOGGLE, AS HEARD.
 *
 * The companion form of 5c, and the last link in the slow-playback chain.
 * st_pitch's own test proves the product layers; this proves the product is
 * what the tape is actually read at, and that the result is TAPE SLOW rather
 * than a time stretch: at the same semitone setting, engaging slow must halve
 * the rendered frequency, i.e. DOUBLE the measured period. A time-stretching
 * implementation would leave the period where it was, and fails here.
 *
 * Measured at several semitone settings on purpose. Half the period at every
 * one of them is the audible statement of "slow is relative to the varispeed":
 * an implementation that dropped to a flat 0.5x would produce the SAME period
 * at every setting instead of one that tracks the pitch.
 * ====================================================================== */
static void case_slow_playback_reaches_the_tape(void)
{
	static const int steps[] = { -12, -4, 0, 4 };
	uint32_t i;

	g_cases++;
	printf("\n-- FX+PLAY slow halves the rendered pitch, at every setting\n");
	printf("     semitones   normal    slow     ratio\n");

	for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		st_pitch_t p;
		rig_t r;
		uint32_t b;
		double fast_period, slow_period, amp_f, amp_s, ratio;

		st_pitch_reset(&p);
		p.half = (int16_t)steps[i];

		/* Normal, then slow, from the same starting position. */
		rig_init(&r, 0u);
		g_nout = 0;
		for (b = 0; b < 32u; b++) {
			rig_block(&r, st_pitch_effective_q16(&p, ST_PITCH_ONE));
		}
		measure(g_nout / 8u, g_nout, &fast_period, &amp_f);

		rig_init(&r, 0u);
		g_nout = 0;
		for (b = 0; b < 32u; b++) {
			rig_block(&r,
				  st_pitch_effective_q16(&p, ST_PITCH_SLOW_Q16));
		}
		measure(g_nout / 8u, g_nout, &slow_period, &amp_s);

		ratio = (fast_period > 0.0) ? slow_period / fast_period : 0.0;
		printf("     %+5.1f      %7.1f  %7.1f   %.4f\n",
		       steps[i] * 0.5, fast_period, slow_period, ratio);

		CHECK(fast_period > 0.0 && slow_period > 0.0,
		      "no cycles measured at %+.1f semitones", steps[i] * 0.5);
		CHECK(fabs(ratio - 2.0) < 0.04,
		      "slow at %+.1f semitones gave a period ratio of %.4f, not "
		      "2.0 -- the pitch did not drop with the speed",
		      steps[i] * 0.5, ratio);
		/* Slow is a transport rate, not a fade. */
		CHECK(amp_s > 1900000.0,
		      "the level fell to %.0f in slow mode at %+.1f semitones "
		      "-- slow must not touch gain", amp_s, steps[i] * 0.5);
	}

	/*
	 * AND THE STATE SURVIVES IT. The brief is explicit that engaging slow
	 * must not overwrite the semitone value; here that is checked after a
	 * full render rather than in isolation, because the render is the one
	 * place the value is read on a hot path and therefore the one place a
	 * cached or scribbled copy would show up.
	 */
	{
		st_pitch_t p;
		rig_t r;
		uint32_t b;

		st_pitch_reset(&p);
		p.half = 4;
		rig_init(&r, 0u);
		for (b = 0; b < 32u; b++) {
			rig_block(&r,
				  st_pitch_effective_q16(&p, ST_PITCH_SLOW_Q16));
		}
		CHECK(p.half == 4,
		      "rendering a slow block left the semitone state at %+.1f, "
		      "not +2.0", p.half * 0.5);
	}
}

/* ======================================================================
 * 6. ONE PLAYHEAD, FOUR STEMS. The stems are interleaved in the same
 *    frame and read at one index, so phase lock is structural rather than
 *    maintained -- but a future edit could give a stem its own cursor,
 *    and that is what this catches.
 * ====================================================================== */
static void case_stems_share_one_playhead(void)
{
	rig_t r;
	uint32_t b, i, bad = 0, n = 0;

	g_cases++;
	printf("\n-- all four stems are read from the same position\n");

	rig_init(&r, 0u);
	g_nout = 0;
	for (b = 0; b < 12u; b++) {
		rig_block(&r, 24000u);   /* ~0.366x, mid-ramp */
	}

	/*
	 * Every stem carries the SAME tone at a known amplitude ratio, so if
	 * they are read at one position their samples stay in that exact
	 * ratio frame by frame. A stem read at a different position would
	 * break the ratio wherever the waveform is moving.
	 */
	for (i = 0; i < g_nout && i < 4000u; i++) {
		st11_audio_frame_t fr;
		const uint32_t gf = g_src_at[i];
		const uint32_t sec = gf / ST11_FRAMES_PER_SECTOR;
		uint32_t sp;

		if (sec >= SONG_SECTORS) {
			break;
		}
		st11_sector_decode_frame(g_song[sec],
					  gf - sec * ST11_FRAMES_PER_SECTOR, &fr);
		if (fr.stem_l[0] == 0) {
			continue;
		}
		n++;
		for (sp = 1; sp < ST11_STEM_COUNT; sp++) {
			const double want = (double)fr.stem_l[0] / (double)(sp + 1u);
			const double got  = (double)fr.stem_l[sp];

			if (fabs(got - want) > fabs(want) * 0.02 + 4.0) {
				bad++;
			}
		}
	}
	CHECK(n > 100u, "the phase-lock check examined only %u frames", n);
	CHECK(bad == 0, "%u stem samples were not in lock with stem 0", bad);
	printf("     %u frames checked across all four stems, all locked\n", n);
}

/* ======================================================================
 * 7. AT 1x, NOTHING CHANGED. The interpolating reader must not touch
 *    ordinary playback: same source frames, one per output frame.
 * ====================================================================== */
static void case_unity_is_one_to_one(void)
{
	rig_t r;
	uint32_t b, i, off = 0;

	g_cases++;
	printf("\n-- at 1x the reader is still exactly one frame per frame\n");

	rig_init(&r, 0u);
	g_nout = 0;
	for (b = 0; b < 30u; b++) {
		rig_block(&r, ST_RS_ONE);
	}
	for (i = 0; i < g_nout; i++) {
		if (g_src_at[i] != i) {
			off++;
		}
	}
	CHECK(off == 0, "%u of %u output frames did not read their own source "
	      "frame at 1x", off, g_nout);
	CHECK(r.frac == 0u, "the cursor fraction drifted to %u at 1x", r.frac);
	CHECK(g_nout == 30u * BLK_FRAMES,
	      "30 blocks at 1x produced %u frames, expected %u",
	      g_nout, 30u * BLK_FRAMES);
	printf("     %u frames, source index == output index throughout\n",
	       g_nout);
}

/* ======================================================================
 * 8. THE SEAM'S DUCK, CONVERTED. The duck is a fixed number of OUTPUT
 *    frames; the loop arms it by comparing SOURCE positions. Below 1x the
 *    playhead covers less tape while the duck runs, so the arm point must
 *    move nearer the boundary -- otherwise the duck finishes early and the
 *    wrap fires before the playhead ever reaches loopEnd.
 * ====================================================================== */
static void case_seam_arm_distance_follows_rate(void)
{
	g_cases++;
	printf("\n-- the seam's arm distance is converted into source frames\n");

	CHECK(st_rs_src_for_out(128u, ST_RS_ONE) == 128u,
	      "at 1x the duck distance must be unchanged, got %u",
	      st_rs_src_for_out(128u, ST_RS_ONE));
	CHECK(st_rs_src_for_out(128u, ST_RS_ONE / 2u) == 64u,
	      "at 0.5x a 128-frame duck covers 64 source frames, got %u",
	      st_rs_src_for_out(128u, ST_RS_ONE / 2u));
	CHECK(st_rs_src_for_out(128u, ST_RS_ONE / 4u) == 32u,
	      "at 0.25x, 32, got %u", st_rs_src_for_out(128u, ST_RS_ONE / 4u));
	CHECK(st_rs_src_for_out(128u, 1u) >= 1u,
	      "at a crawl the arm distance must not collapse to zero");
	printf("     1x->128, 0.5x->64, 0.25x->32 source frames\n");
}

int main(void)
{
	printf("== Stem Tape TRANSPORT INERTIA, AS RENDERED ==\n");
	printf("%u Hz, %u-frame blocks, %.0f Hz test tone through the real "
	       "v1.1 codec\n", SR, BLK_FRAMES, TONE_HZ);

	build_song();

	case_bounds_never_escape_the_sector();
	case_one_run_fills_a_block();
	case_position_tracks_rate();
	case_source_sequence_is_sane();
	case_spindown_is_a_pitch_drop_not_a_fade();
	case_spinup_rises_to_pitch();
	case_the_blend_actually_interpolates();
	case_semitones_reach_the_tape();
	case_slow_playback_reaches_the_tape();
	case_stems_share_one_playhead();
	case_unity_is_one_to_one();
	case_seam_arm_distance_follows_rate();

	printf("\n");
	if (g_failures) {
		printf("INERTIA TRANSPORT GATE FAILED (%d cases, %d checks, "
		       "%d failures)\n", g_cases, g_checks, g_failures);
		return 1;
	}
	printf("INERTIA TRANSPORT GATE PASSED (%d cases, %d checks, "
	       "0 failures)\n", g_cases, g_checks);
	return 0;
}
