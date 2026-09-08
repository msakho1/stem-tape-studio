/*
 * test_loop_pitch_gate.c -- a loop, at pitch, keeps its four ordinary forward
 * stems on ONE position, through engage, wraps, both resizes, release, a
 * starvation episode, and a reversed stem.
 *
 * ======================================================================
 * THE DEFECT THIS GATE EXISTS FOR
 * ======================================================================
 * The authoritative loop wrap used to select which heads it moved by POSITION:
 *
 *     if (g_stem_stream[sk].song_frame != tr->song_frame) continue;
 *
 * That is the same category error the co-location guard made before st64 -- a
 * position standing in for an INTENT. It cannot tell "somewhere else because I
 * am deliberately reversed" from "somewhere else because I wrapped one run
 * early" or "somewhere else because I was starved".
 *
 * At unity it was harmless: the run clamp lands every head on loop_end
 * together and the duck's target is where the backstop already put them, so
 * all four move. Off unity the transport waits for the duck while the other
 * three wrap immediately, so by the time the duck fires they no longer match
 * and ONLY THE TRANSPORT MOVES. The leftover is d * (1 - 1/rate) per wrap:
 * permanent, cumulative across resizes, and all three reported hardware
 * symptoms at once -- the flam, the collapse of the shared-lane predicate that
 * produced the crackle and the apparent BPM drop, and the starved stem that
 * never came back.
 *
 * ======================================================================
 * WHAT THIS GATE IS, AND THE LIMIT IT INHERITS
 * ======================================================================
 * It links the REAL st_stem_stream.c, st_seam.h, st_resample.h, st_pitch.c and
 * the production st_rs_cursor.h, and drives them with a MODEL of the loop
 * block in main.c -- which cannot be linked on the host. So, like every other
 * transport gate here, it can agree with a main.c that has drifted from it.
 * The wiring check (J-1/J-2) is the other half and reads production main.c
 * directly.
 *
 * WHAT MAKES IT MORE THAN A MODEL: it runs BOTH guards over identical state
 * and requires the old one to FAIL. A gate that only shows the new code
 * working proves the code runs, not that the change mattered.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "st_pitch.h"
#include "st_planar.h"
#include "st_resample.h"
#include "st_rs_cursor.h"
#include "st_seam.h"
#include "st_stem_stream.h"
#include "st_v11_format.h"

static int g_checks, g_failures, g_cases;

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

#define BLK          256u
#define STEMS        ST_PL_STEMS
#define SONG_FRAMES  (510u * 400u)
#define SECTORS      400u
#define JUMP_WRAP    1u
#define LP_LO        (510u * 20u)

/* ---- the model's state ---------------------------------------------- */
static st_stream_t hd[STEMS];
static uint32_t    frac[STEMS];
static bool        pv[STEMS];
static st_seam_t   seam;
static uint32_t    jump_to;
static uint8_t     jump_pend;
static uint32_t    transport;

/* 0 = the OLD positional guard (pre-fix), 1 = the production explicit-state
 * guard. Both are compiled; the gate runs both and compares. */
static int g_guard_explicit = 1;

static uint32_t n_duck, n_duck_seeked, n_backstop, n_run0;
/* Seeks the authoritative wrap applied to a lane that was REVERSED at the
 * time. The reverse exclusion is a property about what the wrap does, so it
 * is asserted directly rather than inferred from where the lane ended up:
 * a dragged reversed head immediately resumes travelling backward, so a
 * position comparison a few blocks later cannot see it. */
static uint32_t n_reverse_seeked;
static uint32_t out_locked, out_unlocked, out_silent;

/* Throttle: starve one stem to reproduce the disappearing-stem report. */
static int      g_starve_stem = -1;
static uint32_t g_starving;

static void rs_drop(void)
{
	for (uint32_t k = 0; k < STEMS; k++) { frac[k] = 0u; pv[k] = false; }
}

static uint32_t rate_for(int8_t half)
{
	st_pitch_t p;

	st_pitch_reset(&p);
	p.half = half;
	return st_rs_rate_clamp(st_pitch_ratio_q16(&p));
}

/*
 * THE GUARD UNDER TEST. main.c's authoritative ST_SEAM_JUMP_WRAP participation
 * rule, both forms, so the gate can show the old one failing.
 *
 * MUTATION TARGET: stemtape_player_loop_pitch_mutations.py breaks exactly this
 * function, which is why it is one function and not four inline tests.
 */
static bool participates(uint32_t k, const st_stream_t *tr)
{
	if (!g_guard_explicit) {
		return hd[k].song_frame == tr->song_frame;   /* the OLD rule */
	}
	if (hd[k].reverse) {
		return false;                               /* independent by intent */
	}
	if (hd[k].state == ST_STREAM_START_OF_SONG ||
	    hd[k].state == ST_STREAM_END_OF_SONG) {
		return false;                               /* parked: consumes no source */
	}
	return true;
}

/* One audio block. The loop window, arm, jump, bounds and backstop all live
 * INSIDE the run loop, exactly as main.c has them (its run loop opens at
 * main.c:3762, above all of it). */
static void block(bool lp_on, uint32_t lp_lo, uint32_t lp_hi, uint32_t rate)
{
	st_stream_t *tr = &hd[transport];
	uint32_t f = 0;

	while (f < BLK) {
		uint32_t fig[STEMS], run_k[STEMS], needed[STEMS], used[STEMS];
		uint32_t run = ST11_FRAMES_PER_SECTOR, out_n, frac_max;
		uint32_t lp_end = lp_hi;
		int8_t dirs[STEMS];
		bool lp_live = lp_on, lk;

		if (lp_live && lp_hi <= lp_lo) { lp_live = false; }
		if (lp_live) {
			const uint32_t len = lp_hi - lp_lo;
			const uint32_t pos = tr->song_frame;

			if (pos >= lp_hi) {
				lp_end = lp_lo + ((pos - lp_lo) / len + 1u) * len;
			}
		}
		/* ---- the wrap arm (untouched by this fix) ---- */
		if (lp_live && !jump_pend && tr->song_frame >= lp_lo &&
		    tr->song_frame < lp_end &&
		    tr->song_frame + ST_SEAM_FRAMES >= lp_end) {
			jump_to = lp_lo;
			jump_pend = JUMP_WRAP;
			st_seam_begin_in(&seam, (uint16_t)(lp_end - tr->song_frame));
		}
		/* ---- THE AUTHORITATIVE JUMP ---- */
		if (jump_pend && st_seam_jump_due(&seam)) {
			n_duck++;
			rs_drop();
			for (uint32_t k = 0; k < STEMS; k++) {
				const bool was_reverse = hd[k].reverse;

				if (!participates(k, tr)) { continue; }
				if (st_stream_seek(&hd[k], jump_to)) {
					n_duck_seeked++;
					if (was_reverse) { n_reverse_seeked++; }
				}
			}
			jump_pend = 0u;
		}

		for (uint32_t k = 0; k < STEMS; k++) {
			uint32_t rk, left;

			needed[k] = st_stream_required_sector(&hd[k]);
			if ((int)k == g_starve_stem && g_starving) {
				hd[k].ready_sector = ST_STREAM_NO_SECTOR;
			} else {
				st_stream_sector_ready(&hd[k], needed[k]);
			}
			fig[k] = hd[k].song_frame - needed[k] * ST11_FRAMES_PER_SECTOR;
			dirs[k] = hd[k].reverse ? -1 : 1;
			if (hd[k].reverse) {
				rk = fig[k] + 1u;
				if (rk > hd[k].song_frame + 1u) { rk = hd[k].song_frame + 1u; }
				if (rk == 0u) { rk = 1u; }
			} else {
				rk = ST11_FRAMES_PER_SECTOR - fig[k];
				left = SONG_FRAMES - hd[k].song_frame;
				if (rk > left) { rk = left; }
			}
			/* the loop-window run clamp (untouched by this fix) */
			if (lp_live && !hd[k].reverse &&
			    hd[k].song_frame >= lp_lo && hd[k].song_frame < lp_end &&
			    rk > lp_end - hd[k].song_frame) {
				rk = lp_end - hd[k].song_frame;
			}
			run_k[k] = rk;
			if (rk < run) { run = rk; }
		}
		frac_max = frac[0];
		for (uint32_t k = 1; k < STEMS; k++) {
			if (frac[k] > frac_max) { frac_max = frac[k]; }
		}
		if (run == 0u) {
			n_run0++;
			st_seam_advance(&seam, BLK - f);
			out_silent += BLK - f;
			break;
		}
		out_n = st_rs_out_frames(run, frac_max, rate);
		if (out_n > BLK - f) { out_n = BLK - f; }
		if (jump_pend) {
			uint16_t tj = st_seam_frames_to_jump(&seam);

			if (tj > 0u && out_n > tj) { out_n = tj; }
		}
		if (out_n == 0u) {
			st_seam_advance(&seam, BLK - f);
			out_silent += BLK - f;
			break;
		}

		lk = st_rs_cursor_locked(fig, dirs, run_k, frac, pv);
		if (lk) { out_locked += out_n; } else { out_unlocked += out_n; }

		{
			static uint8_t gbuf[STEMS][ST_PL_GROUP_BYTES];
			const uint8_t *grp[STEMS];
			uint32_t cur[STEMS] = { 0u, 0u, 0u, 0u }, idx[STEMS];
			st11_audio_frame_t nxt, prev;

			for (uint32_t k = 0; k < STEMS; k++) {
				grp[k] = gbuf[k];
				prev.stem_l[k] = 0; prev.stem_r[k] = 0;
			}
			for (uint32_t i = 0; i < out_n; i++) {
				st_rs_cursor_index(fig, dirs, run_k, cur, idx, lk);
				st_rs_cursor_fetch(grp, idx, &nxt, lk);
				st_rs_cursor_prime(&nxt, &prev, pv, lk);
				st_rs_cursor_advance(grp, fig, dirs, run_k, idx, &nxt,
						      rate, frac, cur, &prev, lk);
				st_seam_tick(&seam);
			}
			for (uint32_t k = 0; k < STEMS; k++) {
				used[k] = (cur[k] > run_k[k]) ? run_k[k] : cur[k];
			}
		}
		f += out_n;
		for (uint32_t k = 0; k < STEMS; k++) {
			if ((int)k == g_starve_stem && g_starving) {
				continue;   /* UNDERRUN: song_frame frozen, silence out */
			}
			(void)st_stream_advance_frames(&hd[k], used[k]);
		}
		/* ---- the backstop (untouched by this fix) ---- */
		if (lp_live) {
			for (uint32_t k = 0; k < STEMS; k++) {
				bool wrapped = false;

				if (hd[k].reverse) {
					if (hd[k].song_frame < lp_lo && lp_end > 0u) {
						wrapped = st_stream_seek(&hd[k], lp_end - 1u);
					}
				} else if (k == transport && jump_pend == JUMP_WRAP) {
					continue;
				} else if (hd[k].song_frame >= lp_end) {
					wrapped = st_stream_seek(&hd[k], lp_lo);
				}
				if (wrapped) {
					pv[k] = false; frac[k] = 0u; n_backstop++;
				}
			}
		}
	}
}

/* ---- helpers --------------------------------------------------------- */

static void reset_all(void)
{
	for (uint32_t k = 0; k < STEMS; k++) {
		(void)st_stream_init(&hd[k], 0u, SECTORS * ST11_BLOCKS_PER_SECTOR,
				      SONG_FRAMES, SECTORS, false);
		st_stream_play(&hd[k]);
		(void)st_stream_seek(&hd[k], LP_LO);
		frac[k] = 0u; pv[k] = false;
	}
	transport = 0u;
	memset(&seam, 0, sizeof(seam));
	jump_pend = 0u;
	n_duck = n_duck_seeked = n_backstop = n_run0 = 0u;
	n_reverse_seeked = 0u;
	out_locked = out_unlocked = out_silent = 0u;
	g_starve_stem = -1; g_starving = 0u;
}

/* Spread across the ordinary forward stems only. */
static uint32_t fwd_spread(void)
{
	uint32_t lo = UINT32_MAX, hi = 0u;

	for (uint32_t k = 0; k < STEMS; k++) {
		if (hd[k].reverse) { continue; }
		if (hd[k].song_frame < lo) { lo = hd[k].song_frame; }
		if (hd[k].song_frame > hi) { hi = hd[k].song_frame; }
	}
	return (lo == UINT32_MAX) ? 0u : (hi - lo);
}

static double locked_pct(void)
{
	const uint32_t t = out_locked + out_unlocked;

	return t ? (100.0 * (double)out_locked / (double)t) : 0.0;
}

/* engage -> wraps -> shorten -> wraps -> lengthen -> wraps -> release. Returns
 * the worst forward spread seen at any stable checkpoint. */
static uint32_t lifecycle(int8_t half, uint32_t *spread_after_release)
{
	const uint32_t rate = rate_for(half);
	uint32_t worst = 0u, s;

	reset_all();
	for (uint32_t b = 0; b < 200u; b++) { block(false, 0u, 0u, rate); }
	s = fwd_spread(); if (s > worst) { worst = s; }
	for (uint32_t b = 0; b < 600u; b++) { block(true, LP_LO, LP_LO + 22050u, rate); }
	s = fwd_spread(); if (s > worst) { worst = s; }
	for (uint32_t b = 0; b < 600u; b++) { block(true, LP_LO, LP_LO + 11025u, rate); }
	s = fwd_spread(); if (s > worst) { worst = s; }
	for (uint32_t b = 0; b < 600u; b++) { block(true, LP_LO, LP_LO + 33075u, rate); }
	s = fwd_spread(); if (s > worst) { worst = s; }
	if (jump_pend == JUMP_WRAP) { jump_pend = 0u; }   /* release cancels a pending duck */
	for (uint32_t b = 0; b < 400u; b++) { block(false, 0u, 0u, rate); }
	*spread_after_release = fwd_spread();
	if (*spread_after_release > worst) { worst = *spread_after_release; }
	return worst;
}

static const int8_t k_halves[5] = { 0, 2, 3, 4, 5 };
static const char *const k_names[5] = { "unity", "+1.0 st", "+1.5 st", "+2.0 st", "+2.5 st" };

/* ===================================================================== */

static void test_fixed_guard_holds_every_rate(void)
{
	CASE("PRODUCTION GUARD: forward spread is exactly zero at every "
	     "checkpoint, every rate, with no cumulative drift");
	g_guard_explicit = 1;
	for (uint32_t i = 0; i < 5u; i++) {
		uint32_t after = 0u;
		const uint32_t worst = lifecycle(k_halves[i], &after);
		const double hpd = n_duck ? (double)n_duck_seeked / (double)n_duck : 0.0;

		printf("  %-8s worst_spread=%u after_release=%u heads/duck=%.2f "
		       "locked=%.1f%% run0=%u silent=%u\n",
		       k_names[i], worst, after, hpd, locked_pct(), n_run0, out_silent);
		CHECK(worst == 0u,
		      "%s: forward spread reached %u -- the four ordinary stems "
		      "must share ONE position through engage, both resizes and "
		      "release", k_names[i], worst);
		CHECK(after == 0u,
		      "%s: forward spread %u persists 400 blocks after release",
		      k_names[i], after);
		CHECK(hpd > 3.99,
		      "%s: the authoritative wrap moved %.2f heads per duck, not "
		      "4 -- an ordinary forward stem was left behind", k_names[i], hpd);
		CHECK(locked_pct() > 99.0,
		      "%s: shared-lane eligibility only %.1f%% -- pitched loop "
		      "playback fell into the four-lane path, which is the "
		      "crackle", k_names[i], locked_pct());
		CHECK(n_run0 == 0u, "%s: %u zero-length runs", k_names[i], n_run0);
		CHECK(out_silent == 0u, "%s: %u silent output frames",
		      k_names[i], out_silent);
	}
}

static void test_old_guard_reproduces_the_hardware_failure(void)
{
	CASE("PRE-FIX GUARD: the positional rule reproduces the reported "
	     "failure -- so this gate proves the patch matters");
	g_guard_explicit = 0;
	for (uint32_t i = 0; i < 5u; i++) {
		uint32_t after = 0u;
		const uint32_t worst = lifecycle(k_halves[i], &after);
		const double hpd = n_duck ? (double)n_duck_seeked / (double)n_duck : 0.0;

		printf("  %-8s worst_spread=%u after_release=%u heads/duck=%.2f "
		       "locked=%.1f%%\n",
		       k_names[i], worst, after, hpd, locked_pct());
		if (k_halves[i] == 0) {
			CHECK(worst == 0u,
			      "unity must be clean even with the old guard -- that "
			      "is why this was never seen at 1x");
		} else {
			CHECK(worst > 0u,
			      "%s: the old positional guard did NOT diverge. The "
			      "gate would then pass for free after the fix",
			      k_names[i]);
			CHECK(after > 0u,
			      "%s: the old guard's divergence must PERSIST after "
			      "release -- that is the reported symptom", k_names[i]);
			CHECK(hpd < 2.0,
			      "%s: the old guard moved %.2f heads per duck; the "
			      "defect is that it collapses toward 1", k_names[i], hpd);
			CHECK(locked_pct() < 25.0,
			      "%s: the old guard left shared-lane eligibility at "
			      "%.1f%%; the defect is that it collapses",
			      k_names[i], locked_pct());
		}
	}
	g_guard_explicit = 1;
}

static void test_starved_stem_reconverges(void)
{
	const uint32_t rate = rate_for(4);   /* +2.0 st */
	uint32_t old_after = 0u, new_after = 0u, before_starve = 0u;

	CASE("THE DISAPPEARING STEM: one stem starved 40 blocks at +2.0 comes "
	     "back into line, without any separate recovery mechanism");
	for (int mode = 0; mode < 2; mode++) {
		g_guard_explicit = mode;
		reset_all();
		g_starve_stem = 1;
		for (uint32_t b = 0; b < 300u; b++) { g_starving = 0u; block(true, LP_LO, LP_LO + 22050u, rate); }
		/* Only the explicit guard is expected to be synchronised here.
		 * The positional one has already diverged from the wraps alone
		 * -- which is case 2's finding, not this case's. */
		if (mode) {
			CHECK(fwd_spread() == 0u,
			      "explicit guard: not synchronised before starvation "
			      "(spread %u)", fwd_spread());
		} else {
			before_starve = fwd_spread();
		}
		for (uint32_t b = 0; b < 40u;  b++) { g_starving = 1u; block(true, LP_LO, LP_LO + 22050u, rate); }
		for (uint32_t b = 0; b < 900u; b++) { g_starving = 0u; block(true, LP_LO, LP_LO + 22050u, rate); }
		if (mode) { new_after = fwd_spread(); } else { old_after = fwd_spread(); }
		printf("  %-10s guard: spread 900 blocks after recovery = %u\n",
		       mode ? "explicit" : "positional", fwd_spread());
	}
	CHECK(old_after > 1000u && old_after > before_starve * 4u,
	      "the positional guard left the recovered stem %u frames out "
	      "(it was %u before starvation); the reported symptom is a stem "
	      "that never comes back", old_after, before_starve);
	CHECK(new_after == 0u,
	      "the explicit guard left the recovered stem %u frames out -- the "
	      "authoritative wrap must re-converge it", new_after);
	g_guard_explicit = 1;
}

static void test_reverse_inside_pitched_loop(void)
{
	const uint32_t rate = rate_for(5);   /* +2.5 st */
	const uint32_t lp_hi = LP_LO + 22050u;
	uint32_t master;

	CASE("REVERSE inside a +2.5 pitched loop: the reversed stem stays "
	     "independent, the other three stay together, and the st63 rejoin "
	     "still works");
	g_guard_explicit = 1;
	reset_all();
	for (uint32_t b = 0; b < 300u; b++) { block(true, LP_LO, lp_hi, rate); }
	CHECK(fwd_spread() == 0u, "all four forward: spread %u", fwd_spread());

	st_stream_set_reverse(&hd[2], true);
	pv[2] = false; frac[2] = 0u;
	for (uint32_t b = 0; b < 300u; b++) { block(true, LP_LO, lp_hi, rate); }
	CHECK(fwd_spread() == 0u,
	      "with stem 2 reversed the other three drifted apart by %u",
	      fwd_spread());
	CHECK(hd[2].song_frame != hd[transport].song_frame,
	      "the reversed stem was dragged onto the master -- the guard must "
	      "leave it independent");
	CHECK(n_reverse_seeked == 0u,
	      "the authoritative wrap seeked a REVERSED lane %u times. Per-track "
	      "reverse inside a loop is exactly what this guard exists to "
	      "protect, and a position check cannot see the violation because a "
	      "dragged reversed head resumes travelling backward at once",
	      n_reverse_seeked);
	printf("  reversed stem at %u (state=%d), master at %u, fwd spread=%u\n",
	       hd[2].song_frame, (int)hd[2].state, hd[transport].song_frame, fwd_spread());

	/* st63/st64 release semantics: capture MASTER first, then rejoin. */
	master = hd[transport].song_frame;
	st_stream_set_reverse(&hd[2], false);
	if (hd[2].song_frame != master) { (void)st_stream_seek(&hd[2], master); }
	pv[2] = false; frac[2] = 0u;
	for (uint32_t b = 0; b < 600u; b++) { block(true, LP_LO, lp_hi, rate); }
	CHECK(fwd_spread() == 0u,
	      "after the reverse release and 600 further blocks of wrapping the "
	      "forward spread is %u", fwd_spread());

	for (uint32_t b = 0; b < 400u; b++) { block(false, 0u, 0u, rate); }
	CHECK(fwd_spread() == 0u,
	      "after the later loop release the forward spread is %u", fwd_spread());
	CHECK(n_reverse_seeked == 0u,
	      "%u reversed-lane seeks over the whole reverse case", n_reverse_seeked);
	printf("  after reverse release + loop release: spread=%u locked=%.1f%% "
	       "reversed_lane_seeks=%u\n",
	       fwd_spread(), locked_pct(), n_reverse_seeked);
}

/*
 * DEFENCE IN DEPTH, CONSTRUCTED. A head parked at either end of the song with
 * reverse ALREADY CLEARED is not reachable through today's control flow -- a
 * START_OF_SONG park implies a reversed head, and st63's release seeks the
 * leaver out of it. The two state conditions are therefore not currently
 * load-bearing in production, and this case says so honestly while still
 * pinning them: it constructs the state directly, so a mutation that drops
 * either condition is caught rather than surviving as an equivalent mutant.
 */
static void test_parked_heads_are_never_dragged(void)
{
	const uint32_t rate = rate_for(4);
	const uint32_t lp_hi = LP_LO + 22050u;

	CASE("a head parked at START_OF_SONG or END_OF_SONG is never moved by "
	     "the authoritative wrap (constructed; see the comment)");
	g_guard_explicit = 1;
	for (int which = 0; which < 2; which++) {
		const st_stream_state_t st = which ? ST_STREAM_END_OF_SONG
						   : ST_STREAM_START_OF_SONG;
		uint32_t parked_at;

		reset_all();
		for (uint32_t b = 0; b < 300u; b++) { block(true, LP_LO, lp_hi, rate); }
		hd[3].state = st;
		hd[3].reverse = false;
		parked_at = hd[3].song_frame;
		for (uint32_t b = 0; b < 600u; b++) { block(true, LP_LO, lp_hi, rate); }
		CHECK(hd[3].song_frame == parked_at,
		      "a head parked in state %d was MOVED from %u to %u by the "
		      "authoritative wrap; a parked head consumes no source and "
		      "must not be seeked (st64's rule)",
		      (int)st, parked_at, hd[3].song_frame);
	}
}

int main(void)
{
	printf("Stem Tape LOOP-AT-PITCH gate\n");
	printf("one authoritative wrap, all ordinary forward stems, every rate\n\n");

	test_fixed_guard_holds_every_rate();
	test_old_guard_reproduces_the_hardware_failure();
	test_starved_stem_reconverges();
	test_reverse_inside_pitched_loop();
	test_parked_heads_are_never_dragged();

	printf("\n%d cases, %d checks, %d failures\n", g_cases, g_checks, g_failures);
	if (g_failures == 0) { printf("LOOP PITCH GATE PASSED\n"); }
	return g_failures ? 1 : 0;
}
