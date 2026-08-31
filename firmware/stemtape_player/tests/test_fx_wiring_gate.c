/*
 * test_fx_wiring_gate.c -- does the FX rack actually reach the audio?
 *
 * WHY THIS FILE EXISTS. tests/test_fx_dsp.c proves the arithmetic and says so
 * itself, in its own last line: "host arithmetic only. Not production-linked
 * and not heard." tests/test_fx_ctl.c proves the button grammar, equally
 * standalone. Neither touches main.c. So the rack could have been correct
 * maths sitting in a function nothing called, and every gate in this
 * repository would still have been green -- the runtime symbol gate did not
 * even require st_fx_process() to survive linking.
 *
 * That gap is what this closes. It renders REAL decoded four-stem audio from
 * the frozen fixture through the SAME insertion arithmetic main.c performs,
 * at both of its two insertion points, and asserts the rack changes the
 * samples -- and, just as importantly, that it changes NOTHING when no effect
 * is held.
 *
 * WHAT MAKES IT A WIRING GATE RATHER THAN A SECOND DSP TEST. It never asserts
 * a sample value produced by st_fx_process(). Every expectation is a property
 * this file measures for itself out of the two rendered buffers -- energy
 * above a frequency, runs of exact silence, signal persisting after the input
 * stops. That keeps it honest about the one question it asks (is the rack in
 * the path, and does it do the kind of thing its name claims) without
 * re-deriving expected audio from the function under test, which would prove
 * nothing.
 *
 * THE INSERTION ARITHMETIC IS COPIED FROM main.c DELIBERATELY, and the CI step
 * that runs this also greps main.c to prove those call sites still exist and
 * still look like this. A host file cannot call main.c's static render
 * function, so the alternative to copying is not testing it at all.
 *
 * Build (from the repo root, which is where the fixture path resolves):
 *   cc -std=c11 -Wall -Wextra -Werror -Ifirmware/stemtape_player/src \
 *      firmware/stemtape_player/src/st_fx.c \
 *      firmware/stemtape_player/src/st_sector_v11.c \
 *      firmware/stemtape_player/src/st_stem_mix.c \
 *      firmware/stemtape_player/tests/test_fx_wiring_gate.c -lm \
 *      -o test_fx_wiring_gate && ./test_fx_wiring_gate
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "st_sector_v11.h"
#include "st_stem_mix.h"
#include "st_fx.h"
#include "st_fx_ctl.h"

static int g_checks, g_failures;

#define CHECK(cond, fmt, ...)                                                  \
	do {                                                                   \
		g_checks++;                                                    \
		if (!(cond)) {                                                 \
			g_failures++;                                          \
			printf("  FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,  \
			       ##__VA_ARGS__);                                 \
		}                                                              \
	} while (0)

/* ---------------------------------------------------------------------- *
 * The frozen fixture. Same file and same loader the loop-seam gate uses.
 * ---------------------------------------------------------------------- */
static uint8_t *g_fix;
static uint32_t g_fix_sectors, g_fix_frames;

static void load_fixture(void)
{
	const char *path = "handoff/v1.1/binaries/song-sectors-four-stem.bin";
	FILE *f = fopen(path, "rb");
	long sz;

	if (!f) {
		fprintf(stderr, "FATAL: could not open %s (run from the repo root)\n",
			path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	rewind(f);
	g_fix = malloc((size_t)sz);
	if (!g_fix || fread(g_fix, 1, (size_t)sz, f) != (size_t)sz) {
		fprintf(stderr, "FATAL: short read on %s\n", path);
		exit(2);
	}
	fclose(f);
	g_fix_sectors = (uint32_t)((size_t)sz / ST11_SECTOR_BYTES);
	g_fix_frames = g_fix_sectors * ST11_FRAMES_PER_SECTOR;
	if (g_fix_sectors < 4u) {
		fprintf(stderr, "FATAL: fixture has only %u sectors\n", g_fix_sectors);
		exit(2);
	}
}

/* ---------------------------------------------------------------------- *
 * THE RENDER. This mirrors main.c's stem_render_run(): decode, optional
 * STEM-scope rack in the decoder's own Q23 domain, the real prepared mixer,
 * then the optional GLOBAL-scope rack with the shift-up/saturate-back pair.
 * Master volume and the loop seam are deliberately left at unity so nothing
 * but the rack can move a sample.
 * ---------------------------------------------------------------------- */
#define SCOPE_NONE   0
#define SCOPE_STEM   1
#define SCOPE_GLOBAL 2

static void render(int scope, uint8_t mask, uint32_t target_stem,
		   uint32_t first_frame, uint32_t n,
		   int16_t *out_l, int16_t *out_r, bool silence_input_after,
		   uint32_t silence_from)
{
	st_stem_mix_channel_t ch[ST11_STEM_COUNT];
	st_stem_mix_prepared_t prep;
	st_fx_t fx;
	uint32_t s, k;

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		ch[s].gain_q8 = 256;   /* unity: no fader colouring the result */
		ch[s].mute = false;
		ch[s].solo = false;
	}
	st_stem_mix_prepare(ch, &prep);

	st_fx_reset(&fx);
	/* 120 BPM at 48 kHz: 24000 frames per beat, downbeat at frame 0. The
	 * exact tempo does not matter to a wiring question, but it must be a
	 * real one -- st_fx_prepare() fails the tempo-locked effects CLOSED
	 * when frames_per_beat is zero, and a gate that silently tested the
	 * closed path would prove nothing. */
	st_fx_prepare(&fx, 24000u, 0u, mask);

	for (k = 0; k < n; k++) {
		/* THE FRAME COUNTER IS MONOTONIC, THE FIXTURE READ WRAPS.
		 * The frozen fixture is 43 sectors -- 14,620 frames, about
		 * 0.3 s -- which is shorter than a single beat at any musical
		 * tempo, so a gate that ran only to the end of it could not
		 * observe an echo repeat or a gate cycle at all. Reading it
		 * modulo its length yields as much real fixture audio as the
		 * test needs, while `f` keeps counting so the tempo-locked
		 * effects see the continuous song position they would see in
		 * production. The wrap puts one waveform discontinuity per lap
		 * into the material; that is irrelevant to whether the rack is
		 * in the path, and this file makes no seam claims. */
		uint32_t f = first_frame + k;
		uint32_t src = f % g_fix_frames;
		uint32_t sec = src / ST11_FRAMES_PER_SECTOR;
		uint32_t fis = src % ST11_FRAMES_PER_SECTOR;
		st11_audio_frame_t frame;
		int16_t stem_l, stem_r;

		st11_sector_decode_frame(g_fix + (size_t)sec * ST11_SECTOR_BYTES,
					 fis, &frame);

		/* For the echo test: cut the source dead partway through, so
		 * anything still audible afterwards came from the rack. */
		if (silence_input_after && k >= silence_from) {
			memset(frame.stem_l, 0, sizeof(frame.stem_l));
			memset(frame.stem_r, 0, sizeof(frame.stem_r));
		}

		/* ---- main.c:1956-1963, STEM SCOPE ---- */
		if (scope == SCOPE_STEM && st_fx_running(&fx)) {
			int32_t fl = frame.stem_l[target_stem];
			int32_t fr = frame.stem_r[target_stem];

			st_fx_process(&fx, &fl, &fr, f);
			frame.stem_l[target_stem] = fl;
			frame.stem_r[target_stem] = fr;
		}

		st_stem_mix_frame_prepared(&frame, &prep, &stem_l, &stem_r);

		/* ---- main.c:2010-2023, GLOBAL SCOPE ---- */
		if (scope == SCOPE_GLOBAL && st_fx_running(&fx)) {
			int32_t gl = (int32_t)stem_l << 8;
			int32_t gr = (int32_t)stem_r << 8;

			st_fx_process(&fx, &gl, &gr, f);
			gl >>= 8;
			gr >>= 8;
			if (gl > INT16_MAX) gl = INT16_MAX;
			if (gl < INT16_MIN) gl = INT16_MIN;
			if (gr > INT16_MAX) gr = INT16_MAX;
			if (gr < INT16_MIN) gr = INT16_MIN;
			stem_l = (int16_t)gl;
			stem_r = (int16_t)gr;
		}

		out_l[k] = stem_l;
		out_r[k] = stem_r;
	}
}

/* ---------------------------------------------------------------------- *
 * Properties this file measures for itself. None of these consults st_fx.
 * ---------------------------------------------------------------------- */

/* Crude high-frequency energy: the first difference is a one-pole highpass,
 * so its mean magnitude falls when a lowpass is inserted and does not fall
 * merely because the signal got quieter overall (that is what the companion
 * total-energy figure is for). */
static double hf_energy(const int16_t *x, uint32_t n)
{
	double acc = 0.0;
	uint32_t i;

	for (i = 1; i < n; i++)
		acc += fabs((double)x[i] - (double)x[i - 1]);
	return acc / (double)(n - 1);
}

static double total_energy(const int16_t *x, uint32_t n)
{
	double acc = 0.0;
	uint32_t i;

	for (i = 0; i < n; i++)
		acc += fabs((double)x[i]);
	return acc / (double)n;
}

static uint32_t differing_samples(const int16_t *a, const int16_t *b, uint32_t n)
{
	uint32_t i, d = 0;

	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			d++;
	return d;
}

/* Longest run of exact zeroes -- what a gate closing actually produces, and
 * something ordinary programme material essentially never does for long. */
static uint32_t longest_zero_run(const int16_t *x, uint32_t n)
{
	uint32_t i, run = 0, best = 0;

	for (i = 0; i < n; i++) {
		if (x[i] == 0) {
			run++;
			if (run > best) best = run;
		} else {
			run = 0;
		}
	}
	return best;
}

/* Two seconds at 48 kHz. Four beats at the 120 BPM test tempo, so the
 * 0.375-beat echo repeats many times and the 1/16 gate opens and closes
 * many times inside one buffer. The fixture is looped underneath to fill
 * it (see render()). */
#define N 96000u

static int16_t dry_l[N], dry_r[N], wet_l[N], wet_r[N];

/* ======================================================================
 * 1. NOTHING HELD MUST CHANGE NOTHING. The most important case in the file:
 *    it is the one that protects ordinary playback. If this ever fails, the
 *    rack is colouring audio for people who never opened FX.
 * ====================================================================== */
static void t_bypass_is_bit_exact(void)
{
	printf("\n-- no effect held: both scopes are bit-exact passthrough\n");
	render(SCOPE_NONE, 0, 0, 0, N, dry_l, dry_r, false, 0);

	render(SCOPE_STEM, 0, 0, 0, N, wet_l, wet_r, false, 0);
	CHECK(differing_samples(dry_l, wet_l, N) == 0,
	      "STEM scope with an empty mask altered %u of %u left samples",
	      differing_samples(dry_l, wet_l, N), N);
	CHECK(differing_samples(dry_r, wet_r, N) == 0,
	      "STEM scope with an empty mask altered the right channel");

	render(SCOPE_GLOBAL, 0, 0, 0, N, wet_l, wet_r, false, 0);
	CHECK(differing_samples(dry_l, wet_l, N) == 0,
	      "GLOBAL scope with an empty mask altered %u of %u left samples",
	      differing_samples(dry_l, wet_l, N), N);
	CHECK(differing_samples(dry_r, wet_r, N) == 0,
	      "GLOBAL scope with an empty mask altered the right channel");
	printf("     dry energy %.1f, %u frames of real fixture audio\n",
	       total_energy(dry_l, N), N);
}

/* ======================================================================
 * 2. EACH EFFECT REACHES THE AUDIO, IN BOTH SCOPES. Difference alone would
 *    be satisfied by any bug that merely corrupted samples, so each one is
 *    also checked to move the signal the way its name claims.
 * ====================================================================== */
static void t_filter_reaches_audio(void)
{
	double dry_hf, wet_hf;

	printf("\n-- Filter (T1) reaches the audio and removes highs\n");
	render(SCOPE_GLOBAL, 0, 0, 0, N, dry_l, dry_r, false, 0);
	render(SCOPE_GLOBAL, ST_FX_BIT(ST_FX_FILTER), 0, 0, N,
	       wet_l, wet_r, false, 0);

	CHECK(differing_samples(dry_l, wet_l, N) > N / 2u,
	      "filter changed only %u of %u samples -- is it in the path?",
	      differing_samples(dry_l, wet_l, N), N);

	dry_hf = hf_energy(dry_l, N);
	wet_hf = hf_energy(wet_l, N);
	CHECK(wet_hf < dry_hf * 0.8,
	      "filter did not remove high frequencies: dry HF %.1f, wet HF %.1f",
	      dry_hf, wet_hf);
	printf("     HF energy %.1f -> %.1f (%.0f%% removed)\n",
	       dry_hf, wet_hf, 100.0 * (1.0 - wet_hf / dry_hf));
}

static void t_gate_reaches_audio(void)
{
	uint32_t dry_run, wet_run;

	printf("\n-- Gate/Stutter (T4) reaches the audio and produces silence\n");
	render(SCOPE_GLOBAL, 0, 0, 0, N, dry_l, dry_r, false, 0);
	render(SCOPE_GLOBAL, ST_FX_BIT(ST_FX_GATE), 0, 0, N,
	       wet_l, wet_r, false, 0);

	dry_run = longest_zero_run(dry_l, N);
	wet_run = longest_zero_run(wet_l, N);
	CHECK(wet_run > dry_run * 4u + 100u,
	      "gate produced no silent run: dry longest %u, wet longest %u",
	      dry_run, wet_run);
	/* A 1/16 gate at 24000 frames/beat closes for part of a 6000-frame
	 * cycle. The run must be a real closure, not a stray sample, and must
	 * not swallow the whole buffer either. */
	CHECK(wet_run > 200u && wet_run < N,
	      "gate silent run %u is not a plausible 1/16 closure", wet_run);
	printf("     longest exact-zero run %u -> %u frames\n", dry_run, wet_run);
}

static void t_dirt_reaches_audio(void)
{
	double dry_e, wet_e;

	printf("\n-- Distortion (T3) reaches the audio and adds level\n");
	render(SCOPE_GLOBAL, 0, 0, 0, N, dry_l, dry_r, false, 0);
	render(SCOPE_GLOBAL, ST_FX_BIT(ST_FX_DIRT), 0, 0, N,
	       wet_l, wet_r, false, 0);

	CHECK(differing_samples(dry_l, wet_l, N) > N / 2u,
	      "distortion changed only %u of %u samples",
	      differing_samples(dry_l, wet_l, N), N);

	dry_e = total_energy(dry_l, N);
	wet_e = total_energy(wet_l, N);
	CHECK(wet_e > dry_e,
	      "distortion did not raise level: dry %.1f, wet %.1f", dry_e, wet_e);
	printf("     mean level %.1f -> %.1f\n", dry_e, wet_e);
}

static void t_echo_reaches_audio(void)
{
	const uint32_t cut = N / 3u;
	double tail_dry, tail_wet;

	printf("\n-- Delay/Echo (T2) reaches the audio and outlives the source\n");
	/* Kill the source a third of the way in. Anything audible after that
	 * point cannot have come from the fixture. */
	render(SCOPE_GLOBAL, 0, 0, 0, N, dry_l, dry_r, true, cut);
	render(SCOPE_GLOBAL, ST_FX_BIT(ST_FX_ECHO), 0, 0, N,
	       wet_l, wet_r, true, cut);

	tail_dry = total_energy(dry_l + cut + 2000u, N - cut - 2000u);
	tail_wet = total_energy(wet_l + cut + 2000u, N - cut - 2000u);

	CHECK(tail_dry == 0.0,
	      "the dry tail is not silent (%.3f) -- the source cut did not work",
	      tail_dry);
	CHECK(tail_wet > 0.0,
	      "echo produced nothing after the source stopped -- is it in the path?");
	printf("     energy after the source stops: dry %.3f, wet %.1f\n",
	       tail_dry, tail_wet);
}

/* ======================================================================
 * 3. THE STEM SCOPE IS A DIFFERENT INSERTION POINT, and must work too. It
 *    touches ONE stem before the mix, so its effect on the mixed output is
 *    real but smaller than the global scope's -- which is itself the check
 *    that it went in where the contract says.
 * ====================================================================== */
static void t_stem_scope_reaches_audio(void)
{
	uint32_t diff_stem, diff_global;

	printf("\n-- STEM scope reaches the mix through one stem only\n");
	render(SCOPE_NONE, 0, 0, 0, N, dry_l, dry_r, false, 0);

	render(SCOPE_STEM, ST_FX_BIT(ST_FX_FILTER), 0, 0, N,
	       wet_l, wet_r, false, 0);
	diff_stem = differing_samples(dry_l, wet_l, N);
	CHECK(diff_stem > N / 10u,
	      "STEM-scope filter changed only %u of %u samples", diff_stem, N);

	render(SCOPE_GLOBAL, ST_FX_BIT(ST_FX_FILTER), 0, 0, N,
	       wet_l, wet_r, false, 0);
	diff_global = differing_samples(dry_l, wet_l, N);

	CHECK(diff_global >= diff_stem,
	      "filtering the whole mix (%u changed) moved less than filtering "
	      "one stem (%u) -- the insertion points look swapped",
	      diff_global, diff_stem);
	printf("     samples changed: one stem %u, whole mix %u, of %u\n",
	       diff_stem, diff_global, N);
}

/* ======================================================================
 * 4. EACH OF THE FOUR TARGETS IS REACHABLE. One rack, moved -- so pointing
 *    it at a different stem must produce a different result, or the target
 *    index is being ignored.
 * ====================================================================== */
static void t_every_stem_target_is_reachable(void)
{
	uint32_t s;

	printf("\n-- the rack's target actually selects a stem\n");
	render(SCOPE_NONE, 0, 0, 0, N, dry_l, dry_r, false, 0);

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		uint32_t d;

		render(SCOPE_STEM, ST_FX_BIT(ST_FX_FILTER), s, 0, N,
		       wet_l, wet_r, false, 0);
		d = differing_samples(dry_l, wet_l, N);
		CHECK(d > 0, "stem %u as the target changed nothing", s);
		printf("     target stem %u: %u of %u samples changed\n", s, d, N);
	}
}

/* ======================================================================
 * 5. THE GLOBAL INSERTION'S OWN ARITHMETIC IS LOSSLESS. main.c shifts the
 *    mixer's int16 up 8 into the rack's Q23 domain and saturates back. That
 *    round trip must be the identity, or merely opening FX would degrade the
 *    signal before any effect ran. Checked over every int16, not sampled.
 * ====================================================================== */
static void t_global_shift_roundtrip_is_identity(void)
{
	int v;
	int bad = 0;

	printf("\n-- the GLOBAL scope's <<8 / >>8 round trip is the identity\n");
	for (v = INT16_MIN; v <= INT16_MAX; v++) {
		int32_t g = (int32_t)v << 8;

		g >>= 8;
		if (g > INT16_MAX) g = INT16_MAX;
		if (g < INT16_MIN) g = INT16_MIN;
		if ((int16_t)g != (int16_t)v)
			bad++;
	}
	CHECK(bad == 0, "%d of 65536 int16 values do not survive the round trip",
	      bad);
	printf("     all 65536 int16 values survive unchanged\n");
}

int main(void)
{
	printf("== Stem Tape FX PRODUCTION WIRING gate ==\n");
	printf("real four-stem fixture audio, through main.c's own insertion\n");
	printf("arithmetic at both scopes. Properties are measured here; no\n");
	printf("expected sample is taken from the function under test.\n");

	load_fixture();
	printf("fixture: %u sectors, %u frames\n", g_fix_sectors, g_fix_frames);

	t_bypass_is_bit_exact();
	t_filter_reaches_audio();
	t_gate_reaches_audio();
	t_dirt_reaches_audio();
	t_echo_reaches_audio();
	t_stem_scope_reaches_audio();
	t_every_stem_target_is_reachable();
	t_global_shift_roundtrip_is_identity();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	if (g_failures) {
		printf("FX WIRING GATE FAILED\n");
		return 1;
	}
	printf("FX WIRING GATE PASSED\n");
	printf("NOTE: this proves the rack is IN the audio path and changes it.\n");
	printf("It is still not a listening test.\n");
	return 0;
}
