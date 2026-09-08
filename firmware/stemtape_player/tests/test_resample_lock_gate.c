/*
 * test_resample_lock_gate.c -- the SHARED-LANE cursor: it activates exactly
 * when the four cursors are provably one cursor, it never activates when they
 * are not, and it publishes every lane before anything downstream reads one.
 *
 * ======================================================================
 * WHAT THIS GATE IS FOR, AND WHAT test_rs_cursor_gate.c ALREADY DOES
 * ======================================================================
 * Bit-identity is proved next door: test_rs_cursor_gate.c runs 200,000
 * randomized specs -- half of them deliberately locked-shaped -- against a
 * frozen transcription of the PRE-EXTRACTION main.c text, and compares every
 * blended sample, index, fraction, source count and interpolator state. That
 * gate answers "does the shared lane render what the original rendered".
 *
 * This one answers the three questions it cannot:
 *
 *   SOUND        the predicate must refuse every state where sharing would be
 *                wrong. Five ways to diverge, each pinned as a separate case,
 *                each with the production st_rs_cursor_locked() consulted
 *                directly rather than a copy of its rules.
 *   NOT VACUOUS  the predicate must be TRUE for ordinary four-forward playback
 *                at every rocker position the hardware symptom lives at. A
 *                predicate that is always false is bit-identical, costs
 *                nothing, and buys nothing -- and every soundness case above
 *                would still pass. E-2 in this repository printed "runs
 *                unconditionally" while checking three call orderings; a
 *                shared path that never activates is the same failure wearing
 *                a different hat.
 *   PUBLISHED    st_fx_process()'s clock offset reads cur[s_fx_target] and the
 *                per-stem meter reads g_stem_zero_at[sp] = song_frame +
 *                cur[sp], both AFTER the walk, both per lane. A shared walk
 *                that leaves lanes 1-3 holding the previous output frame's
 *                cursor is INAUDIBLE in the mix and wrong in the echo's time
 *                index and the dropout detector's reported position. This gate
 *                models both readers.
 *
 * The two arms here are LOCKED and FORCED-UNLOCKED over the same production
 * header, which isolates the shared lane from the extraction: any difference
 * is the lock's fault and nothing else's.
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

static uint32_t s_rng = 0x9e3779b9u;

static uint32_t rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

static uint32_t rnd_range(uint32_t lo, uint32_t hi)
{
	return lo + (rnd() % (hi - lo + 1u));
}

typedef struct {
	uint8_t b[ST_PL_STEMS][ST_PL_GROUP_BYTES];
} groups_t;

static void fill_groups(groups_t *g, uint32_t salt)
{
	uint32_t k, f;

	memset(g, 0, sizeof(*g));
	for (k = 0; k < ST_PL_STEMS; k++) {
		for (f = 0; f < ST_PL_FRAMES_PER_GROUP; f++) {
			const uint32_t off = st_pl_frame_off(f);
			const int32_t l = (int32_t)((f * 331u + k * 5011u + salt * 197u) % 60000u) - 30000;
			const int32_t r = (int32_t)((f * 811u + k * 2003u + salt * 887u) % 60000u) - 30000;
			const uint32_t w = ((uint32_t)(uint16_t)(int16_t)l) |
					   (((uint32_t)(uint16_t)(int16_t)r) << 16);

			memcpy(g->b[k] + off, &w, sizeof(w));
		}
	}
}

#define MAX_OUT 256u

typedef struct {
	uint32_t frame_in_group[ST_PL_STEMS];
	int8_t   dirs[ST_PL_STEMS];
	uint32_t src_avail[ST_PL_STEMS];
	uint32_t frac_in[ST_PL_STEMS];
	bool     prev_valid_in[ST_PL_STEMS];
	uint32_t rate_q16;
	uint32_t n;
} spec_t;

typedef struct {
	uint32_t sample_hash;
	/* THE TWO DOWNSTREAM READERS, MODELLED. fx_hash is what
	 * st_fx_process() would be handed as its time index; meter_hash is what
	 * g_stem_zero_at[sp] would be set to. Both read cur[] per lane, after
	 * the walk, exactly as main.c does. */
	uint32_t fx_hash;
	uint32_t meter_hash;
	uint32_t frac_io[ST_PL_STEMS];
	uint32_t used_out[ST_PL_STEMS];
	int32_t  prev_l[ST_PL_STEMS];
	int32_t  prev_r[ST_PL_STEMS];
} result_t;

static uint32_t hmix(uint32_t h, uint32_t v)
{
	h ^= v;
	h *= 16777619u;
	return h;
}

/* One run through the PRODUCTION header, with `locked` forced by the caller so
 * the two paths can be compared over identical state. */
static void run(const groups_t *g, const spec_t *s, bool locked, result_t *out)
{
	const uint8_t *grp[ST_PL_STEMS];
	uint32_t frac[ST_PL_STEMS], cur[ST_PL_STEMS], idx[ST_PL_STEMS];
	uint32_t sp, k;
	st11_audio_frame_t prev;
	bool prev_valid[ST_PL_STEMS];
	/* Stand-ins for main.c's own values. Any fixed numbers do; what matters
	 * is that the offset added to them is cur[], per lane. */
	const uint32_t song_frame = 100000u;
	const uint32_t fx_target = 2u;

	memset(out, 0, sizeof(*out));
	out->sample_hash = 2166136261u;
	out->fx_hash = 2166136261u;
	out->meter_hash = 2166136261u;
	for (sp = 0; sp < ST_PL_STEMS; sp++) {
		grp[sp] = g->b[sp];
		frac[sp] = s->frac_in[sp];
		cur[sp] = 0u;
		prev_valid[sp] = s->prev_valid_in[sp];
		/* OUT OF THE GROUP'S RANGE ON PURPOSE. fill_groups() only ever
		 * writes samples in [-30000, 29999], so a lane that is never
		 * primed carries a value no decode can produce and the
		 * difference is guaranteed observable rather than accidentally
		 * masked. */
		prev.stem_l[sp] = 30001;
		prev.stem_r[sp] = -30001;
	}

	for (k = 0; k < s->n; k++) {
		st11_audio_frame_t frame;
		st11_audio_frame_t nxt;

		st_rs_cursor_index(s->frame_in_group, s->dirs, s->src_avail, cur, idx, locked);
		st_rs_cursor_fetch(grp, idx, &nxt, locked);
		st_rs_cursor_prime(&nxt, &prev, prev_valid, locked);
		for (sp = 0; sp < ST11_STEM_COUNT; sp++) {
			const int32_t pl = prev.stem_l[sp];
			const int32_t pr = prev.stem_r[sp];

			frame.stem_l[sp] = pl +
				(int32_t)(((int64_t)(nxt.stem_l[sp] - pl) *
					    (int32_t)frac[sp]) >> 16);
			frame.stem_r[sp] = pr +
				(int32_t)(((int64_t)(nxt.stem_r[sp] - pr) *
					    (int32_t)frac[sp]) >> 16);
		}
		st_rs_cursor_advance(grp, s->frame_in_group, s->dirs, s->src_avail, idx,
				      &nxt, s->rate_q16, frac, cur, &prev, locked);

		/* ---- THE DOWNSTREAM READERS, in main.c's own order ---- */
		/* st_fx_process(&g_stem_fx, &fl, &fr,
		 *               heads[s_fx_target].song_frame +
		 *               dirs[s_fx_target] * cur[s_fx_target]); */
		out->fx_hash = hmix(out->fx_hash,
				     (uint32_t)((int32_t)song_frame +
						 s->dirs[fx_target] * (int32_t)cur[fx_target]));
		/* g_stem_zero_at[sp] = song_frame + cur[sp], one frame in 32 */
		if ((k & 31u) == 0u) {
			for (sp = 0; sp < ST_PL_STEMS; sp++) {
				out->meter_hash = hmix(out->meter_hash,
							song_frame + cur[sp]);
			}
		}
		for (sp = 0; sp < ST_PL_STEMS; sp++) {
			out->sample_hash = hmix(out->sample_hash, (uint32_t)frame.stem_l[sp]);
			out->sample_hash = hmix(out->sample_hash, (uint32_t)frame.stem_r[sp]);
		}
	}

	st_rs_cursor_finish(frac, cur, s->src_avail, out->frac_io, out->used_out, locked);
	for (sp = 0; sp < ST_PL_STEMS; sp++) {
		out->prev_l[sp] = prev.stem_l[sp];
		out->prev_r[sp] = prev.stem_r[sp];
	}
}

static bool same(const result_t *a, const result_t *b, const char *what)
{
	bool ok = true;
	uint32_t sp;

	if (a->sample_hash != b->sample_hash) {
		ok = false;
		printf("  %s: SAMPLES 0x%08x vs 0x%08x\n", what, a->sample_hash, b->sample_hash);
	}
	if (a->fx_hash != b->fx_hash) {
		ok = false;
		printf("  %s: FX CLOCK OFFSET 0x%08x vs 0x%08x -- st_fx_process() would "
		       "be handed a different time index\n", what, a->fx_hash, b->fx_hash);
	}
	if (a->meter_hash != b->meter_hash) {
		ok = false;
		printf("  %s: METER POSITION 0x%08x vs 0x%08x -- g_stem_zero_at[] would "
		       "report a dropout at the wrong frame\n", what, a->meter_hash, b->meter_hash);
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
	}
	return ok;
}

/* Locked and forced-unlocked over identical state must be indistinguishable in
 * every observable, including the two downstream readers. */
static bool lock_is_transparent(const groups_t *g, const spec_t *s, const char *what)
{
	result_t a, b;

	run(g, s, true, &a);
	run(g, s, false, &b);
	return same(&a, &b, what);
}

/*
 * THE OTHER HALF OF SOUNDNESS. Refusing a state is only load-bearing if
 * sharing it would actually have been wrong; a predicate that refuses states
 * the lock would have handled correctly is merely conservative, and one whose
 * refusals are all decorative is untested. Every divergence case therefore
 * asserts BOTH: the predicate says no, and forcing it to say yes changes the
 * audio.
 */
static bool lock_would_corrupt(const groups_t *g, const spec_t *s)
{
	result_t a, b;

	run(g, s, true, &a);
	run(g, s, false, &b);
	if (a.sample_hash != b.sample_hash) {
		return true;
	}
	if (a.fx_hash != b.fx_hash || a.meter_hash != b.meter_hash) {
		return true;
	}
	{
		uint32_t sp;

		for (sp = 0; sp < ST_PL_STEMS; sp++) {
			if (a.frac_io[sp] != b.frac_io[sp] ||
			    a.used_out[sp] != b.used_out[sp] ||
			    a.prev_l[sp] != b.prev_l[sp] ||
			    a.prev_r[sp] != b.prev_r[sp]) {
				return true;
			}
		}
	}
	return false;
}

static uint32_t rate_for_half(int8_t half)
{
	st_pitch_t p;

	st_pitch_reset(&p);
	p.half = half;
	return st_rs_rate_clamp(st_pitch_ratio_q16(&p));
}

static void locked_spec(spec_t *s, uint32_t fig, uint32_t frac, uint32_t rate,
			 uint32_t n)
{
	uint32_t k;

	for (k = 0; k < ST_PL_STEMS; k++) {
		s->frame_in_group[k] = fig;
		s->dirs[k] = 1;
		s->src_avail[k] = ST_PL_FRAMES_PER_GROUP - fig;
		s->frac_in[k] = frac;
		s->prev_valid_in[k] = true;
	}
	s->rate_q16 = rate;
	s->n = n;
}

static uint32_t s_locked_seen;
static uint32_t s_unlocked_seen;

static bool predicate(const spec_t *s)
{
	bool v = st_rs_cursor_locked(s->frame_in_group, s->dirs, s->src_avail,
				      s->frac_in, s->prev_valid_in);

	if (v) {
		s_locked_seen++;
	} else {
		s_unlocked_seen++;
	}
	return v;
}

/* ===================================================================== */

/*
 * NOT VACUOUS. If this fails, the optimisation is dead code: every soundness
 * case below would still pass, the audio would still be bit-identical, and the
 * change would buy exactly nothing.
 */
static void test_locked_actually_activates(void)
{
	static const int8_t halves[5] = { 1, 2, 3, 4, 5 };
	static const char *const names[5] = { "+0.5", "+1.0", "+1.5", "+2.0", "+2.5" };
	groups_t g;
	uint32_t i;

	CASE("the shared lane ACTIVATES for ordinary four-forward playback at "
	     "every rocker position");
	fill_groups(&g, 3u);
	for (i = 0; i < 5u; i++) {
		spec_t s;
		uint32_t fig;

		for (fig = 0; fig < ST_PL_FRAMES_PER_GROUP; fig += 97u) {
			locked_spec(&s, fig, (fig * 613u) & (ST_RS_ONE - 1u),
				     rate_for_half(halves[i]), MAX_OUT);
			CHECK(predicate(&s),
			      "%s st: the predicate refused ordinary four-forward "
			      "playback at frame_in_group %u -- the shared lane "
			      "would never run", names[i], fig);
			CHECK(lock_is_transparent(&g, &s, names[i]),
			      "%s st at fig %u: locked and unlocked differ",
			      names[i], fig);
		}
	}
	/* And the slow-playback direction, which is the same path below 1x. */
	{
		spec_t s;

		locked_spec(&s, 200u, 12345u, rate_for_half(-4), MAX_OUT);
		CHECK(predicate(&s), "-2.0 st: the predicate refused four-forward playback");
		CHECK(lock_is_transparent(&g, &s, "-2.0 st"), "-2.0 st differs");
	}
}

/*
 * SOUND. Five ways the lanes can diverge; the predicate must refuse every one,
 * and the unlocked path must then run unchanged. Each case is built from a
 * LOCKED spec and then broken in exactly one place, so nothing else can be the
 * reason it is refused.
 */
static void test_predicate_refuses_every_divergence(void)
{
	groups_t g;
	spec_t s;
	uint32_t j;

	fill_groups(&g, 5u);

	CASE("a reversed lane is refused");
	for (j = 0; j < ST_PL_STEMS; j++) {
		uint32_t k;

		/* DIRECTION ALONE. 50 frames is a legal run for a forward lane
		 * at frame 300 (510-300=210) and for a reversed one (300+1),
		 * so every other condition stays equal and `dirs` is the only
		 * thing that differs. */
		locked_spec(&s, 300u, 20000u, rate_for_half(4), MAX_OUT);
		for (k = 0; k < ST_PL_STEMS; k++) {
			s.src_avail[k] = 50u;
		}
		s.dirs[j] = -1;
		CHECK(!predicate(&s), "lane %u reversed but the predicate said locked", j);
		CHECK(lock_would_corrupt(&g, &s),
		      "lane %u reversed: forcing the lock changed nothing, so this "
		      "refusal proves nothing", j);
	}

	CASE("a lane at a different frame_in_group is refused");
	for (j = 0; j < ST_PL_STEMS; j++) {
		locked_spec(&s, 120u, 20000u, rate_for_half(4), MAX_OUT);
		s.frame_in_group[j] = 121u;
		s.src_avail[j] = ST_PL_FRAMES_PER_GROUP - 121u;
		CHECK(!predicate(&s), "lane %u displaced but the predicate said locked", j);
		CHECK(lock_would_corrupt(&g, &s),
		      "lane %u displaced: forcing the lock changed nothing", j);
	}

	CASE("THE st63 STATE: one lane's carried fraction cleared, three carrying");
	for (j = 0; j < ST_PL_STEMS; j++) {
		uint32_t hi;

		for (hi = 1u; hi <= 5u; hi++) {
			locked_spec(&s, 150u, 40000u, rate_for_half((int8_t)hi), MAX_OUT);
			s.frac_in[j] = 0u;   /* main.c:3839 / 3852 -- the leaver only */
			CHECK(!predicate(&s),
			      "lane %u at +%u/2 st: fraction 0 against three at 40000 "
			      "and the predicate said locked. This is the state one "
			      "block after a reverse release, and sharing here renders "
			      "that stem at the other three's sub-sample phase",
			      j, hi);
			CHECK(lock_would_corrupt(&g, &s),
			      "lane %u at +%u/2 st: forcing the lock changed nothing, "
			      "so the fraction condition is untested here", j, hi);
		}
	}

	CASE("a lane with a different run bound is refused");
	for (j = 0; j < ST_PL_STEMS; j++) {
		locked_spec(&s, 400u, 60000u, ST_RS_RATE_MAX, 64u);
		s.src_avail[j] = 3u;   /* a starved lane, or a song/loop edge */
		CHECK(!predicate(&s), "lane %u short-bounded but the predicate said locked", j);
		CHECK(lock_would_corrupt(&g, &s),
		      "lane %u short-bounded: forcing the lock changed nothing", j);
	}

	CASE("a lane with a different interpolator validity is refused");
	for (j = 0; j < ST_PL_STEMS; j++) {
		locked_spec(&s, 200u, 30000u, rate_for_half(3), MAX_OUT);
		s.prev_valid_in[j] = false;
		CHECK(!predicate(&s), "lane %u unprimed but the predicate said locked", j);
		CHECK(lock_would_corrupt(&g, &s),
		      "lane %u unprimed: forcing the lock changed nothing -- it would "
		      "read lane 0's flag and leave this lane interpolating from a "
		      "position it no longer occupies", j);
	}
}

/*
 * PUBLISHED. The broadcast has to happen before anything downstream reads a
 * lane. This walks a long run and compares the FX clock offset and the meter
 * position frame by frame -- the two readers named in the hazard list.
 */
static void test_every_lane_is_published(void)
{
	groups_t g;
	spec_t s;
	uint32_t i;

	CASE("every lane's cursor is published before the FX clock and the meter "
	     "read it");
	fill_groups(&g, 9u);
	for (i = 1u; i <= 5u; i++) {
		result_t a, b;
		uint32_t sp;

		locked_spec(&s, 7u, 0u, rate_for_half((int8_t)i), MAX_OUT);
		run(&g, &s, true, &a);
		run(&g, &s, false, &b);
		CHECK(a.fx_hash == b.fx_hash,
		      "+%u/2 st: the FX rack's time index differs between the shared "
		      "and independent cursors", i);
		CHECK(a.meter_hash == b.meter_hash,
		      "+%u/2 st: the per-stem meter would report a dropout at a "
		      "different song frame", i);
		for (sp = 0; sp < ST_PL_STEMS; sp++) {
			CHECK(a.used_out[sp] == b.used_out[sp],
			      "+%u/2 st: used_out[%u] %u vs %u -- a playhead would be "
			      "advanced by a distance it did not travel",
			      i, sp, a.used_out[sp], b.used_out[sp]);
		}
	}
}

/* A broad randomized transparency sweep over locked-shaped states only: the
 * lock must be invisible for every rate, position, bound and phase. */
static void test_random_locked_sweep(void)
{
	groups_t g;
	uint32_t t;
	uint32_t bad = 0;

	CASE("60,000 randomized locked-shaped runs: the lock is invisible");
	for (t = 0; t < 60000u; t++) {
		spec_t s;
		uint32_t fig;

		if ((t % 256u) == 0u) {
			fill_groups(&g, t);
		}
		fig = rnd_range(0u, ST_PL_FRAMES_PER_GROUP - 1u);
		locked_spec(&s, fig, rnd() & (ST_RS_ONE - 1u),
			     rnd_range(ST_RS_ONE / 4u, ST_RS_RATE_MAX),
			     rnd_range(1u, MAX_OUT));
		if ((rnd() & 1u) != 0u) {
			const uint32_t shorter = rnd_range(1u, s.src_avail[0]);
			uint32_t k;

			for (k = 0; k < ST_PL_STEMS; k++) {
				s.src_avail[k] = shorter;
			}
		}
		if (!predicate(&s)) {
			bad++;
			printf("  a locked-shaped spec was refused at t=%u\n", t);
			if (bad > 3u) {
				break;
			}
		}
		if (!lock_is_transparent(&g, &s, "locked sweep")) {
			bad++;
			if (bad > 3u) {
				break;
			}
		}
	}
	CHECK(bad == 0u, "%u randomized locked runs were not transparent", bad);
}

int main(void)
{
	printf("Stem Tape shared-lane resampler cursor gate\n");
	printf("st_rs_cursor_locked() + the locked path, both from the production "
	       "header\n\n");

	test_locked_actually_activates();
	test_predicate_refuses_every_divergence();
	test_every_lane_is_published();
	test_random_locked_sweep();

	printf("\npredicate: locked=%u refused=%u\n", s_locked_seen, s_unlocked_seen);
	printf("\n%d cases, %d checks, %d failures\n", g_cases, g_checks, g_failures);
	if (g_failures == 0) {
		printf("RESAMPLE LOCK GATE PASSED\n");
	}
	return g_failures ? 1 : 0;
}
