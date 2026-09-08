/*
 * st_rs_cursor.h -- the four per-stem resampler cursors, lifted out of
 * stem_render_run() so they can be TESTED AGAINST THE PRODUCTION CODE.
 *
 * ======================================================================
 * WHY THIS FILE EXISTS, AND WHY IT IS A HEADER
 * ======================================================================
 * Every gate in this repository that covers the audio path carries the same
 * caveat in its own report: "the gate's transport is a MODEL -- main.c cannot
 * be linked on the host -- so it can agree with a main.c that has drifted."
 * That is tolerable for a behavioural change, which is judged by what it does.
 * It is fatal for a change whose entire safety claim is THE OUTPUT IS
 * UNCHANGED, because a model cannot prove bit-identity of code it is not.
 *
 * So the cursor bookkeeping moves here, where a host test links the REAL
 * implementation rather than a copy of it. It is a header of `static inline`
 * functions and not a .c file for one reason: stem_render_run() carries
 * __attribute__((optimize("O2"), noinline, noclone)) and runs 48,000 times a
 * second on the deadline thread. A cross-translation-unit call per output
 * frame is precisely the cost st45 and st57 removed from this loop -- see
 * st_pl_decode_frame_shared()'s own comment. always_inline is not decoration
 * here; if these ever stop being inlined the extraction has cost real time,
 * and CI asserts their absence from the linked symbol table for that reason.
 *
 * ======================================================================
 * WHAT THIS IS NOT
 * ======================================================================
 * NOTHING HERE IS NEW. Every line is the pre-extraction main.c text, moved,
 * with the file-static `s_rs_prev` / `s_rs_prev_valid` reached through
 * parameters instead of directly. No arithmetic, no ordering, no clamp, no
 * fractional handling, no decode dispatch and no writeback rule was altered.
 * The four cursors stay four: frac[], cur[] and idx[] are per-stem arrays and
 * every loop below still walks all ST_PL_STEMS of them independently. Sharing
 * them when the heads are together is a SEPARATE, LATER change; this file
 * deliberately does not contain it, so that the change can be diffed against a
 * proven-identical baseline.
 *
 * ======================================================================
 * THE CALL ORDER IS PART OF THE CONTRACT
 * ======================================================================
 * Per output frame, exactly:
 *
 *     st_rs_cursor_index()     -- clamp each cursor, form each read index
 *     st_rs_cursor_fetch()     -- decode the frame AT the cursors
 *     st_rs_cursor_prime()     -- first frame after a drop: prev := nxt
 *     <the caller's blend, which reads prev, nxt and frac[]>
 *     st_rs_cursor_advance()   -- walk each cursor by one output frame
 *
 * and once per run, after the loop:
 *
 *     st_rs_cursor_finish()    -- publish frac[] and the source counts
 *
 * The blend sits BETWEEN prime and advance and is not extracted: it is mix
 * work, not cursor work, and every stem's samples genuinely differ, so there
 * is nothing shared about it in any future variant either.
 */
#ifndef ST_RS_CURSOR_H
#define ST_RS_CURSOR_H

#include <stdbool.h>
#include <stdint.h>

#include "st_planar.h"
#include "st_resample.h"
#include "st_v11_format.h"

/* See the file comment: these are hot-loop helpers whose whole purpose is to
 * compile to exactly what the code they were lifted from compiled to. A
 * non-inlined call here would be a per-output-frame function call at 48 kHz on
 * the priority-0 thread. */
#if defined(__GNUC__)
#define ST_RS_CURSOR_INLINE static inline __attribute__((always_inline))
#else
#define ST_RS_CURSOR_INLINE static inline
#endif

/*
 * ======================================================================
 * LOCKED: WHEN THE FOUR CURSORS ARE PROVABLY ONE CURSOR
 * ======================================================================
 * Ordinary four-forward playback off unity runs four bit-for-bit identical
 * cursor walks and throws three of them away. This predicate says when that is
 * demonstrably what is happening, so the bookkeeping can be done once on lane 0
 * and broadcast -- with the per-stem AUDIO work (the blend, `prev`, the
 * decodes) left entirely alone, because those genuinely differ per stem.
 *
 * IT IS DELIBERATELY STRONGER THAN `together`. main.c's `together` checks only
 * that no lane is reversed and that all four frame_in_group agree. Three more
 * things can differ underneath that, and each of them makes a shared cursor
 * silently wrong:
 *
 *   frac[]        st63's reverse release clears ONLY the rejoining lane's
 *                 s_stem_rate_frac[j] and seeks it to MASTER. One block later
 *                 all four are co-located and forward -- `together` is TRUE --
 *                 while that lane carries fraction 0 and three carry a
 *                 fraction. A shared fraction renders it at the other three's
 *                 sub-sample phase: audible only off centre pitch, only just
 *                 after a reverse release. The hardest kind of defect to
 *                 attribute, which is why it is a precondition and not a
 *                 comment.
 *   src_avail[]   a starved lane borrows the transport's offset and a forward
 *                 direction (main.c's silent-group path), so `together` stays
 *                 true while its run bound differs -- and the bound is what the
 *                 floored corner of st_rs_out_frames() clamps against.
 *   prev_valid[]  cleared per lane at the same sites as frac[]. Today they are
 *                 always cleared together; requiring it explicitly costs three
 *                 compares per RUN and removes a coupling assumption that a
 *                 future change could break without noticing.
 *
 * Under all five conditions every cursor-domain quantity -- c, idx, frac, cur,
 * the walk's step count, its pc/pidx and its branch -- is identical across the
 * four lanes at every iteration, by induction on the loop. So the shared form
 * computes the same values, not merely equivalent ones, and the output is
 * bit-identical rather than "indistinguishable".
 *
 * ANY condition failing means the existing independent per-stem path runs
 * unchanged. There is no partial sharing and no separate divergence detector to
 * keep in sync: the predicate IS the fallback.
 */
ST_RS_CURSOR_INLINE bool st_rs_cursor_locked(const uint32_t frame_in_group[ST_PL_STEMS],
					      const int8_t dirs[ST_PL_STEMS],
					      const uint32_t src_avail[ST_PL_STEMS],
					      const uint32_t frac[ST_PL_STEMS],
					      const bool prev_valid[ST_PL_STEMS])
{
	uint32_t sp;

	/* Lane 0 must itself be forward: every other lane is compared against
	 * it, so "all equal to a reversed lane 0" must not pass. */
	if (dirs[0] <= 0) {
		return false;
	}
	for (sp = 1; sp < ST_PL_STEMS; sp++) {
		if (dirs[sp] <= 0 ||
		    frame_in_group[sp] != frame_in_group[0] ||
		    src_avail[sp]      != src_avail[0]      ||
		    frac[sp]           != frac[0]           ||
		    prev_valid[sp]     != prev_valid[0]) {
			return false;
		}
	}
	return true;
}

/*
 * THE HARD BOUND. st_rs_out_frames() floors at one output frame, and above 1x
 * that single forced frame can ask for a source frame the run does not
 * contain. Holding the last available frame is a degenerate corner measured in
 * single frames; reading past the group buffer is memory corruption in a
 * real-time thread.
 *
 * The read index is
 *
 *     idx = frame_in_group + dir * cur
 *
 * per stem, in that stem's OWN direction: dirs[] is +1 for a head reading
 * forward and -1 for one reading back, so a reversed head's "further along"
 * is a LOWER index and its "behind" is a higher one.
 *
 * LOCKED: one index, broadcast. Downstream -- the decode dispatch and the
 * walk's `pidx == idx[sp]` test -- keeps reading idx[] per lane and never
 * learns that it was computed once.
 */
ST_RS_CURSOR_INLINE void st_rs_cursor_index(const uint32_t frame_in_group[ST_PL_STEMS],
					     const int8_t dirs[ST_PL_STEMS],
					     const uint32_t src_avail[ST_PL_STEMS],
					     const uint32_t cur[ST_PL_STEMS],
					     uint32_t idx[ST_PL_STEMS],
					     bool locked)
{
	uint32_t sp;

	if (locked) {
		const uint32_t c = (cur[0] >= src_avail[0])
				   ? (src_avail[0] - 1u) : cur[0];
		const uint32_t i0 = (uint32_t)((int32_t)frame_in_group[0] +
						dirs[0] * (int32_t)c);

		for (sp = 0; sp < ST_PL_STEMS; sp++) {
			idx[sp] = i0;
		}
		return;
	}
	for (sp = 0; sp < ST_PL_STEMS; sp++) {
		const uint32_t c = (cur[sp] >= src_avail[sp])
				   ? (src_avail[sp] - 1u) : cur[sp];

		idx[sp] = (uint32_t)((int32_t)frame_in_group[sp] +
				      dirs[sp] * (int32_t)c);
	}
}

/*
 * THE ARRAY FORM, one index per stem -- but only when the four indices
 * actually differ, which today they never do.
 *
 * st_pl_decode_frame() lives in st_planar.c, so this was a call across a
 * translation unit once per output frame at 48 kHz, on the deadline thread,
 * for the ENTIRE variable-rate path. Unity playback stopped paying it in st45
 * and this path kept paying it, which is exactly what the hardware reported:
 * ordinary playback clean, the pitch rocker off centre crackling again.
 *
 * One rate and one direction means all four cursors are the same number, so
 * the equal case is 100% of today's traffic and takes the inline decode. The
 * array call REMAINS, reached the moment a stem's cursor genuinely diverges --
 * which is per-track reverse, and is why the array form exists at all. Three
 * compares per frame buy back a call plus a four-element array construction,
 * and tests/test_planar.c already asserts the two forms agree when the indices
 * are equal.
 */
ST_RS_CURSOR_INLINE void st_rs_cursor_fetch(const uint8_t *const grp[ST_PL_STEMS],
					     const uint32_t idx[ST_PL_STEMS],
					     st11_audio_frame_t *nxt,
					     bool locked)
{
	/* LOCKED: the three compares are statically true. Skipping them is a
	 * saving, not a different path -- the branch taken is the same one, and
	 * the decode is byte-for-byte the same call. */
	if (locked) {
		st_pl_decode_frame_shared(grp, idx[0], nxt);
		return;
	}
	if (idx[0] == idx[1] && idx[1] == idx[2] &&
	    idx[2] == idx[3]) {
		st_pl_decode_frame_shared(grp, idx[0], nxt);
	} else {
		st_pl_decode_frame(grp, idx, nxt);
	}
}

/*
 * THE FIRST OUTPUT FRAME AFTER A DROP has no frame behind it, so the blend
 * would interpolate from whatever the last position left in prev -- audio from
 * a place the playhead no longer occupies. Priming prev from nxt makes that
 * first frame a copy of the source frame instead, which is the same thing the
 * blend would produce at frac == 0.
 *
 * PER STEM, because the validity flags are per stem: st63 clears exactly one
 * lane's flag when that stem leaves reverse and rejoins the master, while the
 * other three keep interpolating across a join they never left.
 */
ST_RS_CURSOR_INLINE void st_rs_cursor_prime(const st11_audio_frame_t *nxt,
					     st11_audio_frame_t *prev,
					     bool prev_valid[ST_PL_STEMS],
					     bool locked)
{
	uint32_t sp;

	/* LOCKED: one flag test instead of four, because the predicate has
	 * already established that all four agree. The COPY is still per stem
	 * -- it is audio, and every stem's samples differ. */
	if (locked) {
		if (!prev_valid[0]) {
			for (sp = 0; sp < ST_PL_STEMS; sp++) {
				prev->stem_l[sp] = nxt->stem_l[sp];
				prev->stem_r[sp] = nxt->stem_r[sp];
				prev_valid[sp] = true;
			}
		}
		return;
	}
	for (sp = 0; sp < ST_PL_STEMS; sp++) {
		if (!prev_valid[sp]) {
			prev->stem_l[sp] = nxt->stem_l[sp];
			prev->stem_r[sp] = nxt->stem_r[sp];
			prev_valid[sp] = true;
		}
	}
}

/*
 * Advance the cursor by one output frame's worth of source. ABOVE 1x this can
 * cross more than one frame, so it WALKS them rather than jumping: `prev` must
 * end up holding the frame immediately behind the new cursor, or the next
 * output frame would blend across a gap it never looked at. Below 1x the loop
 * runs at most once and this is what it always was.
 */
ST_RS_CURSOR_INLINE void st_rs_cursor_advance(const uint8_t *const grp[ST_PL_STEMS],
					       const uint32_t frame_in_group[ST_PL_STEMS],
					       const int8_t dirs[ST_PL_STEMS],
					       const uint32_t src_avail[ST_PL_STEMS],
					       const uint32_t idx[ST_PL_STEMS],
					       const st11_audio_frame_t *nxt,
					       uint32_t rate_q16,
					       uint32_t frac[ST_PL_STEMS],
					       uint32_t cur[ST_PL_STEMS],
					       st11_audio_frame_t *prev,
					       bool locked)
{
	uint32_t sp;

	/*
	 * LOCKED: ONE WALK, THEN BROADCAST.
	 *
	 * The scalar half -- the fraction accumulate, the step count, the
	 * cursor, pc, pidx and the `pidx == idx` test -- is done once on lane 0.
	 * The AUDIO half stays per stem inside it: `prev` is four different
	 * pairs of samples and st_pl_decode_stem_inline() reads four different
	 * groups, so those loops run for all four lanes exactly as before.
	 *
	 * THE BROADCAST IS THE LAST THING THIS FUNCTION DOES, and that
	 * placement is load-bearing rather than tidy. Everything downstream in
	 * the caller's frame loop -- st_fx_process()'s clock offset
	 * `cur[s_fx_target]`, the per-stem meter's `g_stem_zero_at[sp]`
	 * (song_frame + cur[sp]), and the global rack's `cur[fx_clock_stem]` --
	 * reads cur[] AFTER this call, per lane, and must never see a lane that
	 * still holds the previous output frame's cursor. Publishing all four
	 * before returning is what makes every one of those readers correct
	 * without any of them knowing the cursor was shared.
	 */
	if (locked) {
		uint32_t f0 = frac[0] + rate_q16;
		uint32_t c0 = cur[0];

		while (f0 >= ST_RS_ONE) {
			f0 -= ST_RS_ONE;
			c0++;
			if (c0 >= src_avail[0]) {
				/* OUT OF RUN -- see the unlocked path below for
				 * why the whole frames are dropped and the
				 * sub-frame phase is kept. */
				c0 = src_avail[0];
				for (sp = 0; sp < ST_PL_STEMS; sp++) {
					prev->stem_l[sp] = nxt->stem_l[sp];
					prev->stem_r[sp] = nxt->stem_r[sp];
				}
				f0 &= (ST_RS_ONE - 1u);
				break;
			}
			{
				uint32_t pc = c0 - 1u;
				uint32_t pidx;

				if (pc >= src_avail[0]) {
					pc = src_avail[0] - 1u;
				}
				pidx = (uint32_t)((int32_t)frame_in_group[0] +
						   dirs[0] * (int32_t)pc);
				if (pidx == idx[0]) {
					for (sp = 0; sp < ST_PL_STEMS; sp++) {
						prev->stem_l[sp] = nxt->stem_l[sp];
						prev->stem_r[sp] = nxt->stem_r[sp];
					}
				} else {
					for (sp = 0; sp < ST_PL_STEMS; sp++) {
						st_pl_decode_stem_inline(
							grp[sp], pidx,
							&prev->stem_l[sp],
							&prev->stem_r[sp]);
					}
				}
			}
		}
		/* PUBLISH BEFORE RETURNING. See above. */
		for (sp = 0; sp < ST_PL_STEMS; sp++) {
			frac[sp] = f0;
			cur[sp]  = c0;
		}
		return;
	}

	for (sp = 0; sp < ST_PL_STEMS; sp++) {
		frac[sp] += rate_q16;
		while (frac[sp] >= ST_RS_ONE) {
			frac[sp] -= ST_RS_ONE;
			cur[sp]++;
			if (cur[sp] >= src_avail[sp]) {
				/*
				 * OUT OF RUN. Reachable only from the floored
				 * corner in st_rs_out_frames() -- one forced
				 * output frame that at a rate above 1x wants
				 * more source than the run holds.
				 *
				 * The whole frames the rate asked for beyond
				 * the run are not there, so they are dropped;
				 * the SUB-FRAME phase is kept, because throwing
				 * it away would be a position step and leaving
				 * frac above 1.0 would make the next blend
				 * extrapolate past both its samples. ST_RS_ONE
				 * is a power of two, so the mask is exactly
				 * "the fractional part".
				 */
				cur[sp] = src_avail[sp];
				prev->stem_l[sp] = nxt->stem_l[sp];
				prev->stem_r[sp] = nxt->stem_r[sp];
				frac[sp] &= (ST_RS_ONE - 1u);
				break;
			}
			{
				/* ONE STEM'S frame behind its OWN new cursor.
				 * Decoding all four here would be three stems'
				 * work thrown away and, once directions differ,
				 * three stems read at a position that is not
				 * theirs. */
				uint32_t pc = cur[sp] - 1u;
				uint32_t pidx;

				if (pc >= src_avail[sp]) {
					pc = src_avail[sp] - 1u;
				}
				/* BEHIND IN THE DIRECTION OF TRAVEL: one step
				 * back along this head's own path, which for a
				 * reversed head is a HIGHER index in the
				 * group. */
				pidx = (uint32_t)((int32_t)frame_in_group[sp] +
						   dirs[sp] * (int32_t)pc);
				/*
				 * THE FRAME BEHIND THE NEW CURSOR IS USUALLY
				 * THE ONE ALREADY IN HAND.
				 *
				 * On the first step of this walk the cursor
				 * moves from c to c+1, so the frame behind it
				 * is c -- which is exactly where `nxt` was just
				 * decoded. Re-reading it from the group was
				 * four decodes per output frame thrown away,
				 * and at any rate at or above 1x that is EVERY
				 * frame: it roughly doubled the cost of the
				 * variable-rate render against the unity one,
				 * which is why the pitch rocker pushed the
				 * audio block past its 5.333 ms deadline while
				 * unity playback sat comfortably inside it.
				 *
				 * The compare is against the index `nxt` was
				 * actually decoded at, including the clamp, so
				 * it is correct rather than merely usually
				 * correct -- and a second or later step of the
				 * walk (rates above 2x) still decodes for real.
				 * Identical bytes either way: it is the same
				 * frame of the same group.
				 */
				if (pidx == idx[sp]) {
					prev->stem_l[sp] = nxt->stem_l[sp];
					prev->stem_r[sp] = nxt->stem_r[sp];
				} else {
					st_pl_decode_stem_inline(
						grp[sp], pidx,
						&prev->stem_l[sp],
						&prev->stem_r[sp]);
				}
			}
		}
	}
}

/*
 * The SOURCE frames this run actually consumed, PER STEM, which is what each
 * stream must be advanced by, together with the cursor fraction carried into
 * the next run. Reported rather than recomputed by the caller: two derivations
 * of the same count is exactly how a playhead and the audio it reads drift
 * apart.
 */
ST_RS_CURSOR_INLINE void st_rs_cursor_finish(const uint32_t frac[ST_PL_STEMS],
					      const uint32_t cur[ST_PL_STEMS],
					      const uint32_t src_avail[ST_PL_STEMS],
					      uint32_t frac_io[ST_PL_STEMS],
					      uint32_t used_out[ST_PL_STEMS],
					      bool locked)
{
	uint32_t sp;

	/* LOCKED: one bound test instead of four. cur[] and frac[] were already
	 * broadcast by the walk, so this is the same four values either way. */
	if (locked) {
		const uint32_t u0 = (cur[0] > src_avail[0]) ? src_avail[0] : cur[0];

		for (sp = 0; sp < ST_PL_STEMS; sp++) {
			frac_io[sp]  = frac[0];
			used_out[sp] = u0;
		}
		return;
	}
	for (sp = 0; sp < ST_PL_STEMS; sp++) {
		frac_io[sp] = frac[sp];
		/* Never report more than the run held, whatever the arithmetic
		 * did. */
		used_out[sp] = (cur[sp] > src_avail[sp]) ? src_avail[sp] : cur[sp];
	}
}

#endif /* ST_RS_CURSOR_H */
