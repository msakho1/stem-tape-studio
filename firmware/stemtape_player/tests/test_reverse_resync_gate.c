/*
 * test_reverse_resync_gate.c -- reverse is a TEMPORARY departure from the
 * shared timeline, and a head that stops being reversed rejoins it.
 *
 * ======================================================================
 * THE INVARIANT, IN ONE LINE
 * ======================================================================
 *
 *     if a stem is no longer reversed, it must not remain displaced by its
 *     prior reverse excursion.
 *
 * Both ways out of reverse are the same event: an explicit release of that
 * stem, and the implicit release of the previously-reversed stem when reverse
 * is switched straight to another one. Before Stage 2B only the first was even
 * considered, and neither resynced -- main.c's own comment recorded the older
 * rule outright ("track 2 resumes forward from wherever it is ... and not
 * re-synced").
 *
 * ======================================================================
 * WHAT WAS ACTUALLY BROKEN, AND WHY ORDER IS THE WHOLE FIX
 * ======================================================================
 * The song's master position is g_stem_stream[s_stem_transport].song_frame --
 * the same single value the loop window, the seam duck, the beat phase and
 * g_stem_song_frame_pub are all read from. There is deliberately no second
 * clock.
 *
 * s_stem_transport is reassigned on every reverse request by a search for the
 * LOWEST-INDEX FORWARD head, and that search has no idea which head was just
 * displaced. So reading the master AFTER it, whenever the head leaving reverse
 * has a lower index than the current transport -- reversing stem 0, or
 * switching away from it -- makes the master BE the displaced head. A rejoin
 * would then seek the stem to where it already is, and (this part was a live
 * defect in its own right) the published song clock, the beat phase and the
 * frame a PLAY-down captures for loop_start all jump backwards by the
 * excursion.
 *
 * Capturing the master BEFORE anything moves is the entire fix. Cases 7 and 8
 * below are the ones that fail if the capture is moved.
 *
 * ======================================================================
 * WHAT IS MODELLED, AND HOW HONEST THE MODEL IS
 * ======================================================================
 * st_stem_stream.c is the REAL module, compiled in and driven directly: every
 * head is a real st_stream_t, every advance and every seek is the production
 * call, and st_stream_set_reverse()'s real terminal-state handling is what runs.
 *
 * The transport around it -- the reverse-request consume block in three passes,
 * the per-head source bounds, the four-head advance, the loop-window backstop
 * and the transport search -- is a MODEL of stem_audio_block()'s wiring, in the
 * same shape and the same order. It is a model because main.c cannot be linked
 * on the host, so it CAN agree with a main.c that has drifted from it. That is
 * why the wiring check reads the production file for the ordering separately.
 *
 * Everything here runs at 1x. This gate is about WHICH FRAME a head is on after
 * a direction change, not about resampling.
 *
 * Build (from the repo root):
 *   cc -std=c11 -Wall -Wextra -Werror -Ifirmware/stemtape_player/src \
 *      firmware/stemtape_player/src/st_stem_stream.c \
 *      firmware/stemtape_player/tests/test_reverse_resync_gate.c \
 *      -o test_reverse_resync_gate && ./test_reverse_resync_gate
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "st_stem_stream.h"
#include "st_planar.h"

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

static void case_begin(const char *name)
{
	g_cases++;
	printf("case %d: %s\n", g_cases, name);
}

/* ======================================================================
 * THE MODEL
 * ====================================================================== */

#define BLK_FRAMES  256u
#define SECTORS     40u
#define SONG_FRAMES (SECTORS * ST11_FRAMES_PER_SECTOR - 37u)

typedef struct {
	st_stream_t head[ST_PL_STEMS];
	uint8_t     transport;                    /* s_stem_transport */

	bool        rs_prev_valid[ST_PL_STEMS];   /* s_rs_prev_valid[]  */
	uint32_t    rate_frac[ST_PL_STEMS];       /* s_stem_rate_frac[] */

	bool        lp_on;                        /* the user loop window */
	uint32_t    lp_lo, lp_end;

	uint32_t    wraps;                        /* g_stem_loop_wraps */
	bool        starve[ST_PL_STEMS];          /* test hook */
} model_t;

static void m_init(model_t *m)
{
	uint32_t k;

	memset(m, 0, sizeof(*m));
	for (k = 0; k < ST_PL_STEMS; k++) {
		if (!st_stream_init(&m->head[k], 0u,
				     SECTORS * ST11_BLOCKS_PER_SECTOR,
				     SONG_FRAMES, SECTORS,
				     /*loop_enabled=*/false)) {
			printf("  FATAL: st_stream_init failed\n");
			g_failures++;
		}
		st_stream_play(&m->head[k]);
	}
	m->transport = 0u;
}

/*
 * THE CARRIED RESAMPLER STATE IS MARKED, not merely observed.
 *
 * In production these are written by the renderer every block. The gate cannot
 * run the renderer, so before every direction change it stamps all four with a
 * distinguishable non-default value. A drop is then VISIBLE: the head whose
 * state went back to (false, 0) is the head whose state was dropped, and the
 * three that kept their stamp are the three that were left alone. Without this,
 * "only one was dropped" would be unfalsifiable -- all four already look
 * dropped at rest.
 */
static void m_mark_rs(model_t *m)
{
	uint32_t k;

	for (k = 0; k < ST_PL_STEMS; k++) {
		m->rs_prev_valid[k] = true;
		m->rate_frac[k]     = 1000u + k;
	}
}

static bool m_rs_dropped(const model_t *m, uint32_t k)
{
	return !m->rs_prev_valid[k] && m->rate_frac[k] == 0u;
}

static bool m_rs_intact(const model_t *m, uint32_t k)
{
	return m->rs_prev_valid[k] && m->rate_frac[k] == (1000u + k);
}

/*
 * main.c's reverse-request consume block, in its three passes and its order.
 * `req_track` is the track the player named (the gesture publishes a track, not
 * a direction -- the audio thread decides what it means).
 */
static void m_reverse_toggle(model_t *m, uint32_t k)
{
	uint32_t j;
	bool turning_on;
	uint32_t master;

	if (k >= ST_PL_STEMS) {
		return;
	}
	turning_on = !m->head[k].reverse;

	/* ---- MASTER, CAPTURED BEFORE ANYTHING MOVES ---- */
	master = m->head[m->transport].song_frame;

	/* ---- PASS 1: every head LEAVING reverse rejoins master ---- */
	for (j = 0; j < ST_PL_STEMS; j++) {
		const bool want = turning_on && (j == k);

		if (want || !m->head[j].reverse) {
			continue;
		}
		st_stream_set_reverse(&m->head[j], false);
		if (m->head[j].song_frame != master) {
			(void)st_stream_seek(&m->head[j], master);
		}
		m->rs_prev_valid[j] = false;
		m->rate_frac[j]     = 0u;
	}

	/* ---- PASS 2: the head ENTERING reverse ---- */
	if (turning_on && !m->head[k].reverse) {
		st_stream_set_reverse(&m->head[k], true);
		m->rs_prev_valid[k] = false;
		m->rate_frac[k]     = 0u;
	}

	/* ---- PASS 3: the transport search, LAST ---- */
	for (j = 0; j < ST_PL_STEMS; j++) {
		if (!m->head[j].reverse) {
			m->transport = (uint8_t)j;
			break;
		}
	}
}

static void m_serve(model_t *m)
{
	uint32_t k;

	for (k = 0; k < ST_PL_STEMS; k++) {
		if (m->starve[k] ||
		    m->head[k].state == ST_STREAM_END_OF_SONG ||
		    m->head[k].state == ST_STREAM_START_OF_SONG) {
			continue;
		}
		st_stream_sector_ready(&m->head[k],
					st_stream_required_sector(&m->head[k]));
	}
}

/* One audio block at 1x: per-head source bounds, the minimum, four advances in
 * their own directions, then the loop-window backstop. */
static void m_audio_block(model_t *m)
{
	uint32_t left = BLK_FRAMES;

	while (left > 0u) {
		uint32_t run = left;
		uint32_t k;

		m_serve(m);

		for (k = 0; k < ST_PL_STEMS; k++) {
			const st_stream_t *h = &m->head[k];
			uint32_t fis = h->song_frame % ST11_FRAMES_PER_SECTOR;
			uint32_t rk;

			if (h->ready_sector != st_stream_required_sector(h)) {
				continue;   /* silent this run; bounds nothing */
			}
			if (h->reverse) {
				rk = fis + 1u;
				if (rk > h->song_frame + 1u) {
					rk = h->song_frame + 1u;
				}
				if (m->lp_on && h->song_frame >= m->lp_lo &&
				    h->song_frame < m->lp_end &&
				    rk > h->song_frame - m->lp_lo + 1u) {
					rk = h->song_frame - m->lp_lo + 1u;
				}
			} else {
				rk = ST11_FRAMES_PER_SECTOR - fis;
				if (rk > h->frames - h->song_frame) {
					rk = h->frames - h->song_frame;
				}
				if (m->lp_on && h->song_frame >= m->lp_lo &&
				    h->song_frame < m->lp_end &&
				    rk > m->lp_end - h->song_frame) {
					rk = m->lp_end - h->song_frame;
				}
			}
			if (rk < run) {
				run = rk;
			}
		}
		if (run == 0u) {
			break;
		}

		for (k = 0; k < ST_PL_STEMS; k++) {
			(void)st_stream_advance_frames(&m->head[k], run);
		}
		left -= run;

		if (m->lp_on && m->lp_end > m->lp_lo) {
			for (k = 0; k < ST_PL_STEMS; k++) {
				st_stream_t *h = &m->head[k];
				bool wrapped = false;

				if (h->reverse) {
					if (h->song_frame < m->lp_lo && m->lp_end > 0u) {
						wrapped = st_stream_seek(h, m->lp_end - 1u);
					}
				} else if (h->song_frame >= m->lp_end) {
					wrapped = st_stream_seek(h, m->lp_lo);
				}
				if (wrapped) {
					m->rs_prev_valid[k] = false;
					m->rate_frac[k]     = 0u;
					if (k == m->transport) {
						m->wraps++;
					}
				}
			}
		}
	}
}

static void m_run(model_t *m, uint32_t blocks)
{
	while (blocks--) {
		m_audio_block(m);
	}
}

static uint32_t m_master(const model_t *m)
{
	return m->head[m->transport].song_frame;
}

/*
 * A FIXTURE GUARD, not a property of the code under test.
 *
 * `master` is only a seekable position while the transport head is inside the
 * song; st_stream_seek() refuses frame >= frames. In production that is assured
 * by the transport being stopped at the end of the song, but this gate models
 * no end-of-song handling, so a fixture that runs past the last frame silently
 * turns every rejoin into a no-op. It did: the first draft of the
 * song-start-park case ran 200 blocks over a 20,363-frame song and the failure
 * read as a resync bug rather than a fixture bug.
 */
static void m_check_master_in_song(const model_t *m, const char *where)
{
	CHECK(m_master(m) < m->head[m->transport].frames,
	      "%s: the fixture kept master inside the song (%u < %u)", where,
	      m_master(m), m->head[m->transport].frames);
}

/* ======================================================================
 * 1. EXPLICIT REVERSE RELEASE
 * ====================================================================== */

static void t_release_rejoins(uint32_t rev)
{
	model_t m;
	uint32_t k;
	uint32_t others[ST_PL_STEMS];
	uint32_t master_before, displaced;

	m_init(&m);
	m_run(&m, 30u);                     /* MASTER is well into the song */

	m_mark_rs(&m);
	m_reverse_toggle(&m, rev);          /* reverse ON */
	CHECK(m.head[rev].reverse, "stem %u is reversed", rev);
	CHECK(m.transport != rev,
	      "the song clock moved off the reversed head (transport=%u)",
	      m.transport);

	m_run(&m, 20u);                     /* it travels BACKWARD, alone */

	displaced = m.head[rev].song_frame;
	m_check_master_in_song(&m, __func__);
	master_before = m_master(&m);
	CHECK(displaced < master_before,
	      "stem %u is behind master (%u < %u)", rev, displaced,
	      master_before);
	for (k = 0; k < ST_PL_STEMS; k++) {
		if (k == rev) {
			continue;
		}
		CHECK(m.head[k].song_frame == master_before,
		      "stem %u kept up with master (%u vs %u)", k,
		      m.head[k].song_frame, master_before);
		others[k] = m.head[k].song_frame;
	}

	/* ---- THE RELEASE ---- */
	m_mark_rs(&m);
	m_reverse_toggle(&m, rev);

	CHECK(!m.head[rev].reverse, "stem %u is forward again", rev);
	CHECK(m.head[rev].song_frame == master_before,
	      "stem %u REJOINED master (%u, want %u) -- it did not continue "
	      "forward from %u", rev, m.head[rev].song_frame, master_before,
	      displaced);
	CHECK(m.head[rev].ready_sector == ST_STREAM_NO_SECTOR,
	      "stem %u dropped residency, so no stale audio from %u can play",
	      rev, displaced);
	CHECK(m.head[rev].state == ST_STREAM_PLAYING,
	      "stem %u is playing, state=%u", rev,
	      (unsigned)m.head[rev].state);

	for (k = 0; k < ST_PL_STEMS; k++) {
		if (k == rev) {
			continue;
		}
		CHECK(m.head[k].song_frame == others[k],
		      "stem %u was NOT repositioned by the release (%u, was %u)",
		      k, m.head[k].song_frame, others[k]);
		CHECK(m.head[k].ready_sector != ST_STREAM_NO_SECTOR,
		      "stem %u kept its residency -- the other three did not "
		      "move and must not re-prime", k);
	}

	/* ---- ONLY THE OUTGOING STEM'S CARRIED STATE ---- */
	CHECK(m_rs_dropped(&m, rev),
	      "stem %u's resampler state was dropped (prev_valid=%d frac=%u)",
	      rev, (int)m.rs_prev_valid[rev], m.rate_frac[rev]);
	for (k = 0; k < ST_PL_STEMS; k++) {
		if (k == rev) {
			continue;
		}
		CHECK(m_rs_intact(&m, k),
		      "stem %u's resampler state is UNTOUCHED (prev_valid=%d "
		      "frac=%u) -- stem_rs_drop() would have cleared it", k,
		      (int)m.rs_prev_valid[k], m.rate_frac[k]);
	}

	/* ---- AND ALL FOUR NOW MOVE AS ONE ---- */
	m_run(&m, 10u);
	for (k = 1; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame == m.head[0].song_frame,
		      "after the rejoin stem %u tracks stem 0 (%u vs %u)", k,
		      m.head[k].song_frame, m.head[0].song_frame);
	}
}

static void t_release_all_stems(void)
{
	uint32_t rev;

	for (rev = 0; rev < ST_PL_STEMS; rev++) {
		char name[96];

		snprintf(name, sizeof(name),
			 "explicit reverse release on stem %u rejoins master", rev);
		case_begin(name);
		t_release_rejoins(rev);
	}
}

/* ======================================================================
 * 2. SWITCHING REVERSE STRAIGHT TO ANOTHER STEM
 * ====================================================================== */

static void t_switch(uint32_t from, uint32_t to)
{
	model_t m;
	uint32_t k;
	uint32_t master_before, displaced;
	uint32_t others[ST_PL_STEMS];

	m_init(&m);
	m_run(&m, 30u);

	m_mark_rs(&m);
	m_reverse_toggle(&m, from);
	m_run(&m, 20u);

	displaced     = m.head[from].song_frame;
	m_check_master_in_song(&m, __func__);
	master_before = m_master(&m);
	CHECK(displaced < master_before,
	      "stem %u departed backwards (%u < %u)", from, displaced,
	      master_before);
	for (k = 0; k < ST_PL_STEMS; k++) {
		others[k] = m.head[k].song_frame;
	}

	/* ---- THE SWITCH: reverse engaged on `to`, never released on `from` -- */
	m_mark_rs(&m);
	m_reverse_toggle(&m, to);

	CHECK(!m.head[from].reverse, "the outgoing stem %u is forward again",
	      from);
	CHECK(m.head[to].reverse, "the incoming stem %u is reversed", to);
	CHECK(m.head[from].song_frame == master_before,
	      "the OUTGOING stem %u rejoined master (%u, want %u) -- it did "
	      "not stay at %u", from, m.head[from].song_frame, master_before,
	      displaced);
	CHECK(m.head[to].song_frame == others[to],
	      "the incoming stem %u was not moved by being reversed (%u, was "
	      "%u)", to, m.head[to].song_frame, others[to]);
	CHECK(m.head[to].ready_sector != ST_STREAM_NO_SECTOR,
	      "the incoming stem %u kept its residency -- turning around is "
	      "not a position change", to);
	CHECK(m.transport != to,
	      "the song clock is not the newly reversed head (transport=%u)",
	      m.transport);
	CHECK(!m.head[m.transport].reverse,
	      "the song clock is a FORWARD head");
	CHECK(m_master(&m) == master_before,
	      "the master did not move across the switch (%u, want %u)",
	      m_master(&m), master_before);

	/* the two untouched stems */
	for (k = 0; k < ST_PL_STEMS; k++) {
		if (k == from || k == to) {
			continue;
		}
		CHECK(m.head[k].song_frame == others[k],
		      "bystander stem %u did not move (%u, was %u)", k,
		      m.head[k].song_frame, others[k]);
		CHECK(m_rs_intact(&m, k),
		      "bystander stem %u's resampler state is untouched", k);
	}
	CHECK(m_rs_dropped(&m, from),
	      "the outgoing stem %u's resampler state was dropped", from);
	CHECK(m_rs_dropped(&m, to),
	      "the incoming stem %u's resampler state was dropped (it turned "
	      "around)", to);

	/* ---- the new head departs independently, the rest stay together ---- */
	m_run(&m, 10u);
	CHECK(m.head[to].song_frame < m_master(&m),
	      "the incoming stem %u is now travelling backwards alone (%u < %u)",
	      to, m.head[to].song_frame, m_master(&m));
	for (k = 0; k < ST_PL_STEMS; k++) {
		if (k == to) {
			continue;
		}
		CHECK(m.head[k].song_frame == m_master(&m),
		      "stem %u is on master after the switch (%u vs %u)", k,
		      m.head[k].song_frame, m_master(&m));
	}
}

static void t_switch_all(void)
{
	static const uint32_t pairs[][2] = {
		{ 0u, 2u },   /* away from stem 0 -- the master-capture case */
		{ 2u, 0u },
		{ 1u, 3u },
		{ 3u, 1u },
		{ 0u, 1u },
		{ 2u, 3u },
	};
	size_t i;

	for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
		char name[96];

		snprintf(name, sizeof(name),
			 "switching reverse from stem %u to stem %u",
			 (unsigned)pairs[i][0], (unsigned)pairs[i][1]);
		case_begin(name);
		t_switch(pairs[i][0], pairs[i][1]);
	}
}

/* ======================================================================
 * 3. WHILE A USER LOOP IS LATCHED
 * ====================================================================== */

static void loop_setup(model_t *m, uint32_t lo, uint32_t hi)
{
	uint32_t k;

	m_init(m);
	for (k = 0; k < ST_PL_STEMS; k++) {
		(void)st_stream_seek(&m->head[k], lo);
	}
	m->lp_on  = true;
	m->lp_lo  = lo;
	m->lp_end = hi;
	m_run(m, 4u);            /* a little way into the window */
}

static void t_release_in_loop(uint32_t rev)
{
	model_t m;
	uint32_t k;
	uint32_t master_before, displaced, wraps_before;
	const uint32_t lo = 10u * ST11_FRAMES_PER_SECTOR;
	const uint32_t hi = lo + 4000u;

	loop_setup(&m, lo, hi);

	m_mark_rs(&m);
	m_reverse_toggle(&m, rev);
	m_run(&m, 3u);

	displaced     = m.head[rev].song_frame;
	m_check_master_in_song(&m, __func__);
	master_before = m_master(&m);
	CHECK(master_before >= lo && master_before < hi,
	      "the loop master is inside the window (%u in [%u,%u))",
	      master_before, lo, hi);
	CHECK(displaced != master_before,
	      "stem %u departed from the loop master (%u vs %u)", rev,
	      displaced, master_before);
	wraps_before = m.wraps;

	/* ---- THE RELEASE, WITH THE WINDOW STILL LATCHED ---- */
	m_mark_rs(&m);
	m_reverse_toggle(&m, rev);

	CHECK(m.head[rev].song_frame == master_before,
	      "stem %u rejoined the CURRENT LOOP MASTER (%u, want %u)", rev,
	      m.head[rev].song_frame, master_before);
	CHECK(m.head[rev].song_frame >= lo && m.head[rev].song_frame < hi,
	      "and that position is inside the window [%u,%u), not some "
	      "song position outside it", lo, hi);
	CHECK(m.lp_on, "the loop is still latched");
	CHECK(m.wraps == wraps_before,
	      "the rejoin did not cause a wrap (%u, was %u)", m.wraps,
	      wraps_before);
	CHECK(m_rs_dropped(&m, rev), "only stem %u's carried state dropped",
	      rev);
	for (k = 0; k < ST_PL_STEMS; k++) {
		if (k != rev) {
			CHECK(m_rs_intact(&m, k),
			      "stem %u's carried state is untouched", k);
		}
	}

	/* ---- and the loop keeps looping, with all four together ---- */
	m_run(&m, 200u);
	CHECK(m.wraps > wraps_before, "the loop wrapped %u times after the "
	      "rejoin", m.wraps - wraps_before);
	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame >= lo && m.head[k].song_frame < hi,
		      "stem %u is still inside the window, at %u", k,
		      m.head[k].song_frame);
		CHECK(m.head[k].song_frame == m.head[0].song_frame,
		      "stem %u tracks stem 0 (%u vs %u)", k,
		      m.head[k].song_frame, m.head[0].song_frame);
	}
}

static void t_switch_in_loop(uint32_t from, uint32_t to)
{
	model_t m;
	uint32_t k;
	uint32_t master_before, displaced;
	const uint32_t lo = 6u * ST11_FRAMES_PER_SECTOR;
	const uint32_t hi = lo + 3000u;

	loop_setup(&m, lo, hi);

	m_mark_rs(&m);
	m_reverse_toggle(&m, from);
	m_run(&m, 3u);

	displaced     = m.head[from].song_frame;
	m_check_master_in_song(&m, __func__);
	master_before = m_master(&m);
	CHECK(displaced != master_before,
	      "stem %u departed inside the loop (%u vs %u)", from, displaced,
	      master_before);

	m_mark_rs(&m);
	m_reverse_toggle(&m, to);

	CHECK(m.head[from].song_frame == master_before,
	      "the OUTGOING stem %u rejoined the current loop master (%u, want "
	      "%u)", from, m.head[from].song_frame, master_before);
	CHECK(m.head[from].song_frame >= lo && m.head[from].song_frame < hi,
	      "inside the window [%u,%u)", lo, hi);
	CHECK(m.head[to].reverse,
	      "the incoming stem %u is now the ONLY independent head", to);
	for (k = 0; k < ST_PL_STEMS; k++) {
		if (k != to) {
			CHECK(!m.head[k].reverse,
			      "stem %u is forward", k);
			CHECK(m.head[k].song_frame == master_before,
			      "stem %u is on the loop master (%u vs %u)", k,
			      m.head[k].song_frame, master_before);
		}
	}
	CHECK(m.lp_on, "the loop is still latched");
}

/* ======================================================================
 * 4. THE MASTER CAPTURE, ISOLATED
 * ======================================================================
 * The one case that fails if the capture moves below the transport search.
 * Stem 0 is special because the search picks the lowest-index forward head:
 * while stem 0 is reversed the transport is stem 1, and the moment stem 0 comes
 * back it becomes the transport again -- so a master read after the search is
 * the displaced head's own position and the rejoin is a no-op.
 */

static void t_master_not_the_displaced_head(void)
{
	model_t m;
	uint32_t master_before, displaced;

	case_begin("the rejoin target is not the head being released (stem 0)");
	m_init(&m);
	m_run(&m, 30u);

	m_reverse_toggle(&m, 0u);
	CHECK(m.transport == 1u,
	      "while stem 0 is reversed the song clock is stem 1 (transport=%u)",
	      m.transport);
	m_run(&m, 20u);

	displaced     = m.head[0].song_frame;
	m_check_master_in_song(&m, __func__);
	master_before = m_master(&m);
	CHECK(master_before > displaced + BLK_FRAMES,
	      "stem 0 is a long way behind master (%u vs %u)", displaced,
	      master_before);

	m_reverse_toggle(&m, 0u);
	CHECK(m.transport == 0u,
	      "the search hands the clock back to stem 0 (transport=%u)",
	      m.transport);
	CHECK(m.head[0].song_frame == master_before,
	      "and stem 0 is ON master (%u, want %u) -- a master read AFTER "
	      "the search would have been %u, its own displaced position, and "
	      "the rejoin would have been a no-op", m.head[0].song_frame,
	      master_before, displaced);
	CHECK(m_master(&m) == master_before,
	      "so the published song clock did not jump backwards (%u, want "
	      "%u)", m_master(&m), master_before);
}

/* ======================================================================
 * 5. THE AWKWARD CORNERS
 * ====================================================================== */

static void t_reverse_to_song_start(void)
{
	model_t m;
	uint32_t k;
	uint32_t master_before;

	case_begin("a head that backed up to frame 0 and parked still rejoins");
	m_init(&m);
	m_run(&m, 4u);

	/* Ten blocks: four take stem 2 from ~1024 back to 0, the rest keep it
	 * parked there. NOT more -- overrunning the whole song would leave the
	 * forward heads at `frames`, and a `master` of `frames` is not a
	 * position any head can be seeked to. m_check_master_in_song() below
	 * makes that a loud failure rather than a silent one, because the first
	 * draft of this fixture ran 200 blocks and did exactly that. */
	m_reverse_toggle(&m, 2u);
	m_run(&m, 10u);

	CHECK(m.head[2].song_frame == 0u,
	      "stem 2 clamped at the song start, at %u", m.head[2].song_frame);
	CHECK(m.head[2].state == ST_STREAM_START_OF_SONG,
	      "and parked there, state=%u", (unsigned)m.head[2].state);

	m_check_master_in_song(&m, __func__);
	master_before = m_master(&m);
	m_mark_rs(&m);
	m_reverse_toggle(&m, 2u);

	CHECK(m.head[2].song_frame == master_before,
	      "stem 2 rejoined master from the park (%u, want %u)",
	      m.head[2].song_frame, master_before);
	CHECK(m.head[2].state == ST_STREAM_PLAYING,
	      "and START_OF_SONG was lifted, state=%u",
	      (unsigned)m.head[2].state);
	CHECK(m_rs_dropped(&m, 2u), "only stem 2's carried state dropped");
	for (k = 0; k < ST_PL_STEMS; k++) {
		if (k != 2u) {
			CHECK(m_rs_intact(&m, k),
			      "stem %u's carried state is untouched", k);
		}
	}

	m_run(&m, 10u);
	for (k = 1; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame == m.head[0].song_frame,
		      "stem %u tracks stem 0 after the rejoin", k);
	}
}

static void t_no_excursion_no_reprime(void)
{
	model_t m;
	uint32_t k;
	uint32_t sector_before;

	case_begin("a reverse that never moved does not pay a needless re-prime");
	m_init(&m);
	m_run(&m, 8u);

	m_reverse_toggle(&m, 1u);          /* ON, no blocks run */
	sector_before = m.head[1].ready_sector;
	CHECK(sector_before != ST_STREAM_NO_SECTOR,
	      "turning around did not invalidate residency");

	m_mark_rs(&m);
	m_reverse_toggle(&m, 1u);          /* straight back OFF */

	CHECK(m.head[1].song_frame == m_master(&m),
	      "stem 1 is on master (%u vs %u)", m.head[1].song_frame,
	      m_master(&m));
	CHECK(m.head[1].ready_sector == sector_before,
	      "and kept its residency -- no seek, so no re-prime and no "
	      "whole-block stall for an excursion that never happened");
	CHECK(m_rs_dropped(&m, 1u),
	      "the carried state IS still dropped: the direction changed twice "
	      "and the frame behind the cursor is not trustworthy");
	for (k = 0; k < ST_PL_STEMS; k++) {
		if (k != 1u) {
			CHECK(m_rs_intact(&m, k),
			      "stem %u's carried state is untouched", k);
		}
	}
}

static void t_starved_master_still_forward(void)
{
	model_t m;
	uint32_t master_before;

	case_begin("the rejoin target is a forward head even when a lane starves");
	m_init(&m);
	m_run(&m, 20u);

	m_reverse_toggle(&m, 3u);
	m.starve[1] = true;                /* a bystander lane loses its ring */
	m_run(&m, 10u);

	CHECK(!m.head[m.transport].reverse,
	      "the transport is a forward head (transport=%u)", m.transport);
	m_check_master_in_song(&m, __func__);
	master_before = m_master(&m);

	m_reverse_toggle(&m, 3u);
	CHECK(m.head[3].song_frame == master_before,
	      "stem 3 rejoined the master position (%u, want %u)",
	      m.head[3].song_frame, master_before);
	CHECK(!m.head[3].reverse, "and is forward again");
}

/* ======================================================================
 * 6. LOOP SEMANTICS ARE UNCHANGED
 * ======================================================================
 * The regression wall. Reverse resync must not have reached loop entry, loop
 * wrap or loop release.
 */

static void t_loop_untouched(void)
{
	model_t m;
	uint32_t k;
	uint32_t at_entry[ST_PL_STEMS];
	uint32_t at_release[ST_PL_STEMS];
	/* A window that CONTAINS where twelve blocks of playback leaves the
	 * playhead (12 * 256 = 3072), so the latch is the ordinary case. */
	const uint32_t lo = 4u * ST11_FRAMES_PER_SECTOR;
	const uint32_t hi = lo + 2500u;
	uint32_t wraps_before;

	case_begin("loop entry still changes the rules, never the position");
	m_init(&m);
	m_run(&m, 12u);
	for (k = 0; k < ST_PL_STEMS; k++) {
		at_entry[k] = m.head[k].song_frame;
	}
	CHECK(at_entry[0] > lo && at_entry[0] < hi,
	      "the playhead is inside the window to be latched, at %u",
	      at_entry[0]);

	m.lp_on  = true;
	m.lp_lo  = lo;
	m.lp_end = hi;
	m_audio_block(&m);
	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame > at_entry[k] &&
		      m.head[k].song_frame <= at_entry[k] + BLK_FRAMES,
		      "stem %u kept moving forward and was not moved by the "
		      "latch (%u -> %u)", k, at_entry[k], m.head[k].song_frame);
	}

	case_begin("loop wrap is still the only thing that moves a playhead");
	wraps_before = m.wraps;
	m_run(&m, 60u);
	CHECK(m.wraps > wraps_before, "it wrapped %u times",
	      m.wraps - wraps_before);
	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame >= lo && m.head[k].song_frame < hi,
		      "stem %u stayed inside the window, at %u", k,
		      m.head[k].song_frame);
	}

	case_begin("loop release still preserves the audible position");
	for (k = 0; k < ST_PL_STEMS; k++) {
		at_release[k] = m.head[k].song_frame;
	}
	wraps_before = m.wraps;
	m.lp_on = false;                    /* THE RELEASE, and nothing else */
	m_audio_block(&m);

	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame > at_release[k],
		      "stem %u continued FORWARD from where it was (%u -> %u)",
		      k, at_release[k], m.head[k].song_frame);
		CHECK(m.head[k].song_frame <= at_release[k] + BLK_FRAMES,
		      "stem %u did not jump (%u -> %u)", k, at_release[k],
		      m.head[k].song_frame);
		CHECK(m.head[k].song_frame != hi,
		      "stem %u did not seek to loop_end", k);
		CHECK(m.head[k].song_frame != lo,
		      "stem %u did not seek to loop_start", k);
	}
	m_run(&m, 40u);
	CHECK(m.wraps == wraps_before,
	      "and no further wrap happened (%u, was %u)", m.wraps,
	      wraps_before);
	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame >= hi,
		      "stem %u ran on past the old loop end (%u >= %u)", k,
		      m.head[k].song_frame, hi);
	}
}

/* Reverse released in the SAME pass the loop is released: the rejoin must land
 * on the current loop master, never on a hypothetical outside-the-loop
 * position, and the loop release itself must stay a no-op on position. */
static void t_release_both_together(void)
{
	model_t m;
	uint32_t k;
	uint32_t master_before, displaced;
	const uint32_t lo = 5u * ST11_FRAMES_PER_SECTOR;
	const uint32_t hi = lo + 2000u;

	case_begin("reverse release and loop release in the same pass");
	loop_setup(&m, lo, hi);
	m_reverse_toggle(&m, 1u);
	m_run(&m, 3u);

	displaced     = m.head[1].song_frame;
	m_check_master_in_song(&m, __func__);
	master_before = m_master(&m);
	CHECK(displaced != master_before, "stem 1 departed (%u vs %u)",
	      displaced, master_before);

	/* main.c's order: the reverse request is consumed at the top of the
	 * block; lp_on is not read until inside the run loop below it. So the
	 * rejoin happens first, against the window that was still latched. */
	m_mark_rs(&m);
	m_reverse_toggle(&m, 1u);
	m.lp_on = false;

	CHECK(m.head[1].song_frame == master_before,
	      "stem 1 rejoined the loop master (%u, want %u)",
	      m.head[1].song_frame, master_before);
	CHECK(m.head[1].song_frame >= lo && m.head[1].song_frame < hi,
	      "inside the window it was released from [%u,%u), not an "
	      "imaginary song position outside it", lo, hi);

	m_audio_block(&m);
	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame > master_before &&
		      m.head[k].song_frame <= master_before + BLK_FRAMES,
		      "stem %u then continued forward naturally from the "
		      "audible position (%u)", k, m.head[k].song_frame);
	}
}

int main(void)
{
	printf("== Stem Tape reverse-release resync gate ==\n\n");

	t_release_all_stems();
	t_switch_all();

	case_begin("reverse release while a loop is latched (stem 0)");
	t_release_in_loop(0u);
	case_begin("reverse release while a loop is latched (stem 2)");
	t_release_in_loop(2u);

	case_begin("switching reverse while a loop is latched (0 -> 3)");
	t_switch_in_loop(0u, 3u);
	case_begin("switching reverse while a loop is latched (2 -> 1)");
	t_switch_in_loop(2u, 1u);

	t_master_not_the_displaced_head();
	t_reverse_to_song_start();
	t_no_excursion_no_reprime();
	t_starved_master_still_forward();
	t_loop_untouched();
	t_release_both_together();

	printf("\n%d cases, %d checks, %d failures\n", g_cases, g_checks,
	       g_failures);
	if (g_failures == 0) {
		printf("REVERSE RESYNC GATE PASSED\n");
		return 0;
	}
	printf("REVERSE RESYNC GATE FAILED\n");
	return 1;
}
