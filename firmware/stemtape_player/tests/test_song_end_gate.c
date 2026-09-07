/*
 * test_song_end_gate.c -- the natural end of a song, and the loop it must not
 * be confused with.
 *
 * ======================================================================
 * WHY THIS GATE EXISTS
 * ======================================================================
 * Every playback stream was initialised with loop_enabled = true, so
 * st_stream_advance_frames() answered "the last frame was consumed" by setting
 * song_frame = 0 and returning ST_STREAM_TICK_LOOPED. main.c had no handler for
 * ST_STREAM_TICK_ENDED at all -- it could not be produced. On hardware the song
 * reached its end and STARTED AGAIN with no PLAY press: g_playing still set,
 * the reel still turning, four heads jumping to zero with residency dropped and
 * four asynchronous re-primes racing the streamer.
 *
 * The dangerous half of the fix is the confusion it invites. st_stream_t's
 * `loop_enabled` is the WHOLE-SONG WRAP inside the streaming state machine. The
 * Stem Tape loop -- the FUNCTION-held latch, the window, the division, the
 * entry, the wrap, the release -- is a different mechanism that wraps by
 * seeking inside an explicit [loop_start, loop_end) window and never reads that
 * flag. So half of this file is about end-of-song and half is a REGRESSION
 * WALL: the same fixtures, driven with a latched window, asserting that the
 * loop behaves exactly as it did before, so a future change cannot quietly
 * conflate the two again.
 *
 * ======================================================================
 * WHAT IS MODELLED, AND HOW HONEST THE MODEL IS
 * ======================================================================
 * st_stem_stream.c is the REAL module, compiled in and driven directly -- every
 * head here is a real st_stream_t and every advance is the production call.
 *
 * The transport around it (the four-head advance loop, the loop-window
 * backstop, the shared end-of-song apply, the g_playing/g_stem_eof_req handoff
 * between the audio and control threads, the inertia branch and the
 * replay-on-PLAY rewind) is a MODEL of main.c's wiring, in the same shape and
 * the same order as looper_audio_block()/stem_audio_block(). It is a model
 * because main.c cannot be linked on the host. It is therefore capable of
 * agreeing with a main.c that has drifted from it -- that limitation is real
 * and is stated here rather than glossed. What it does prove is that the
 * DESIGN, executed against the real stream module, produces the required
 * behaviour, and it is the executable statement of the authoritative EOF rule.
 *
 * Build (from the repo root):
 *   cc -std=c11 -Wall -Wextra -Werror -Ifirmware/stemtape_player/src \
 *      firmware/stemtape_player/src/st_inertia.c \
 *      firmware/stemtape_player/src/st_stem_stream.c \
 *      firmware/stemtape_player/tests/test_song_end_gate.c \
 *      -o test_song_end_gate && ./test_song_end_gate
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "st_stem_stream.h"
#include "st_planar.h"
#include "st_inertia.h"

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

#define SAMPLE_RATE 48000u
#define BLK_FRAMES  256u

/* A song of a whole number of sectors plus a partial one, so the end of the
 * song is NOT on a sector boundary -- the ordinary case. SONG_EXACT below is
 * the boundary case, which is where st_stream_required_sector() would go out of
 * range if anything asked a parked head for a sector. */
#define SECTORS      6u
#define SONG_FRAMES  (SECTORS * ST11_FRAMES_PER_SECTOR - 37u)
#define SONG_BLOCKS  (SECTORS * ST11_BLOCKS_PER_SECTOR)

typedef struct {
	st_stream_t   head[ST_PL_STEMS];
	st_inertia_t  inertia;
	uint8_t       transport;      /* s_stem_transport */
	bool          playing;        /* g_playing -- CONTROL thread owns it */
	bool          eof_req;        /* g_stem_eof_req -- audio sets, control clears */

	bool          rs_prev_valid[ST_PL_STEMS];
	uint32_t      rate_frac[ST_PL_STEMS];

	/* the user loop window, entirely independent of head[].loop_enabled */
	bool          lp_on;
	uint32_t      lp_lo, lp_end;

	/* THE TWO THREADS DO NOT TICK TOGETHER. The audio block is 256 frames
	 * (~5.3 ms at 48 kHz) and main.c's control loop sleeps ~8 ms, so the
	 * audio thread runs one or two blocks between control passes -- and a
	 * busy pass can make that several. ctl_div is how many audio blocks
	 * pass per control pass, and it is what makes the g_stem_eof_req gate
	 * testable: with a fast control thread the gate is redundant, and a
	 * mutation that removes it survives. */
	uint32_t      ctl_div, ctl_phase;

	/* Always false. A mutation hook: it exists so the "no rewind on PLAY"
	 * mutation can disable the rewind with a change that still COMPILES --
	 * a mutation that only breaks the build proves nothing about whether
	 * the gate would have caught the behaviour. */
	bool          frames_are_never_zero;

	/* observation */
	uint32_t      wraps;          /* g_stem_loop_wraps */
	uint32_t      eof_events;     /* how many times EOF was applied */
	bool          starve[ST_PL_STEMS]; /* test hook: this lane gets no sector */
} model_t;

/* main.c: stem_streams_init() -- four heads, one song, wrap OFF. */
static void m_init(model_t *m, uint32_t frames)
{
	uint32_t k;

	memset(m, 0, sizeof(*m));
	for (k = 0; k < ST_PL_STEMS; k++) {
		uint32_t sectors = (frames + ST11_FRAMES_PER_SECTOR - 1u) /
				    ST11_FRAMES_PER_SECTOR;

		if (!st_stream_init(&m->head[k], 0u,
				     sectors * ST11_BLOCKS_PER_SECTOR, frames,
				     sectors, /*loop_enabled=*/false)) {
			printf("  FATAL: st_stream_init failed\n");
			g_failures++;
		}
	}
	m->transport = 0u;
	m->ctl_div   = 1u;
	m->ctl_phase = 0u;
	st_inertia_reset(&m->inertia);
	m->eof_req = false;
}

/* main.c: stem_rs_drop() */
static void m_rs_drop(model_t *m)
{
	uint32_t k;

	for (k = 0; k < ST_PL_STEMS; k++) {
		m->rs_prev_valid[k] = false;
		m->rate_frac[k]     = 0u;
	}
}

/* main.c: stem_streams_end_of_song() */
static void m_end_of_song(model_t *m)
{
	uint32_t k;

	for (k = 0; k < ST_PL_STEMS; k++) {
		st_stream_end_of_song(&m->head[k]);
	}
	m_rs_drop(m);
	st_inertia_reset(&m->inertia);
	m->eof_req = true;
	m->eof_events++;
}

/* main.c: stem_streams_at_song_end() */
static bool m_at_song_end(const model_t *m)
{
	uint32_t k;

	for (k = 0; k < ST_PL_STEMS; k++) {
		if (m->head[k].song_frame >= m->head[k].frames) {
			return true;
		}
	}
	return false;
}

/* main.c: stem_streams_rewind_to_start() */
static void m_rewind(model_t *m)
{
	uint32_t k;

	for (k = 0; k < ST_PL_STEMS; k++) {
		(void)st_stream_seek(&m->head[k], 0u);
	}
	m_rs_drop(m);
}

/*
 * THE STREAMER, modelled as "the sector a head asks for is there" -- except for
 * a lane the test has marked starved, which never gets one. That is enough to
 * reproduce the case the authoritative-event rule exists for: a starved
 * transport head with a healthy lane running ahead of it.
 */
static void m_serve(model_t *m)
{
	uint32_t k;

	for (k = 0; k < ST_PL_STEMS; k++) {
		if (m->starve[k]) {
			continue;
		}
		if (m->head[k].state == ST_STREAM_END_OF_SONG ||
		    m->head[k].state == ST_STREAM_START_OF_SONG) {
			continue;   /* nothing may ask a parked head for a sector */
		}
		st_stream_sector_ready(&m->head[k],
					st_stream_required_sector(&m->head[k]));
	}
}

/*
 * ONE AUDIO BLOCK, in main.c's order:
 *   inertia branch (eof_req first) -> gate -> replay rewind -> play ->
 *   [ advance all four -> loop backstop -> shared EOF apply ]
 *
 * The per-run source clamps are collapsed to "advance at most to the next
 * boundary", which is what they compute at 1x. Rate is 1x throughout; this gate
 * is about the end of the tape, not about varispeed.
 */
static void m_audio_block(model_t *m)
{
	uint32_t k;
	uint32_t left = BLK_FRAMES;

	/* looper_audio_block(): the three-way inertia branch */
	if (m->eof_req) {
		st_inertia_reset(&m->inertia);
	} else if (m->playing) {
		st_inertia_play(&m->inertia, SAMPLE_RATE);
	} else {
		st_inertia_stop(&m->inertia, SAMPLE_RATE);
	}

	/* looper_audio_block(): the gate */
	if (m->eof_req || !(m->playing || st_inertia_moving(&m->inertia))) {
		/* the "no stem song playing" tail */
		for (k = 0; k < ST_PL_STEMS; k++) {
			st_stream_stop(&m->head[k]);
		}
		st_inertia_reset(&m->inertia);
		m_rs_drop(m);
		return;
	}

	/* looper_audio_block(): a PLAY that follows the end of the song */
	if (m_at_song_end(m)) {
		m_rewind(m);
	}
	for (k = 0; k < ST_PL_STEMS; k++) {
		st_stream_play(&m->head[k]);
	}
	st_inertia_advance(&m->inertia, BLK_FRAMES);

	while (left > 0u) {
		uint32_t run = left;
		bool eof_seen = false;

		m_serve(m);

		/* the per-head source bounds, then the minimum */
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

		/* advance all four, observing -- never acting */
		for (k = 0; k < ST_PL_STEMS; k++) {
			st_stream_tick_t tk =
				st_stream_advance_frames(&m->head[k], run);

			if (tk == ST_STREAM_TICK_ENDED) {
				eof_seen = true;
			}
			if (tk == ST_STREAM_TICK_START_REACHED) {
				m->rs_prev_valid[k] = false;
				m->rate_frac[k]     = 0u;
			}
		}
		left -= run;

		/* the loop-window backstop */
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

		/* THE SHARED END OF SONG, below the backstop */
		if (eof_seen && !m->lp_on) {
			m_end_of_song(m);
			break;
		}
	}
}

/* main.c's control loop: the ONE place g_playing is cleared for EOF. */
static void m_control_pass(model_t *m)
{
	if (m->eof_req) {
		m->playing = false;
		m->eof_req = false;
	}
}

/* One full pass of the machine: an audio block, and a control pass whenever
 * the (slower) control loop is due. */
static void m_tick(model_t *m)
{
	if (m->ctl_phase == 0u) {
		m_control_pass(m);
	}
	m->ctl_phase = (m->ctl_phase + 1u) % (m->ctl_div ? m->ctl_div : 1u);
	m_audio_block(m);
}

static void m_run(model_t *m, uint32_t blocks)
{
	while (blocks--) {
		m_tick(m);
	}
}

/* Blocks needed to traverse the whole song at 1x, with slack. */
static uint32_t blocks_for(uint32_t frames)
{
	return frames / BLK_FRAMES + 16u;
}

/* ======================================================================
 * 1. A NATURAL END OF SONG STOPS, AND DOES NOT WRAP TO ZERO
 * ====================================================================== */

static void t_natural_eof_stops(void)
{
	model_t m;

	case_begin("natural EOF stops the transport and never wraps to frame 0");
	m_init(&m, SONG_FRAMES);
	m.playing = true;
	m_run(&m, blocks_for(SONG_FRAMES));

	CHECK(m.eof_events == 1u,
	      "end of song applied exactly once, got %u", m.eof_events);
	CHECK(!m.playing, "the transport request is cleared at EOF");
	CHECK(!m.eof_req, "the EOF request is consumed, not left latched");
	CHECK(!st_inertia_moving(&m.inertia),
	      "the reel is at rest, state=%u", (unsigned)m.inertia.state);
	for (uint32_t k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame == SONG_FRAMES,
		      "head %u parked at the song end, got %u", k,
		      m.head[k].song_frame);
		CHECK(m.head[k].song_frame != 0u,
		      "head %u did NOT wrap to frame 0", k);
		CHECK(m.head[k].ready_sector == ST_STREAM_NO_SECTOR,
		      "head %u residency invalidated", k);
	}
}

/* ======================================================================
 * 2. IT STAYS STOPPED. NO UNSOLICITED RESTART.
 * ====================================================================== */

static void t_stays_stopped(void)
{
	model_t m;
	uint32_t k;

	case_begin("after EOF the device stays stopped -- no unsolicited restart");
	m_init(&m, SONG_FRAMES);
	m.playing = true;
	m_run(&m, blocks_for(SONG_FRAMES));
	CHECK(m.eof_events == 1u, "reached EOF once");

	/* Ten more seconds of blocks with nobody touching anything. */
	m_run(&m, (10u * SAMPLE_RATE) / BLK_FRAMES);

	CHECK(!m.playing, "still stopped ten seconds later");
	CHECK(m.eof_events == 1u,
	      "the song did NOT restart and end again (%u EOF events)",
	      m.eof_events);
	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame == SONG_FRAMES,
		      "head %u has not moved off the song end (%u)", k,
		      m.head[k].song_frame);
	}
}

static void t_slow_control_thread(void)
{
	uint32_t div, off;

	case_begin("a SLOW control thread still cannot produce a restart");

	/*
	 * SWEPT, not sampled. The audio block is 256 frames (~5.3 ms) and the
	 * control loop sleeps ~8 ms, so the two threads are not in phase and
	 * the number of audio blocks that run with the request pending, and
	 * g_playing still set, depends on where in the control period the song
	 * happens to end. Testing one phase tests one of them; the window this
	 * is about is the one where the control thread is LATE, and a single
	 * fixture can miss it entirely -- it did, and a mutation that removed
	 * the gate on the stem branch survived until this became a sweep.
	 */
	for (div = 1u; div <= 4u; div++) {
		for (off = 0u; off < div; off++) {
			model_t m;
			uint32_t k;

			m_init(&m, SONG_FRAMES);
			m.playing   = true;
			m.ctl_div   = div;
			m.ctl_phase = off;
			m_run(&m, blocks_for(SONG_FRAMES) * 2u);

			CHECK(m.eof_events == 1u,
			      "div=%u off=%u: the song ended exactly once, got %u",
			      div, off, m.eof_events);
			CHECK(!m.playing,
			      "div=%u off=%u: the transport request was cleared",
			      div, off);
			CHECK(!st_inertia_moving(&m.inertia),
			      "div=%u off=%u: the reel is at rest", div, off);
			for (k = 0; k < ST_PL_STEMS; k++) {
				CHECK(m.head[k].song_frame == SONG_FRAMES,
				      "div=%u off=%u: head %u still parked at the "
				      "song end, got %u", div, off, k,
				      m.head[k].song_frame);
			}

			/* And a PLAY still works from there, at every phase. */
			m.playing = true;
			m_run(&m, 2u);
			for (k = 0; k < ST_PL_STEMS; k++) {
				CHECK(m.head[k].song_frame > 0u &&
				      m.head[k].song_frame <= 2u * BLK_FRAMES,
				      "div=%u off=%u: head %u restarted from the "
				      "top (now %u)", div, off, k,
				      m.head[k].song_frame);
			}
		}
	}
}

/* ======================================================================
 * 3. ALL FOUR STEMS AGREE ON EOF -- INCLUDING WHEN ONE IS STARVED
 * ====================================================================== */

static void t_all_four_agree(void)
{
	model_t m;
	uint32_t k;

	case_begin("all four heads end at the same frame, in the same block");
	m_init(&m, SONG_FRAMES);
	m.playing = true;
	m_run(&m, blocks_for(SONG_FRAMES));

	for (k = 1; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame == m.head[0].song_frame,
		      "head %u agrees with head 0 (%u vs %u)", k,
		      m.head[k].song_frame, m.head[0].song_frame);
	}

	/* THE CASE THE RULE EXISTS FOR: the TRANSPORT head is the starved one,
	 * so a lane runs ahead of it and reaches the end first. Taking the
	 * first lane rather than the transport is what stops all four together
	 * instead of parking one while three play on. */
	case_begin("a starved transport head does not split the four lanes");
	m_init(&m, SONG_FRAMES);
	m.playing   = true;
	m.transport = 0u;
	m.starve[0] = true;
	m_run(&m, blocks_for(SONG_FRAMES));

	CHECK(m.eof_events == 1u,
	      "EOF still fired exactly once with the transport starved, got %u",
	      m.eof_events);
	CHECK(!m.playing, "and it stopped the transport");
	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame == SONG_FRAMES,
		      "starved-transport case: head %u at the song end, got %u",
		      k, m.head[k].song_frame);
		CHECK(m.head[k].state == ST_STREAM_STOPPED ||
		      m.head[k].state == ST_STREAM_END_OF_SONG,
		      "starved-transport case: head %u is not still playing", k);
	}
}

/* ======================================================================
 * 4. THE REEL GOES TO THE STOPPED STATE, WITHOUT A SPIN-DOWN PAST THE TAPE
 * ====================================================================== */

static void t_inertia_stops(void)
{
	model_t m;
	uint32_t guard;

	case_begin("the reel reaches ST_INERTIA_STOPPED at the end of the tape");
	m_init(&m, SONG_FRAMES);
	m.playing = true;

	/* run to EOF */
	for (guard = 0; guard < blocks_for(SONG_FRAMES); guard++) {
		m_tick(&m);
		if (m.eof_events) {
			break;
		}
	}
	CHECK(m.eof_events == 1u, "reached EOF");
	CHECK(m.inertia.state == ST_INERTIA_STOPPED,
	      "the reel is at rest in the SAME block the song ended, state=%u",
	      (unsigned)m.inertia.state);
	CHECK(st_inertia_env_q16(&m.inertia) == 0u,
	      "and the envelope is zero, got %u",
	      st_inertia_env_q16(&m.inertia));

	/* One more pass: the control thread clears g_playing, and the reel must
	 * NOT be spun back up in the window before it does. */
	m_tick(&m);
	CHECK(!m.playing, "control thread cleared the transport request");
	CHECK(m.inertia.state == ST_INERTIA_STOPPED,
	      "the reel did not spin back up in the handoff window, state=%u",
	      (unsigned)m.inertia.state);
}

/* ======================================================================
 * 5. MANUAL PLAY AFTER EOF: A DELIBERATE RESTART FROM FRAME 0
 * ====================================================================== */

static void t_manual_replay(void)
{
	model_t m;
	uint32_t k;

	case_begin("PLAY after EOF restarts from frame 0, all four coherent");
	m_init(&m, SONG_FRAMES);
	m.playing = true;
	m_run(&m, blocks_for(SONG_FRAMES));
	CHECK(m.eof_events == 1u, "reached EOF");

	/* Let it sit stopped, as the player would. */
	m_run(&m, 40u);

	/* THE PRESS. */
	m.playing = true;
	m_tick(&m);

	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame > 0u &&
		      m.head[k].song_frame <= BLK_FRAMES,
		      "head %u restarted from the top (now %u)", k,
		      m.head[k].song_frame);
		CHECK(m.head[k].song_frame == m.head[0].song_frame,
		      "head %u restarted level with head 0 (%u vs %u)", k,
		      m.head[k].song_frame, m.head[0].song_frame);
		CHECK(m.head[k].state == ST_STREAM_PLAYING,
		      "head %u is playing again, state=%u", k,
		      (unsigned)m.head[k].state);
		CHECK(!m.rs_prev_valid[k],
		      "head %u carries no stale interpolation state", k);
		CHECK(m.rate_frac[k] == 0u,
		      "head %u starts on a whole frame, frac=%u", k,
		      m.rate_frac[k]);
	}
	CHECK(m.eof_events == 1u, "the restart is not itself an EOF event");

	/* And it plays all the way to the end a second time, once. */
	m_run(&m, blocks_for(SONG_FRAMES));
	CHECK(m.eof_events == 2u,
	      "the replayed song ends again, exactly once (%u)", m.eof_events);
	CHECK(!m.playing, "and stops again");
}

/* A song whose length is an exact multiple of the sector size: the boundary
 * case where a parked head's required sector would be sector_count. Nothing may
 * ask it for one, and the replay must still be clean. */
static void t_exact_sector_multiple(void)
{
	model_t m;
	uint32_t k;
	const uint32_t frames = SECTORS * ST11_FRAMES_PER_SECTOR;

	case_begin("a song ending exactly on a sector boundary ends and replays");
	m_init(&m, frames);
	m.playing = true;
	m_run(&m, blocks_for(frames));

	CHECK(m.eof_events == 1u, "ended once, got %u", m.eof_events);
	CHECK(!m.playing, "and stopped");
	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame == frames,
		      "head %u parked at %u", k, m.head[k].song_frame);
	}

	m.playing = true;
	m_tick(&m);
	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame > 0u &&
		      m.head[k].song_frame <= BLK_FRAMES,
		      "head %u replayed from the top (now %u)", k,
		      m.head[k].song_frame);
	}
}

/* ======================================================================
 * 6. THE REGRESSION WALL: THE USER LOOP IS A DIFFERENT MECHANISM
 * ====================================================================== */

static void t_loop_still_wraps(void)
{
	model_t m;
	uint32_t k;
	const uint32_t lo = 3u * ST11_FRAMES_PER_SECTOR;
	const uint32_t hi = lo + 1000u;

	case_begin("a latched loop wraps at loop_end, and the song never ends");
	m_init(&m, SONG_FRAMES);
	m.playing = true;
	m.lp_on   = true;
	m.lp_lo   = lo;
	m.lp_end  = hi;
	for (k = 0; k < ST_PL_STEMS; k++) {
		(void)st_stream_seek(&m.head[k], lo);
	}

	/* Far longer than the whole song: with the window latched it must never
	 * reach the end of the song at all. */
	m_run(&m, blocks_for(SONG_FRAMES) * 3u);

	CHECK(m.eof_events == 0u,
	      "a latched loop produced NO end-of-song event, got %u",
	      m.eof_events);
	CHECK(m.playing, "and the transport is still running");
	CHECK(m.wraps > 0u, "the loop actually wrapped, %u times", m.wraps);
	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame >= lo && m.head[k].song_frame < hi,
		      "head %u is inside the window [%u,%u), at %u", k, lo, hi,
		      m.head[k].song_frame);
	}
}

static void t_loop_at_song_end(void)
{
	model_t m;
	uint32_t k;
	const uint32_t hi = SONG_FRAMES;
	const uint32_t lo = SONG_FRAMES - 3000u;

	case_begin("a loop whose end IS the song end wraps, it does not end the song");
	m_init(&m, SONG_FRAMES);
	m.playing = true;
	m.lp_on   = true;
	m.lp_lo   = lo;
	m.lp_end  = hi;
	for (k = 0; k < ST_PL_STEMS; k++) {
		(void)st_stream_seek(&m.head[k], lo);
	}
	m_run(&m, 400u);

	CHECK(m.eof_events == 0u,
	      "the window owns the boundary: no end-of-song, got %u",
	      m.eof_events);
	CHECK(m.playing, "the transport is still running");
	CHECK(m.wraps > 0u, "it wrapped %u times", m.wraps);
	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame >= lo && m.head[k].song_frame < hi,
		      "head %u is inside the window, at %u", k,
		      m.head[k].song_frame);
		CHECK(m.head[k].state != ST_STREAM_END_OF_SONG,
		      "head %u is not parked at end-of-song", k);
	}
}

static void t_loop_release_preserved(void)
{
	model_t m;
	uint32_t k;
	uint32_t at_release[ST_PL_STEMS];
	const uint32_t lo = 2u * ST11_FRAMES_PER_SECTOR;
	const uint32_t hi = lo + 900u;

	case_begin("releasing the loop keeps the position and continues forward");
	m_init(&m, SONG_FRAMES);
	m.playing = true;
	m.lp_on   = true;
	m.lp_lo   = lo;
	m.lp_end  = hi;
	for (k = 0; k < ST_PL_STEMS; k++) {
		(void)st_stream_seek(&m.head[k], lo);
	}
	m_run(&m, 12u);   /* well inside the window, mid-lap */

	for (k = 0; k < ST_PL_STEMS; k++) {
		at_release[k] = m.head[k].song_frame;
		CHECK(at_release[k] > lo && at_release[k] < hi,
		      "head %u is mid-lap before the release, at %u", k,
		      at_release[k]);
	}

	/* THE RELEASE: the window is dropped and NOTHING ELSE happens. No seek,
	 * no jump to loop_end, no catch-up. */
	m.lp_on = false;
	m_tick(&m);

	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame > at_release[k],
		      "head %u continued FORWARD from where it was (%u -> %u)",
		      k, at_release[k], m.head[k].song_frame);
		CHECK(m.head[k].song_frame <= at_release[k] + BLK_FRAMES,
		      "head %u did not jump (%u -> %u, one block is %u)", k,
		      at_release[k], m.head[k].song_frame, BLK_FRAMES);
		CHECK(m.head[k].song_frame != hi,
		      "head %u did not seek to loop_end", k);
		CHECK(m.head[k].song_frame != lo,
		      "head %u did not seek to loop_start", k);
	}

	/* FUTURE WRAPS ARE DISABLED: it plays straight through the old window
	 * end and on to the natural end of the song, which now ends normally. */
	{
		uint32_t before = m.wraps;

		m_run(&m, blocks_for(SONG_FRAMES));
		CHECK(m.wraps == before,
		      "no wrap happened after the release (%u -> %u)", before,
		      m.wraps);
		CHECK(m.eof_events == 1u,
		      "and the song then ended naturally, once (%u)",
		      m.eof_events);
		CHECK(!m.playing, "and stopped");
	}
}

static void t_loop_entry_unmoved(void)
{
	model_t m;
	uint32_t k;
	uint32_t at_entry[ST_PL_STEMS];
	/* A window that CONTAINS where ten blocks of playback leaves the
	 * playhead, so the latch is the ordinary case: latching around the
	 * position you are already at. */
	const uint32_t lo = 4u * ST11_FRAMES_PER_SECTOR;
	const uint32_t hi = lo + 900u;

	case_begin("latching a loop changes the rules, never the position");
	m_init(&m, SONG_FRAMES);
	m.playing = true;
	m_run(&m, 10u);

	for (k = 0; k < ST_PL_STEMS; k++) {
		at_entry[k] = m.head[k].song_frame;
	}
	CHECK(at_entry[0] > lo && at_entry[0] < hi,
	      "the playhead is inside the window to be latched, at %u",
	      at_entry[0]);

	/* THE LATCH. Nothing but the window is set. */
	m.lp_on  = true;
	m.lp_lo  = lo;
	m.lp_end = hi;
	m_tick(&m);

	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(m.head[k].song_frame > at_entry[k],
		      "head %u kept moving forward from where it was (%u -> %u)",
		      k, at_entry[k], m.head[k].song_frame);
		CHECK(m.head[k].song_frame <= at_entry[k] + BLK_FRAMES,
		      "head %u was not moved by the latch (%u -> %u)", k,
		      at_entry[k], m.head[k].song_frame);
	}
	CHECK(m.eof_events == 0u, "and no end-of-song was produced");
}

/* ======================================================================
 * 7. THE MODULE ITSELF: THE TWO FLAG VALUES, SIDE BY SIDE
 * ====================================================================== */

static void t_module_flag(void)
{
	st_stream_t a, b;
	const uint32_t frames = 2u * ST11_FRAMES_PER_SECTOR;
	const uint32_t sectors = 2u;
	st_stream_tick_t tk;

	case_begin("st_stream_t::loop_enabled is the WHOLE-SONG wrap, and only that");

	/* loop_enabled = false: the end of the song is the end. */
	CHECK(st_stream_init(&a, 0u, sectors * ST11_BLOCKS_PER_SECTOR, frames,
			      sectors, false), "init a");
	st_stream_play(&a);
	a.song_frame = frames - 1u;
	st_stream_sector_ready(&a, st_stream_required_sector(&a));
	tk = st_stream_advance_frames(&a, 1u);
	CHECK(tk == ST_STREAM_TICK_ENDED, "ENDED, got %d", (int)tk);
	CHECK(a.state == ST_STREAM_END_OF_SONG, "state is END_OF_SONG");
	CHECK(a.song_frame == frames, "parked at %u, want %u", a.song_frame,
	      frames);

	/* AND IT IS STICKY: st_stream_play() does NOT lift it. This is what
	 * makes "the song ended" survive a transport that re-asserts PLAY on
	 * every audio block, and it is why the replay path must seek. */
	st_stream_play(&a);
	CHECK(a.state == ST_STREAM_END_OF_SONG,
	      "st_stream_play() does not resurrect a finished song, state=%u",
	      (unsigned)a.state);
	tk = st_stream_advance_frames(&a, 1u);
	CHECK(tk == ST_STREAM_TICK_NOT_PLAYING,
	      "and it still refuses to advance, got %d", (int)tk);

	/* Only an explicit position change lifts it. */
	CHECK(st_stream_seek(&a, 0u), "seek(0) succeeds");
	CHECK(a.state == ST_STREAM_PLAYING, "and lifts END_OF_SONG to PLAYING");
	CHECK(a.song_frame == 0u, "at frame 0");
	CHECK(a.ready_sector == ST_STREAM_NO_SECTOR, "with residency dropped");

	/* loop_enabled = true: the OLD behaviour, kept working and kept
	 * out of the playback path. This is the defect, reproduced: the song
	 * silently restarts. */
	CHECK(st_stream_init(&b, 0u, sectors * ST11_BLOCKS_PER_SECTOR, frames,
			      sectors, true), "init b");
	st_stream_play(&b);
	b.song_frame = frames - 1u;
	st_stream_sector_ready(&b, st_stream_required_sector(&b));
	tk = st_stream_advance_frames(&b, 1u);
	CHECK(tk == ST_STREAM_TICK_LOOPED, "LOOPED, got %d", (int)tk);
	CHECK(b.song_frame == 0u,
	      "the whole-song wrap is what restarted the song, at %u",
	      b.song_frame);

	/* st_stream_end_of_song() parks a head that has NOT run out itself --
	 * the other three lanes. */
	case_begin("st_stream_end_of_song() parks any head, coherently");
	CHECK(st_stream_init(&a, 0u, sectors * ST11_BLOCKS_PER_SECTOR, frames,
			      sectors, false), "init");
	st_stream_play(&a);
	a.song_frame = 17u;
	st_stream_sector_ready(&a, 0u);
	a.reverse = true;
	st_stream_end_of_song(&a);
	CHECK(a.song_frame == frames, "parked at %u, want %u", a.song_frame,
	      frames);
	CHECK(a.state == ST_STREAM_END_OF_SONG, "state is END_OF_SONG");
	CHECK(a.ready_sector == ST_STREAM_NO_SECTOR, "residency invalidated");
	CHECK(a.reverse, "reverse is NOT cleared -- a song ending is not a "
	      "reverse gesture");
}

/* ======================================================================
 * 8. THE SOURCE ITSELF: THE TWO MECHANISMS SHARE NO STATE
 * ======================================================================
 * The gate above proves behaviour. This proves the SEPARATION that keeps the
 * behaviour true: the loop engine does not read the whole-song wrap flag, so
 * turning that flag off cannot reach loop entry, wrap or release. CI's wiring
 * check asserts the same thing over the tree; this is the copy that travels
 * with the test, and it names the file it is about.
 */

static void t_no_shared_state(void)
{
	static const char *const loop_sources[] = {
		"firmware/stemtape_player/src/st_loop.c",
		"firmware/stemtape_player/src/st_loop.h",
	};
	size_t i;

	case_begin("the user loop engine never reads st_stream_t::loop_enabled");
	for (i = 0; i < sizeof(loop_sources) / sizeof(loop_sources[0]); i++) {
		FILE *fh = fopen(loop_sources[i], "r");
		char line[512];
		int hits = 0;
		int lineno = 0;

		if (!fh) {
			printf("  SKIP %s (run from the repo root to check it)\n",
			       loop_sources[i]);
			continue;
		}
		while (fgets(line, sizeof(line), fh)) {
			lineno++;
			if (strstr(line, "loop_enabled")) {
				hits++;
				printf("    %s:%d: %s", loop_sources[i], lineno,
				       line);
			}
		}
		fclose(fh);
		CHECK(hits == 0,
		      "%s must not mention loop_enabled (%d hit(s)) -- the "
		      "whole-song wrap and the Stem Tape loop are different "
		      "mechanisms and must stay that way",
		      loop_sources[i], hits);
	}
}

int main(void)
{
	printf("== Stem Tape song-end gate ==\n\n");

	t_natural_eof_stops();
	t_stays_stopped();
	t_slow_control_thread();
	t_all_four_agree();
	t_inertia_stops();
	t_manual_replay();
	t_exact_sector_multiple();
	t_loop_still_wraps();
	t_loop_at_song_end();
	t_loop_release_preserved();
	t_loop_entry_unmoved();
	t_module_flag();
	t_no_shared_state();

	printf("\n%d cases, %d checks, %d failures\n", g_cases, g_checks,
	       g_failures);
	if (g_failures == 0) {
		printf("SONG END GATE PASSED\n");
		return 0;
	}
	printf("SONG END GATE FAILED\n");
	return 1;
}
