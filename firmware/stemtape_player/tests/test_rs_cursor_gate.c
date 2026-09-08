/*
 * test_rs_cursor_gate.c -- the extraction of the four resampler cursors out of
 * stem_render_run() and into st_rs_cursor.h is a NO-OP, proved sample by
 * sample against a frozen transcription of the code that was there before.
 *
 * ======================================================================
 * WHY THIS GATE IS SHAPED LIKE THIS
 * ======================================================================
 * Every other audio gate in this repository states the same limitation in its
 * own report: main.c cannot be linked on the host, so the gate necessarily
 * checks a MODEL of stem_render_run() and can agree with a main.c that has
 * drifted from it. That is survivable for a gate judging BEHAVIOUR. It is
 * worthless for a commit whose entire claim is THE OUTPUT DID NOT CHANGE.
 *
 * So this gate has two arms over identical inputs:
 *
 *   REFERENCE  a verbatim transcription of the pre-extraction main.c text,
 *              frozen here. It is a copy, and it is allowed to be, because it
 *              is the thing being compared AGAINST, not the thing shipping.
 *   PRODUCTION st_rs_cursor.h itself -- the real header main.c now includes,
 *              linked, not modelled.
 *
 * If the two ever differ on any sample, any carried fraction, any consumed
 * source count or any interpolator state, this gate fails. That is the whole
 * proof, and it is a proof about the shipping code rather than about a model
 * of it.
 *
 * ======================================================================
 * WHAT IS DELIBERATELY DUPLICATED, AND WHY
 * ======================================================================
 * The BLEND is written out in full in BOTH arms rather than factored into a
 * shared helper. It is not what is under test -- it stays in main.c untouched
 * -- but it consumes frac[] and prev, so it is how a cursor difference becomes
 * an audible difference. Each arm therefore carries its own transcription of
 * it, exactly as main.c has it, so neither arm can be "right" for a reason the
 * other does not share.
 *
 * ======================================================================
 * AND WHAT THIS GATE DOES NOT PROVE
 * ======================================================================
 * It does not prove main.c CALLS these helpers in the right order, or at all
 * -- that is the wiring check (I-1/I-2), which reads production main.c
 * directly. It does not cover the unity fast path, which this commit does not
 * touch and test_stem_playback_gate.c pins at 0x2a737e00. And a hash is only
 * as good as the states the sweep actually reaches, which is why case 3 fails
 * the build if the sweep did NOT reach the clamp, the array decode, the real
 * re-decode or the out-of-run corner.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "st_pitch.h"
#include "st_planar.h"
#include "st_resample.h"
#include "st_rs_cursor.h"
#include "st_v11_format.h"

static int g_checks;
static int g_failures;
static int g_cases;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		g_checks++;                                                    \
		if (!(cond)) {                                                 \
			g_failures++;                                          \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);          \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

#define CASE(name)                                                             \
	do {                                                                   \
		g_cases++;                                                     \
		printf("case %d: %s\n", g_cases, (name));                      \
	} while (0)

/* ---- deterministic inputs ------------------------------------------- */

/* xorshift32. Fixed seed, so every run of this gate on every machine sees the
 * same sweep and the pinned hash in case 2 is reproducible. */
static uint32_t s_rng = 0x13572468u;

static uint32_t rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

static uint32_t rnd_range(uint32_t lo, uint32_t hi) /* inclusive */
{
	return lo + (rnd() % (hi - lo + 1u));
}

typedef struct {
	uint8_t b[ST_PL_STEMS][ST_PL_GROUP_BYTES];
} groups_t;

/* Real planar group bytes: a stereo 16-bit frame per 4 bytes at
 * ST_PL_OFF_FRAMES, which is what st_pl_decode_stem_inline() reads. Content is
 * a per-stem, per-group deterministic waveform -- not noise, so that a
 * one-sample cursor slip produces a DIFFERENT interpolation rather than
 * accidentally the same one. */
static void fill_groups(groups_t *g, uint32_t salt)
{
	uint32_t k, f;

	memset(g, 0, sizeof(*g));
	for (k = 0; k < ST_PL_STEMS; k++) {
		for (f = 0; f < ST_PL_FRAMES_PER_GROUP; f++) {
			const uint32_t off = st_pl_frame_off(f);
			const int32_t l = (int32_t)((f * 277u + k * 4093u + salt * 131u) % 60000u) - 30000;
			const int32_t r = (int32_t)((f * 613u + k * 1741u + salt * 977u) % 60000u) - 30000;
			const uint32_t w = ((uint32_t)(uint16_t)(int16_t)l) |
					   (((uint32_t)(uint16_t)(int16_t)r) << 16);

			memcpy(g->b[k] + off, &w, sizeof(w));
		}
	}
}

#define MAX_OUT 256u /* BLK_FRAMES */

typedef struct {
	uint32_t frame_in_group[ST_PL_STEMS];
	int8_t   dirs[ST_PL_STEMS];
	uint32_t src_avail[ST_PL_STEMS];
	uint32_t frac_in[ST_PL_STEMS];
	bool     prev_valid_in[ST_PL_STEMS];
	int32_t  prev_l_in[ST_PL_STEMS];
	int32_t  prev_r_in[ST_PL_STEMS];
	uint32_t rate_q16;
	uint32_t n;
} spec_t;

typedef struct {
	uint32_t hash;                        /* every blended stem sample, in order */
	uint32_t frac_io[ST_PL_STEMS];
	uint32_t used_out[ST_PL_STEMS];
	int32_t  prev_l[ST_PL_STEMS];
	int32_t  prev_r[ST_PL_STEMS];
	bool     prev_valid[ST_PL_STEMS];
	uint32_t idx_hash;                    /* every read index formed, in order */
	/* EVERY LANE'S CURSOR AFTER EACH WALK -- what st_fx_process()'s clock
	 * offset and the per-stem meter's g_stem_zero_at[sp] actually read in
	 * main.c. A shared cursor that forgets to publish lanes 1-3 is invisible
	 * in the samples and visible here. */
	uint32_t cur_hash;
} result_t;

static uint32_t hmix(uint32_t h, uint32_t v)
{
	h ^= v;
	h *= 16777619u;
	return h;
}

/* Coverage of the states the sweep actually reached. Counted once, in the
 * reference arm, because both arms see identical inputs. */
static struct {
	uint32_t clamped_cursor;   /* cur >= src_avail at index time */
	uint32_t array_decode;     /* the four indices genuinely differed */
	uint32_t shared_decode;
	uint32_t real_prev_decode; /* pidx != idx: a second walk step */
	uint32_t out_of_run;       /* the floored corner */
	uint32_t primed;           /* a lane with prev_valid false */
	uint32_t reversed_lane;
	uint32_t locked_runs;      /* the shared-lane path was taken */
	uint32_t unlocked_runs;
} s_cov;

/* ===================================================================== */
/*  ARM A -- REFERENCE: the pre-extraction main.c text, frozen            */
/* ===================================================================== */

static void run_ref(const groups_t *g, const spec_t *s, result_t *out)
{
	const uint8_t *grp[ST_PL_STEMS];
	const uint32_t *frame_in_group = s->frame_in_group;
	const int8_t *dirs = s->dirs;
	const uint32_t *src_avail = s->src_avail;
	const uint32_t rate_q16 = s->rate_q16;
	uint32_t frac[ST_PL_STEMS];
	uint32_t cur[ST_PL_STEMS];
	uint32_t idx[ST_PL_STEMS];
	uint32_t sp, k;
	st11_audio_frame_t s_rs_prev;
	bool s_rs_prev_valid[ST_PL_STEMS];

	memset(out, 0, sizeof(*out));
	out->hash = 2166136261u;
	out->idx_hash = 2166136261u;
	out->cur_hash = 2166136261u;
	for (sp = 0; sp < ST_PL_STEMS; sp++) {
		grp[sp] = g->b[sp];
		frac[sp] = s->frac_in[sp];
		cur[sp] = 0u;
		s_rs_prev_valid[sp] = s->prev_valid_in[sp];
		s_rs_prev.stem_l[sp] = s->prev_l_in[sp];
		s_rs_prev.stem_r[sp] = s->prev_r_in[sp];
		if (!s_rs_prev_valid[sp]) {
			s_cov.primed++;
		}
		if (dirs[sp] < 0) {
			s_cov.reversed_lane++;
		}
	}

	for (k = 0; k < s->n; k++) {
		st11_audio_frame_t frame;
		st11_audio_frame_t nxt;

		/* ---- verbatim: THE HARD BOUND ---- */
		for (sp = 0; sp < ST_PL_STEMS; sp++) {
			const uint32_t c = (cur[sp] >= src_avail[sp])
					   ? (src_avail[sp] - 1u) : cur[sp];

			if (cur[sp] >= src_avail[sp]) {
				s_cov.clamped_cursor++;
			}
			idx[sp] = (uint32_t)((int32_t)frame_in_group[sp] +
					      dirs[sp] * (int32_t)c);
		}
		/* ---- verbatim: THE ARRAY FORM ---- */
		if (idx[0] == idx[1] && idx[1] == idx[2] &&
		    idx[2] == idx[3]) {
			s_cov.shared_decode++;
			st_pl_decode_frame_shared(grp, idx[0], &nxt);
		} else {
			s_cov.array_decode++;
			st_pl_decode_frame(grp, idx, &nxt);
		}
		for (sp = 0; sp < ST_PL_STEMS; sp++) {
			if (!s_rs_prev_valid[sp]) {
				s_rs_prev.stem_l[sp] = nxt.stem_l[sp];
				s_rs_prev.stem_r[sp] = nxt.stem_r[sp];
				s_rs_prev_valid[sp] = true;
			}
		}
		/* ---- verbatim: THE BLEND (stays in main.c; see the header) ---- */
		for (sp = 0; sp < ST11_STEM_COUNT; sp++) {
			const int32_t pl = s_rs_prev.stem_l[sp];
			const int32_t pr = s_rs_prev.stem_r[sp];

			frame.stem_l[sp] = pl +
				(int32_t)(((int64_t)(nxt.stem_l[sp] - pl) *
					    (int32_t)frac[sp]) >> 16);
			frame.stem_r[sp] = pr +
				(int32_t)(((int64_t)(nxt.stem_r[sp] - pr) *
					    (int32_t)frac[sp]) >> 16);
		}
		/* ---- verbatim: THE WALK ---- */
		for (sp = 0; sp < ST_PL_STEMS; sp++) {
			frac[sp] += rate_q16;
			while (frac[sp] >= ST_RS_ONE) {
				frac[sp] -= ST_RS_ONE;
				cur[sp]++;
				if (cur[sp] >= src_avail[sp]) {
					s_cov.out_of_run++;
					cur[sp] = src_avail[sp];
					s_rs_prev.stem_l[sp] = nxt.stem_l[sp];
					s_rs_prev.stem_r[sp] = nxt.stem_r[sp];
					frac[sp] &= (ST_RS_ONE - 1u);
					break;
				}
				{
					uint32_t pc = cur[sp] - 1u;
					uint32_t pidx;

					if (pc >= src_avail[sp]) {
						pc = src_avail[sp] - 1u;
					}
					pidx = (uint32_t)((int32_t)frame_in_group[sp] +
							   dirs[sp] * (int32_t)pc);
					if (pidx == idx[sp]) {
						s_rs_prev.stem_l[sp] = nxt.stem_l[sp];
						s_rs_prev.stem_r[sp] = nxt.stem_r[sp];
					} else {
						s_cov.real_prev_decode++;
						st_pl_decode_stem_inline(
							grp[sp], pidx,
							&s_rs_prev.stem_l[sp],
							&s_rs_prev.stem_r[sp]);
					}
				}
			}
		}

		for (sp = 0; sp < ST_PL_STEMS; sp++) {
			out->hash = hmix(out->hash, (uint32_t)frame.stem_l[sp]);
			out->hash = hmix(out->hash, (uint32_t)frame.stem_r[sp]);
			out->idx_hash = hmix(out->idx_hash, idx[sp]);
			out->cur_hash = hmix(out->cur_hash, cur[sp]);
		}
	}

	/* ---- verbatim: THE WRITEBACK ---- */
	for (sp = 0; sp < ST_PL_STEMS; sp++) {
		out->frac_io[sp] = frac[sp];
		out->used_out[sp] = (cur[sp] > src_avail[sp]) ? src_avail[sp] : cur[sp];
	}
	for (sp = 0; sp < ST_PL_STEMS; sp++) {
		out->prev_l[sp] = s_rs_prev.stem_l[sp];
		out->prev_r[sp] = s_rs_prev.stem_r[sp];
		out->prev_valid[sp] = s_rs_prev_valid[sp];
	}
}

/* ===================================================================== */
/*  ARM B -- PRODUCTION: st_rs_cursor.h, the real header                  */
/* ===================================================================== */

static void run_new(const groups_t *g, const spec_t *s, result_t *out)
{
	const uint8_t *grp[ST_PL_STEMS];
	uint32_t frac[ST_PL_STEMS];
	uint32_t cur[ST_PL_STEMS];
	uint32_t idx[ST_PL_STEMS];
	uint32_t sp, k;
	bool locked;
	st11_audio_frame_t s_rs_prev;
	bool s_rs_prev_valid[ST_PL_STEMS];

	memset(out, 0, sizeof(*out));
	out->hash = 2166136261u;
	out->idx_hash = 2166136261u;
	out->cur_hash = 2166136261u;
	for (sp = 0; sp < ST_PL_STEMS; sp++) {
		grp[sp] = g->b[sp];
		frac[sp] = s->frac_in[sp];
		cur[sp] = 0u;
		s_rs_prev_valid[sp] = s->prev_valid_in[sp];
		s_rs_prev.stem_l[sp] = s->prev_l_in[sp];
		s_rs_prev.stem_r[sp] = s->prev_r_in[sp];
	}

	/* THE PRODUCTION PREDICATE, on the production entry state -- exactly as
	 * main.c computes it once per run. When it holds, everything below
	 * takes the SHARED-LANE path, so this differential is not "the header
	 * still works" but "the shared lane renders what the pre-extraction
	 * main.c rendered". */
	locked = st_rs_cursor_locked(s->frame_in_group, s->dirs, s->src_avail,
				      frac, s_rs_prev_valid);
	if (locked) {
		s_cov.locked_runs++;
	} else {
		s_cov.unlocked_runs++;
	}

	for (k = 0; k < s->n; k++) {
		st11_audio_frame_t frame;
		st11_audio_frame_t nxt;

		st_rs_cursor_index(s->frame_in_group, s->dirs, s->src_avail, cur, idx, locked);
		st_rs_cursor_fetch(grp, idx, &nxt, locked);
		st_rs_cursor_prime(&nxt, &s_rs_prev, s_rs_prev_valid, locked);
		/* THE BLEND, transcribed from main.c exactly as the reference
		 * arm transcribes it -- see this file's own comment. */
		for (sp = 0; sp < ST11_STEM_COUNT; sp++) {
			const int32_t pl = s_rs_prev.stem_l[sp];
			const int32_t pr = s_rs_prev.stem_r[sp];

			frame.stem_l[sp] = pl +
				(int32_t)(((int64_t)(nxt.stem_l[sp] - pl) *
					    (int32_t)frac[sp]) >> 16);
			frame.stem_r[sp] = pr +
				(int32_t)(((int64_t)(nxt.stem_r[sp] - pr) *
					    (int32_t)frac[sp]) >> 16);
		}
		st_rs_cursor_advance(grp, s->frame_in_group, s->dirs, s->src_avail, idx,
				      &nxt, s->rate_q16, frac, cur, &s_rs_prev, locked);

		for (sp = 0; sp < ST_PL_STEMS; sp++) {
			out->hash = hmix(out->hash, (uint32_t)frame.stem_l[sp]);
			out->hash = hmix(out->hash, (uint32_t)frame.stem_r[sp]);
			out->idx_hash = hmix(out->idx_hash, idx[sp]);
			out->cur_hash = hmix(out->cur_hash, cur[sp]);
		}
	}

	st_rs_cursor_finish(frac, cur, s->src_avail, out->frac_io, out->used_out, locked);
	for (sp = 0; sp < ST_PL_STEMS; sp++) {
		out->prev_l[sp] = s_rs_prev.stem_l[sp];
		out->prev_r[sp] = s_rs_prev.stem_r[sp];
		out->prev_valid[sp] = s_rs_prev_valid[sp];
	}
}

/* Compares EVERYTHING both arms produce. Returns true on identity. */
static bool same(const result_t *a, const result_t *b, const char *what)
{
	bool ok = true;
	uint32_t sp;

	if (a->hash != b->hash) {
		ok = false;
		printf("  %s: SAMPLE HASH 0x%08x vs 0x%08x\n", what, a->hash, b->hash);
	}
	if (a->idx_hash != b->idx_hash) {
		ok = false;
		printf("  %s: INDEX HASH 0x%08x vs 0x%08x\n", what, a->idx_hash, b->idx_hash);
	}
	if (a->cur_hash != b->cur_hash) {
		ok = false;
		printf("  %s: POST-WALK CURSOR HASH 0x%08x vs 0x%08x -- a lane was "
		       "left stale for the FX clock / meter to read\n", what,
		       a->cur_hash, b->cur_hash);
	}
	for (sp = 0; sp < ST_PL_STEMS; sp++) {
		if (a->frac_io[sp] != b->frac_io[sp]) {
			ok = false;
			printf("  %s: frac_io[%u] %u vs %u\n", what, sp,
			       a->frac_io[sp], b->frac_io[sp]);
		}
		if (a->used_out[sp] != b->used_out[sp]) {
			ok = false;
			printf("  %s: used_out[%u] %u vs %u\n", what, sp,
			       a->used_out[sp], b->used_out[sp]);
		}
		if (a->prev_l[sp] != b->prev_l[sp] || a->prev_r[sp] != b->prev_r[sp]) {
			ok = false;
			printf("  %s: prev[%u] (%d,%d) vs (%d,%d)\n", what, sp,
			       a->prev_l[sp], a->prev_r[sp], b->prev_l[sp], b->prev_r[sp]);
		}
		if (a->prev_valid[sp] != b->prev_valid[sp]) {
			ok = false;
			printf("  %s: prev_valid[%u] %d vs %d\n", what, sp,
			       (int)a->prev_valid[sp], (int)b->prev_valid[sp]);
		}
	}
	return ok;
}

static bool both_arms_agree(const groups_t *g, const spec_t *s, const char *what,
			     uint32_t *hash_out)
{
	result_t ra, rb;

	run_ref(g, s, &ra);
	run_new(g, s, &rb);
	if (hash_out != NULL) {
		*hash_out = ra.hash;
	}
	return same(&ra, &rb, what);
}

/* ===================================================================== */

/* A legal random run: every index the cursors can form must stay inside the
 * group, which is what the caller in main.c guarantees through run_k[]. */
static void random_spec(spec_t *s, bool allow_reverse, bool allow_divergent)
{
	uint32_t k;
	const uint32_t base_fig = rnd_range(0u, ST_PL_FRAMES_PER_GROUP - 1u);
	const uint32_t base_frac = rnd() & (ST_RS_ONE - 1u);
	const bool valid = (rnd() & 3u) != 0u;
	/*
	 * HALF THE SWEEP IS DELIBERATELY LOCKED-SHAPED.
	 *
	 * Left to chance, four independently randomized lanes almost never come
	 * out identical: the first version of this generator produced 943
	 * locked runs in 200,000, so the arm carrying the bit-identity proof
	 * for the SHARED path was exercising it 0.5% of the time. Ordinary
	 * four-forward playback is the case the optimisation exists for and the
	 * case that must be proven identical, so it gets half the sweep --
	 * still with the rate, position, run bound, carried fraction and
	 * interpolator validity all randomized, just randomized ONCE and
	 * applied to every lane.
	 */
	const bool want_locked = (rnd() & 1u) != 0u;
	const uint32_t short_all = ((rnd() & 1u) != 0u)
				   ? rnd_range(1u, ST_PL_FRAMES_PER_GROUP) : 0u;

	if (want_locked) {
		allow_reverse = false;
		allow_divergent = false;
	}

	s->rate_q16 = rnd_range(ST_RS_ONE / 4u, ST_RS_RATE_MAX);
	s->n = rnd_range(1u, MAX_OUT);
	for (k = 0; k < ST_PL_STEMS; k++) {
		uint32_t fig = base_fig;
		int8_t d = 1;

		if (allow_divergent && (rnd() & 3u) == 0u) {
			fig = rnd_range(0u, ST_PL_FRAMES_PER_GROUP - 1u);
		}
		if (allow_reverse && (rnd() & 3u) == 0u) {
			d = -1;
		}
		s->frame_in_group[k] = fig;
		s->dirs[k] = d;
		/* The run a head may consume without leaving its group, in its
		 * own direction -- main.c's own rule (rk = FRAMES - fis
		 * forward, fis + 1 backward). Then randomly SHORTENED, which is
		 * what a song end, a loop edge or a starved lane does. */
		s->src_avail[k] = (d < 0) ? (fig + 1u)
					  : (ST_PL_FRAMES_PER_GROUP - fig);
		if (want_locked) {
			/* Same shortening for every lane, so the run bound
			 * stays equal -- a song end, a loop edge or a starved
			 * lane is what makes it differ, and that is the
			 * UNLOCKED half's job. */
			if (short_all != 0u && short_all < s->src_avail[k]) {
				s->src_avail[k] = short_all;
			}
		} else if ((rnd() & 1u) != 0u) {
			s->src_avail[k] = rnd_range(1u, s->src_avail[k]);
		}
		s->frac_in[k] = (!want_locked && (rnd() & 3u) == 0u)
				? (rnd() & (ST_RS_ONE - 1u)) : base_frac;
		s->prev_valid_in[k] = want_locked ? valid
						  : (valid || ((rnd() & 1u) != 0u));
		s->prev_l_in[k] = (int32_t)(rnd() % 60000u) - 30000;
		s->prev_r_in[k] = (int32_t)(rnd() % 60000u) - 30000;
	}
}

static void test_random_sweep(void)
{
	groups_t g;
	spec_t s;
	uint32_t t;
	uint32_t bad = 0;

	CASE("200,000 randomized runs: every sample, index, fraction, source "
	     "count and interpolator state identical");
	for (t = 0; t < 200000u; t++) {
		if ((t % 512u) == 0u) {
			fill_groups(&g, t);
		}
		random_spec(&s, true, true);
		if (!both_arms_agree(&g, &s, "sweep", NULL)) {
			bad++;
			if (bad > 3u) {
				break;
			}
		}
	}
	CHECK(bad == 0u, "%u randomized runs differed between the arms", bad);
}

/* The real rates the rocker produces, from the production table. */
static uint32_t rate_for_half(int8_t half)
{
	st_pitch_t p;

	st_pitch_reset(&p);
	p.half = half;
	return st_rs_rate_clamp(st_pitch_ratio_q16(&p));
}

/*
 * THE PINNED NON-UNITY HASH. A scripted mini-playback: four heads together,
 * forward, carrying frac and position across 96 consecutive runs per rate, at
 * the six rocker positions the hardware symptom lives at. Both arms must
 * produce it, and it is pinned so a LATER commit -- the shared-lane one -- has
 * a number to reproduce rather than a promise to keep.
 */
/*
 * MEASURED ON THE PRE-EXTRACTION IMPLEMENTATION and pinned here. This is the
 * number Commit 2 (the shared-lane cursor) has to reproduce; without it, "the
 * output did not change" would be a promise rather than an assertion. It is a
 * hash of the CURSOR AND BLEND path only -- not of the whole streaming chain,
 * which is what test_stem_playback_gate.c's 0x2a737e00 covers at unity.
 */
#define PINNED_NON_UNITY_HASH 0xbd69ac9cu

static uint32_t scripted_non_unity(bool use_new)
{
	static const int8_t halves[6] = { 1, 2, 3, 4, 5, -4 };
	groups_t g;
	uint32_t h = 2166136261u;
	uint32_t hi;

	for (hi = 0; hi < 6u; hi++) {
		spec_t s;
		result_t r;
		uint32_t fig = 0u;
		uint32_t frac[ST_PL_STEMS] = { 0u, 0u, 0u, 0u };
		uint32_t group_no = 0u;
		uint32_t run;

		fill_groups(&g, group_no);
		for (run = 0; run < 96u; run++) {
			uint32_t k;

			for (k = 0; k < ST_PL_STEMS; k++) {
				s.frame_in_group[k] = fig;
				s.dirs[k] = 1;
				s.src_avail[k] = ST_PL_FRAMES_PER_GROUP - fig;
				s.frac_in[k] = frac[k];
				s.prev_valid_in[k] = (run != 0u);
				s.prev_l_in[k] = 0;
				s.prev_r_in[k] = 0;
			}
			s.rate_q16 = rate_for_half(halves[hi]);
			s.n = MAX_OUT;

			if (use_new) {
				run_new(&g, &s, &r);
			} else {
				run_ref(&g, &s, &r);
			}
			h = hmix(h, r.hash);
			for (k = 0; k < ST_PL_STEMS; k++) {
				h = hmix(h, r.frac_io[k]);
				h = hmix(h, r.used_out[k]);
				frac[k] = r.frac_io[k];
			}
			/* Advance into the next group when this one is spent --
			 * all four consume the same count here. */
			fig += r.used_out[0];
			if (fig >= ST_PL_FRAMES_PER_GROUP - 1u) {
				fig = 0u;
				group_no++;
				fill_groups(&g, group_no);
			}
		}
	}
	return h;
}

static void test_pinned_non_unity_hash(void)
{
	uint32_t ref_h, new_h;

	CASE("pinned non-unity playback hash: reference and production agree");
	ref_h = scripted_non_unity(false);
	new_h = scripted_non_unity(true);
	printf("  non-unity playback hash (pre-extraction reference): 0x%08x\n", ref_h);
	printf("  non-unity playback hash (st_rs_cursor.h production): 0x%08x\n", new_h);
	CHECK(ref_h == new_h,
	      "the extraction changed scripted non-unity playback: 0x%08x != 0x%08x",
	      ref_h, new_h);
	CHECK(ref_h != 2166136261u, "the scripted playback rendered nothing");
#if PINNED_NON_UNITY_HASH != 0u
	CHECK(new_h == (uint32_t)PINNED_NON_UNITY_HASH,
	      "non-unity hash 0x%08x != pinned 0x%08x", new_h,
	      (uint32_t)PINNED_NON_UNITY_HASH);
#endif
}

/*
 * A HASH IS ONLY AS GOOD AS THE STATES IT REACHED. E-2 in this repository
 * printed "runs unconditionally" while checking three call orderings; a sweep
 * that never enters a branch proves nothing about it and must say so.
 */
static void test_sweep_reached_every_branch(void)
{
	CASE("the sweep actually entered every branch it claims to cover");
	CHECK(s_cov.shared_decode > 1000u,
	      "shared decode reached only %u times", s_cov.shared_decode);
	CHECK(s_cov.array_decode > 1000u,
	      "the four-index array decode reached only %u times -- divergent "
	      "cursors are the reason that path exists", s_cov.array_decode);
	CHECK(s_cov.real_prev_decode > 1000u,
	      "the real re-decode of the frame behind the cursor reached only "
	      "%u times -- rates above 2x per output frame", s_cov.real_prev_decode);
	CHECK(s_cov.out_of_run > 100u,
	      "the floored out-of-run corner reached only %u times",
	      s_cov.out_of_run);
	CHECK(s_cov.clamped_cursor > 100u,
	      "the cur >= src_avail clamp reached only %u times",
	      s_cov.clamped_cursor);
	CHECK(s_cov.primed > 1000u,
	      "a lane with prev_valid false was set up only %u times",
	      s_cov.primed);
	CHECK(s_cov.reversed_lane > 1000u,
	      "a reversed lane was set up only %u times", s_cov.reversed_lane);
}

/*
 * THE st63 STATE, by name. One stem has just left reverse: main.c seeks it to
 * MASTER and clears ONLY that lane's s_rs_prev_valid[j] and
 * s_stem_rate_frac[j]. All four are then co-located and forward, while one
 * carries frac 0 and the others carry a fraction. This is the state a shared
 * cursor would get wrong, so it is pinned here in the commit BEFORE the shared
 * cursor exists.
 */
static void test_st63_reverse_release_state(void)
{
	groups_t g;
	spec_t s;
	uint32_t j, k;
	uint32_t hi;

	CASE("st63 reverse-release: one lane's frac and prev_valid cleared, the "
	     "other three carrying");
	fill_groups(&g, 7u);
	for (hi = 1u; hi <= 5u; hi++) {
		for (j = 0; j < ST_PL_STEMS; j++) {
			char what[64];

			for (k = 0; k < ST_PL_STEMS; k++) {
				s.frame_in_group[k] = 100u;
				s.dirs[k] = 1;
				s.src_avail[k] = ST_PL_FRAMES_PER_GROUP - 100u;
				s.frac_in[k] = (k == j) ? 0u : 40000u;
				s.prev_valid_in[k] = (k != j);
				s.prev_l_in[k] = 1234 + (int32_t)k;
				s.prev_r_in[k] = -4321 - (int32_t)k;
			}
			s.rate_q16 = rate_for_half((int8_t)hi);
			s.n = MAX_OUT;
			snprintf(what, sizeof(what), "st63 lane %u at +%u/2 st", j, hi);
			CHECK(both_arms_agree(&g, &s, what, NULL),
			      "%s differed between the arms", what);
		}
	}
}

/* One reversed lane among three forward ones, at every lane position and both
 * near the group edges -- the state the four-index array decode exists for. */
static void test_reversed_lane(void)
{
	groups_t g;
	spec_t s;
	uint32_t j, k, e;
	static const uint32_t edges[3] = { 0u, 255u, ST_PL_FRAMES_PER_GROUP - 1u };

	CASE("one reversed lane among three forward, at both group edges");
	fill_groups(&g, 11u);
	for (e = 0; e < 3u; e++) {
		for (j = 0; j < ST_PL_STEMS; j++) {
			char what[64];

			for (k = 0; k < ST_PL_STEMS; k++) {
				const uint32_t fig = edges[e];

				s.frame_in_group[k] = fig;
				s.dirs[k] = (k == j) ? -1 : 1;
				s.src_avail[k] = (k == j)
						 ? (fig + 1u)
						 : (ST_PL_FRAMES_PER_GROUP - fig);
				s.frac_in[k] = 12345u;
				s.prev_valid_in[k] = true;
				s.prev_l_in[k] = 0;
				s.prev_r_in[k] = 0;
			}
			s.rate_q16 = rate_for_half(4);
			s.n = MAX_OUT;
			snprintf(what, sizeof(what), "reverse lane %u edge %u", j, e);
			CHECK(both_arms_agree(&g, &s, what, NULL),
			      "%s differed between the arms", what);
		}
	}
}

/* The structural ceiling, where the walk takes two source frames per output
 * frame every frame and the out-of-run corner is routine. */
static void test_rate_extremes(void)
{
	groups_t g;
	spec_t s;
	uint32_t k, i;
	static const uint32_t rates[4] = { 1u, ST_RS_ONE - 1u, ST_RS_ONE + 1u,
					    ST_RS_RATE_MAX };

	CASE("rate extremes: 1, just under 1x, just over 1x, and the 2x ceiling");
	fill_groups(&g, 23u);
	for (i = 0; i < 4u; i++) {
		char what[48];

		for (k = 0; k < ST_PL_STEMS; k++) {
			s.frame_in_group[k] = 400u;
			s.dirs[k] = 1;
			s.src_avail[k] = 40u; /* deliberately shorter than n */
			s.frac_in[k] = 60000u;
			s.prev_valid_in[k] = false;
			s.prev_l_in[k] = 0;
			s.prev_r_in[k] = 0;
		}
		s.rate_q16 = rates[i];
		s.n = MAX_OUT;
		snprintf(what, sizeof(what), "rate %u", rates[i]);
		CHECK(both_arms_agree(&g, &s, what, NULL),
		      "%s differed between the arms", what);
	}
}

int main(void)
{
	printf("Stem Tape resampler-cursor extraction gate\n");
	printf("st_rs_cursor.h (production, linked) vs the frozen "
	       "pre-extraction main.c text\n\n");

	test_random_sweep();
	test_sweep_reached_every_branch();
	test_pinned_non_unity_hash();
	test_st63_reverse_release_state();
	test_reversed_lane();
	test_rate_extremes();

	printf("\ncoverage: shared_decode=%u array_decode=%u real_prev_decode=%u "
	       "out_of_run=%u clamped=%u primed=%u reversed=%u\n",
	       s_cov.shared_decode, s_cov.array_decode, s_cov.real_prev_decode,
	       s_cov.out_of_run, s_cov.clamped_cursor, s_cov.primed,
	       s_cov.reversed_lane);
	printf("shared-lane path: locked_runs=%u unlocked_runs=%u\n",
	       s_cov.locked_runs, s_cov.unlocked_runs);
	printf("\n%d cases, %d checks, %d failures\n", g_cases, g_checks, g_failures);
	return g_failures ? 1 : 0;
}
