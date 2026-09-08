/*
 * test_reverse_start_gate.c -- a reversed stem that reaches the front of the
 * song parks there and becomes BORING.
 *
 * ======================================================================
 * THE INVARIANT
 * ======================================================================
 *
 *     A head in ST_STREAM_START_OF_SONG is terminal for source consumption.
 *     It consumes no source frames, contributes silence, does not bound the
 *     source run, and does not trip the whole-mix underrun guard merely
 *     because its song_frame happens to equal the transport's.
 *
 *     MASTER and the other three forward stems continue normally.
 *
 * ======================================================================
 * WHAT WENT WRONG ON HARDWARE
 * ======================================================================
 * The per-head source-bound loop excluded a head only on `!resident[sk]`. It
 * never looked at state. A reversed head parked at frame 0 is still sitting on
 * a validated sector 0, so it was treated as an active reader:
 *
 *     fis = 0  ->  rk = fis + 1 = 1     (the `rk > pos_k + 1` clamp is 1 > 1,
 *                                        so it does not catch this)
 *              ->  run = min(everything, 1) = 1
 *
 * One source frame per run is one OUTPUT frame per run (st_rs_out_frames()
 * floors at 1), so a 256-frame block took 256 full passes of the run loop --
 * pin lookups, mailbox acquires, request publishes, divisions, the bounds
 * computation, the seam checks and a render call, each time -- where ordinary
 * playback takes one or two. The read path is CPU-bound on this part, so the
 * streamer (priority 1) starved beneath the audio thread (priority 0), the
 * forward heads missed residency, the co-location guard then froze every head
 * for whole blocks, and the song clock and beat phase audibly slowed. Reported
 * from hardware as heavy crackling, a dramatic BPM slowdown, and a device that
 * could no longer decode a PLAY tap or a reverse double-tap because MAIN had
 * stopped getting passes.
 *
 * It also sounded wrong on its own account: rendering fis = 0 on all 256 passes
 * emitted frame 0 forty-eight thousand times a second, a constant, not audio.
 *
 * ======================================================================
 * WHAT IS MODELLED, AND HOW HONEST THE MODEL IS
 * ======================================================================
 * st_stem_stream.c is the REAL module: every head is a real st_stream_t and
 * every advance, seek and set_reverse is the production call.
 *
 * The transport around it -- needed/residency/the co-location guard/the
 * per-head source bounds/the silent-group choice/the four-head advance/the loop
 * backstop/the reverse-consume block -- is a MODEL of stem_audio_block(), in
 * the same shape and the same order. It is a model because main.c cannot be
 * linked on the host, so it CAN agree with a main.c that has drifted from it.
 * The wiring check reads the production file separately for that reason.
 *
 * Everything runs at 1x. This gate is about WHICH HEAD BOUNDS THE RUN and WHAT
 * IT RENDERS, not about resampling.
 *
 * Build (from the repo root):
 *   cc -std=c11 -Wall -Wextra -Werror -Ifirmware/stemtape_player/src \
 *      firmware/stemtape_player/src/st_stem_stream.c \
 *      firmware/stemtape_player/tests/test_reverse_start_gate.c \
 *      -o test_reverse_start_gate && ./test_reverse_start_gate
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
	uint8_t     transport;

	bool        rs_prev_valid[ST_PL_STEMS];
	uint32_t    rate_frac[ST_PL_STEMS];

	bool        lp_on;
	uint32_t    lp_lo, lp_end;
	uint32_t    wraps;

	bool        starve[ST_PL_STEMS];      /* test hook: no sector for this lane */

	/* ---- observation, reset per block ---- */
	uint32_t    runs;                     /* run-loop iterations this block */
	uint32_t    min_run;                  /* smallest `run` this block */
	uint32_t    block_underruns;          /* co-location guard firings, this block */
	/* LIFETIME, never reset. The guard fires at most ONCE per starvation
	 * episode: it freezes every head, the starved lane then fails to
	 * advance while the others do, and from the next block their positions
	 * differ so the position test stops matching. A per-block counter
	 * therefore reads zero on every block but one, which is not the same as
	 * "the guard never fired". */
	uint32_t    total_underruns;
	uint32_t    real_frames[ST_PL_STEMS]; /* rendered from a REAL group */
	uint32_t    zero_frames[ST_PL_STEMS]; /* rendered from the all-zero group */
	uint32_t    song_frame_pub;           /* g_stem_song_frame_pub */
} model_t;

static void m_init(model_t *m)
{
	uint32_t k;

	memset(m, 0, sizeof(*m));
	for (k = 0; k < ST_PL_STEMS; k++) {
		if (!st_stream_init(&m->head[k], 0u,
				     SECTORS * ST11_BLOCKS_PER_SECTOR,
				     SONG_FRAMES, SECTORS, /*loop_enabled=*/false)) {
			printf("  FATAL: st_stream_init failed\n");
			g_failures++;
		}
		st_stream_play(&m->head[k]);
	}
	m->transport = 0u;
}

/* main.c's reverse-consume block (st63), unchanged by this stage. */
static void m_reverse_toggle(model_t *m, uint32_t k)
{
	uint32_t j;
	bool turning_on;
	uint32_t master;

	if (k >= ST_PL_STEMS) {
		return;
	}
	turning_on = !m->head[k].reverse;
	master = m->head[m->transport].song_frame;

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
	if (turning_on && !m->head[k].reverse) {
		st_stream_set_reverse(&m->head[k], true);
		m->rs_prev_valid[k] = false;
		m->rate_frac[k]     = 0u;
	}
	for (j = 0; j < ST_PL_STEMS; j++) {
		if (!m->head[j].reverse) {
			m->transport = (uint8_t)j;
			break;
		}
	}
}

/*
 * ONE AUDIO BLOCK, in stem_audio_block()'s order and with its per-run structure
 * preserved -- because the run-loop ITERATION COUNT is one of the things under
 * test, the loop cannot be collapsed.
 */
static void m_audio_block(model_t *m)
{
	uint32_t f = 0u;
	uint32_t k;

	m->runs            = 0u;
	m->min_run         = ST11_FRAMES_PER_SECTOR;
	m->block_underruns = 0u;
	for (k = 0; k < ST_PL_STEMS; k++) {
		m->real_frames[k] = 0u;
		m->zero_frames[k] = 0u;
	}

	while (f < BLK_FRAMES) {
		uint32_t needed[ST_PL_STEMS];
		bool     resident[ST_PL_STEMS];
		bool     from_zero[ST_PL_STEMS];
		uint32_t run = ST11_FRAMES_PER_SECTOR;
		uint32_t out_n;
		bool     block_underrun = false;
		const st_stream_t *tr = &m->head[m->transport];

		for (k = 0; k < ST_PL_STEMS; k++) {
			needed[k] = st_stream_required_sector(&m->head[k]);
		}

		/* the producer, plus the residency line under test */
		for (k = 0; k < ST_PL_STEMS; k++) {
			if (!m->starve[k] &&
			    m->head[k].state != ST_STREAM_END_OF_SONG &&
			    m->head[k].state != ST_STREAM_START_OF_SONG) {
				st_stream_sector_ready(&m->head[k], needed[k]);
			}
			resident[k] = (m->head[k].ready_sector == needed[k]) &&
				      (m->head[k].state != ST_STREAM_START_OF_SONG);
		}

		/* the co-location guard */
		for (k = 0; k < ST_PL_STEMS; k++) {
			if (!resident[k] &&
			    m->head[k].state != ST_STREAM_START_OF_SONG &&
			    m->head[k].song_frame == tr->song_frame) {
				block_underrun = true;
				break;
			}
		}
		if (block_underrun) {
			m->block_underruns++;
			m->total_underruns++;
			for (k = 0; k < ST_PL_STEMS; k++) {
				(void)st_stream_advance_frames(&m->head[k], 1u);
			}
			break;
		}

		/* the per-head source bounds, then the minimum. A silent head
		 * borrows the transport's offset and a forward direction in
		 * production; here only the fact that it BOUNDS NOTHING is
		 * modelled, because that is what is under test. */
		for (k = 0; k < ST_PL_STEMS; k++) {
			const st_stream_t *h = &m->head[k];
			uint32_t fis, rk;

			if (!resident[k]) {
				from_zero[k] = true;   /* the all-zero group */
				continue;              /* bounds nothing */
			}
			from_zero[k] = false;
			fis = h->song_frame % ST11_FRAMES_PER_SECTOR;
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
		out_n = (run > BLK_FRAMES - f) ? (BLK_FRAMES - f) : run;   /* 1x */

		/* the buffer choice: the ONLY thing that makes a head silent */
		for (k = 0; k < ST_PL_STEMS; k++) {
			if (from_zero[k]) {
				m->zero_frames[k] += out_n;
			} else {
				m->real_frames[k] += out_n;
			}
		}

		m->runs++;
		if (run < m->min_run) {
			m->min_run = run;
		}
		f += out_n;

		for (k = 0; k < ST_PL_STEMS; k++) {
			(void)st_stream_advance_frames(&m->head[k], out_n);
		}

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

	/* audio_block_epilogue(): the ONE published song clock */
	m->song_frame_pub = m->head[m->transport].song_frame;
}

static void m_run(model_t *m, uint32_t blocks)
{
	while (blocks--) {
		m_audio_block(m);
	}
}

/* Park stem `rev` at frame 0 by reversing it and letting it travel back.
 * Returns the number of blocks it took. */
static uint32_t park_at_zero(model_t *m, uint32_t rev, uint32_t lead_blocks)
{
	uint32_t n = 0u;

	m_run(m, lead_blocks);
	m_reverse_toggle(m, rev);
	while (m->head[rev].state != ST_STREAM_START_OF_SONG && n < 400u) {
		m_audio_block(m);
		n++;
	}
	return n;
}

/* ======================================================================
 * 1-7. THE PARKED HEAD IS BORING
 * ====================================================================== */

static void t_parked_is_boring(uint32_t rev, uint32_t lead)
{
	model_t m;
	uint32_t k, b;
	uint32_t before[ST_PL_STEMS];

	m_init(&m);
	(void)park_at_zero(&m, rev, lead);

	/* 1. it reached frame 0 and entered START_OF_SONG */
	CHECK(m.head[rev].song_frame == 0u,
	      "stem %u reached frame 0 (at %u)", rev, m.head[rev].song_frame);
	CHECK(m.head[rev].state == ST_STREAM_START_OF_SONG,
	      "stem %u is parked in START_OF_SONG (state=%u)", rev,
	      (unsigned)m.head[rev].state);
	CHECK(m.head[rev].reverse, "stem %u is still reversed", rev);

	/* Now watch several whole blocks with the stem parked. */
	for (b = 0; b < 8u; b++) {
		for (k = 0; k < ST_PL_STEMS; k++) {
			before[k] = m.head[k].song_frame;
		}
		m_audio_block(&m);

		/* 2. the parked stem does not pin the run to 1 */
		CHECK(m.min_run > 1u,
		      "block %u: run was not pinned to one frame (min_run=%u)",
		      b, m.min_run);
		CHECK(m.runs <= 2u,
		      "block %u: the block took %u run-loop passes, not the "
		      "one or two ordinary playback needs (256 was the defect)",
		      b, m.runs);

		/* 3. it renders SILENCE, not repeated frame 0 */
		CHECK(m.zero_frames[rev] == BLK_FRAMES,
		      "block %u: stem %u rendered %u frames of silence, want %u",
		      b, rev, m.zero_frames[rev], BLK_FRAMES);
		CHECK(m.real_frames[rev] == 0u,
		      "block %u: stem %u rendered NO real audio (%u frames) -- "
		      "the defect emitted frame 0 over and over", b, rev,
		      m.real_frames[rev]);

		/* 4/5. MASTER and the other stems advance a full block */
		CHECK(m.head[m.transport].song_frame ==
		      before[m.transport] + BLK_FRAMES,
		      "block %u: MASTER advanced a full block (%u -> %u)", b,
		      before[m.transport], m.head[m.transport].song_frame);
		for (k = 0; k < ST_PL_STEMS; k++) {
			if (k == rev) {
				continue;
			}
			CHECK(m.head[k].song_frame == before[k] + BLK_FRAMES,
			      "block %u: stem %u advanced a full block (%u -> %u)",
			      b, k, before[k], m.head[k].song_frame);
			CHECK(m.real_frames[k] == BLK_FRAMES,
			      "block %u: stem %u rendered real audio for the "
			      "whole block (%u)", b, k, m.real_frames[k]);
		}

		/* 6. the published song clock keeps up */
		CHECK(m.song_frame_pub == before[m.transport] + BLK_FRAMES,
		      "block %u: the song clock advanced (%u -> %u)", b,
		      before[m.transport], m.song_frame_pub);

		/* 7. no whole-mix underrun caused by the parked stem */
		CHECK(m.block_underruns == 0u && m.total_underruns == 0u,
		      "block %u: the parked stem never tripped the co-location "
		      "guard (%u this block, %u ever)", b, m.block_underruns,
		      m.total_underruns);

		/* and the parked stem has not moved */
		CHECK(m.head[rev].song_frame == 0u,
		      "block %u: stem %u is still parked at 0 (at %u)", b, rev,
		      m.head[rev].song_frame);
	}
}

static void t_parked_all_stems(void)
{
	uint32_t rev;

	for (rev = 0; rev < ST_PL_STEMS; rev++) {
		char name[110];

		snprintf(name, sizeof(name),
			 "stem %u parked at frame 0: run, silence, MASTER, clock, "
			 "guard", rev);
		case_begin(name);
		t_parked_is_boring(rev, 30u);
	}
}

/*
 * THE REPORTED FIRST REPRODUCTION: reverse engaged almost immediately, so the
 * parked head sits at frame 0 while the transport is still near it. This is the
 * case where the co-location guard's POSITION test can be satisfied by pure
 * coincidence, and it is what Fix B is for.
 */
static void t_parked_at_song_start(void)
{
	model_t m;
	uint32_t k;

	/*
	 * (a) THE REPORTED FIRST REPRODUCTION, exactly: playback starts and
	 * reverse is engaged immediately, so the reversed head is sitting ON
	 * frame 0 before it has even parked. Note it pins the run for ONE
	 * iteration in that state -- reversed, resident, fis = 0 gives rk = 1 --
	 * and then the advance parks it and the fix takes over. Two passes, not
	 * 256, is the whole difference.
	 */
	case_begin("reverse engaged at the very start costs one extra run pass, "
		   "not a whole block of them");
	m_init(&m);
	m_reverse_toggle(&m, 2u);
	m_audio_block(&m);

	CHECK(m.head[2].state == ST_STREAM_START_OF_SONG,
	      "stem 2 parked immediately (state=%u)", (unsigned)m.head[2].state);
	CHECK(m.head[2].song_frame == 0u, "at frame 0");
	CHECK(m.runs <= 2u,
	      "the block took %u run-loop passes, not 256", m.runs);
	CHECK(m.block_underruns == 0u, "and stalled nothing (%u firings)",
	      m.block_underruns);
	CHECK(m.head[m.transport].song_frame == BLK_FRAMES,
	      "MASTER advanced a full block (%u)",
	      m.head[m.transport].song_frame);
	CHECK(m.zero_frames[2] >= BLK_FRAMES - 1u,
	      "stem 2 was silent for all but the pre-park frame (%u of %u)",
	      m.zero_frames[2], BLK_FRAMES);

	/*
	 * (b) THE COINCIDENCE THE GUARD CANNOT SEE, constructed directly.
	 *
	 * This is the case Fix B exists for, and it is worth being precise
	 * about WHY it is needed: it is a consequence of Fix A. Before Fix A a
	 * parked head was still `resident`, so `!resident[sk]` was false and the
	 * guard could never fire for it. Fix A makes a parked head NOT resident
	 * -- correctly, it is not reading -- which newly exposes it to the
	 * guard's position test. Without Fix B the fix would have introduced a
	 * fresh whole-mix stall wherever the transport shares frame 0 with the
	 * parked head.
	 */
	case_begin("a parked head sharing MASTER's frame must not stall the mix");
	m_init(&m);
	(void)park_at_zero(&m, 2u, 6u);
	CHECK(m.head[2].state == ST_STREAM_START_OF_SONG, "stem 2 is parked");
	for (k = 0; k < ST_PL_STEMS; k++) {
		if (k != 2u) {
			CHECK(st_stream_seek(&m.head[k], 0u),
			      "forward stem %u placed on frame 0", k);
		}
	}
	CHECK(m.head[2].song_frame == m.head[m.transport].song_frame,
	      "stem 2 and the transport are BOTH on frame %u -- the positional "
	      "coincidence the guard cannot distinguish from being in sync",
	      m.head[2].song_frame);

	m_audio_block(&m);
	CHECK(m.block_underruns == 0u,
	      "the whole mix did not stall (%u guard firings)",
	      m.block_underruns);
	CHECK(m.head[m.transport].song_frame == BLK_FRAMES,
	      "MASTER advanced a full block (%u)",
	      m.head[m.transport].song_frame);
	for (k = 0; k < ST_PL_STEMS; k++) {
		if (k == 2u) {
			continue;
		}
		CHECK(m.head[k].song_frame == BLK_FRAMES,
		      "stem %u advanced a full block (%u)", k,
		      m.head[k].song_frame);
	}
	CHECK(m.zero_frames[2] == BLK_FRAMES, "stem 2 was silent (%u frames)",
	      m.zero_frames[2]);
	CHECK(m.head[2].song_frame == 0u, "and stayed parked at 0");
}

/* A genuinely starved FORWARD lane must STILL stall the mix -- Fix B must not
 * have disarmed the guard for the case it exists for. */
static void t_guard_still_works(void)
{
	model_t m;
	uint32_t frozen;

	case_begin("a starved FORWARD head co-located with MASTER still stalls "
		   "the mix (the guard is narrowed, not disarmed)");
	m_init(&m);
	m_run(&m, 4u);
	/* Starving only stops NEW sectors being served, so the lane stays
	 * resident until it needs one it has not got. Run past the next sector
	 * boundary. */
	m.starve[1] = true;
	m_run(&m, 4u);
	frozen = m.head[m.transport].song_frame;
	m_audio_block(&m);

	CHECK(m.total_underruns > 0u,
	      "the guard fired for a starved co-located forward head (%u times "
	      "over the run)", m.total_underruns);
	CHECK(m.head[1].song_frame < m.head[m.transport].song_frame,
	      "the starved lane fell behind MASTER rather than being silently "
	      "carried (%u < %u)", m.head[1].song_frame,
	      m.head[m.transport].song_frame);
	(void)frozen;
}

/* ======================================================================
 * 8-9. RELEASE AND SWITCH FROM A PARKED HEAD (st63 semantics intact)
 * ====================================================================== */

static void t_release_from_parked(void)
{
	model_t m;
	uint32_t k, master_before;

	case_begin("release from frame 0 rejoins the current MASTER (st63)");
	m_init(&m);
	(void)park_at_zero(&m, 1u, 30u);
	m_run(&m, 5u);

	master_before = m.head[m.transport].song_frame;
	CHECK(master_before > BLK_FRAMES,
	      "MASTER is well past the start (%u)", master_before);

	m_reverse_toggle(&m, 1u);

	CHECK(!m.head[1].reverse, "stem 1 is forward again");
	CHECK(m.head[1].state == ST_STREAM_PLAYING,
	      "START_OF_SONG was lifted (state=%u)", (unsigned)m.head[1].state);
	CHECK(m.head[1].song_frame == master_before,
	      "stem 1 rejoined MASTER (%u, want %u) -- not left at 0",
	      m.head[1].song_frame, master_before);
	CHECK(m.head[1].ready_sector == ST_STREAM_NO_SECTOR,
	      "with residency invalidated, so no stale audio from frame 0");
	CHECK(!m.rs_prev_valid[1] && m.rate_frac[1] == 0u,
	      "and its carried resampler state dropped");

	m_run(&m, 6u);
	for (k = 1; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame == m.head[0].song_frame,
		      "stem %u tracks stem 0 after the rejoin (%u vs %u)", k,
		      m.head[k].song_frame, m.head[0].song_frame);
	}
}

static void t_switch_from_parked(void)
{
	model_t m;
	uint32_t k, master_before;

	case_begin("switching reverse away from a parked stem rejoins it first");
	m_init(&m);
	(void)park_at_zero(&m, 0u, 30u);
	m_run(&m, 5u);

	master_before = m.head[m.transport].song_frame;
	m_reverse_toggle(&m, 3u);              /* engage on another stem */

	CHECK(!m.head[0].reverse, "the outgoing stem 0 is forward again");
	CHECK(m.head[0].song_frame == master_before,
	      "and rejoined MASTER (%u, want %u) rather than staying at 0",
	      m.head[0].song_frame, master_before);
	CHECK(m.head[3].reverse, "the incoming stem 3 is reversed");
	CHECK(m.head[3].song_frame == master_before,
	      "the incoming stem was not moved (%u)", m.head[3].song_frame);
	CHECK(!m.head[m.transport].reverse, "the song clock is a forward head");

	m_run(&m, 4u);
	CHECK(m.head[3].song_frame < m.head[m.transport].song_frame,
	      "and stem 3 now departs backward alone (%u < %u)",
	      m.head[3].song_frame, m.head[m.transport].song_frame);
	for (k = 0; k < ST_PL_STEMS; k++) {
		if (k != 3u) {
			CHECK(m.head[k].song_frame ==
			      m.head[m.transport].song_frame,
			      "stem %u is on MASTER (%u)", k,
			      m.head[k].song_frame);
		}
	}
}

/* ======================================================================
 * 10. LOOP BEHAVIOUR UNCHANGED
 * ====================================================================== */

static void t_loop_unchanged(void)
{
	model_t m;
	uint32_t k, wraps_before;
	uint32_t at_release[ST_PL_STEMS];
	const uint32_t lo = 4u * ST11_FRAMES_PER_SECTOR;
	const uint32_t hi = lo + 2500u;

	case_begin("a latched loop still wraps, and release still preserves the "
		   "audible position");
	m_init(&m);
	for (k = 0; k < ST_PL_STEMS; k++) {
		(void)st_stream_seek(&m.head[k], lo);
	}
	m.lp_on  = true;
	m.lp_lo  = lo;
	m.lp_end = hi;

	wraps_before = m.wraps;
	m_run(&m, 60u);
	CHECK(m.wraps > wraps_before, "the loop wrapped %u times",
	      m.wraps - wraps_before);
	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame >= lo && m.head[k].song_frame < hi,
		      "stem %u stayed inside the window, at %u", k,
		      m.head[k].song_frame);
	}
	CHECK(m.block_underruns == 0u, "with no whole-mix underruns");

	for (k = 0; k < ST_PL_STEMS; k++) {
		at_release[k] = m.head[k].song_frame;
	}
	wraps_before = m.wraps;
	m.lp_on = false;
	m_audio_block(&m);
	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame > at_release[k] &&
		      m.head[k].song_frame <= at_release[k] + BLK_FRAMES,
		      "stem %u continued forward from where it was (%u -> %u)",
		      k, at_release[k], m.head[k].song_frame);
		CHECK(m.head[k].song_frame != lo && m.head[k].song_frame != hi,
		      "stem %u did not seek to either edge", k);
	}
	m_run(&m, 30u);
	CHECK(m.wraps == wraps_before, "and no further wrap happened");
}

int main(void)
{
	printf("== Stem Tape reverse start-of-song gate ==\n\n");

	t_parked_all_stems();
	t_parked_at_song_start();
	t_guard_still_works();
	t_release_from_parked();
	t_switch_from_parked();
	t_loop_unchanged();

	printf("\n%d cases, %d checks, %d failures\n", g_cases, g_checks,
	       g_failures);
	if (g_failures == 0) {
		printf("REVERSE START GATE PASSED\n");
		return 0;
	}
	printf("REVERSE START GATE FAILED\n");
	return 1;
}
