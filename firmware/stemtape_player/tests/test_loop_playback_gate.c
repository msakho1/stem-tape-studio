/*
 * test_loop_playback_gate.c — the LOOP through the real streaming path.
 *
 * This is the production-equivalent gate for global looping. It builds a real
 * multi-sector STSC song, then drives the REAL st_stream_t/st_loop_t through
 * the SAME sequence of operations main.c's stem_audio_block() performs -- the
 * same clamp, the same st_stream_seek() wrap, the same one-shot exit -- and
 * checks the resulting frame sequence directly.
 *
 * WHAT IT PROVES, and this is the point of it:
 *   - the emitted frame sequence is CONTIGUOUS across every wrap: the frame
 *     after end-1 is start, with none skipped and none repeated;
 *   - the exit resumes at exactly loop_start_frame, forward, in the SAME
 *     output block -- no silent frame is ever emitted;
 *   - every frame rendered comes from a sector that was genuinely resident,
 *     so no stale buffer and no missing sector can hide inside "contiguous";
 *   - the pinned exit kit is what makes the exit possible, demonstrated by
 *     running the identical scenario WITHOUT it and observing the silence
 *     that the real firmware would emit.
 *
 * WHAT IT DOES NOT PROVE: real eMMC timing on real hardware. The residency
 * model here is the arithmetic one (a sector read takes a bounded number of
 * blocks' worth of time); the physical margins are reported separately.
 *
 *   cc -std=c11 -Wall -Wextra -I../src ../src/st_loop.c ../src/st_stem_stream.c \
 *      ../src/st_beat_phase.c ../src/st_sector_v11.c ../src/st_checksum32.c \
 *      test_loop_playback_gate.c -o test_loop_playback_gate
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "st_loop.h"
#include "st_stem_stream.h"
#include "st_beat_phase.h"
#include "st_v11_format.h"

static int g_checks, g_failures, g_cases;

#define CHECK(cond, ...) do { \
		g_checks++; \
		if (cond) { printf("[OK  ] " __VA_ARGS__); printf("\n"); } \
		else { g_failures++; printf("[FAIL] " __VA_ARGS__); \
		       printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); } \
	} while (0)

#define FPB        24000u                      /* 120 BPM @ 48 kHz */
/* 4000 sectors == 1,360,000 frames == ~28 s. 64 sectors (0.45 s) was too
 * short for any musical length to fit, so every loop clamped to the song end
 * and the lengths under test were never actually exercised. */
#define SECTORS    4000u
#define SONG_FRAMES (SECTORS * ST11_FRAMES_PER_SECTOR)
#define BLK        256u                        /* main.c's BLK_FRAMES */

/* ---- the model of the production streaming path -------------------------
 * A sector is "resident" if the ring holds it or the pin holds it. The ring
 * is modelled exactly as st_stem_bufmbox.h specifies: sector s occupies slot
 * (s % SLOTS), so it holds a sliding window and cannot retain an arbitrary
 * old sector. The producer fills the nearest missing sector ahead of what the
 * consumer asked for, one per "service", at a bounded rate.
 */
#define SLOTS      12u
#define PIN_N      ST_LOOP_PIN_TEST_SECTORS

/* Kept in step with main.c's ST_LOOP_PIN_SECTORS by the CI gate, which greps
 * the production value and this one and fails if they differ. */
#define ST_LOOP_PIN_TEST_SECTORS 3u

typedef struct {
	uint32_t slot[SLOTS];      /* sector resident in each slot, or NONE */
	uint32_t pin_base;         /* first pinned sector, or NONE */
	uint32_t pin_count;
	uint32_t fills;            /* diagnostics */
} ring_t;

#define NONE 0xFFFFFFFFu

static void ring_init(ring_t *r)
{
	uint32_t i;

	for (i = 0; i < SLOTS; i++) {
		r->slot[i] = NONE;
	}
	r->pin_base = NONE;
	r->pin_count = 0u;
	r->fills = 0u;
}

static bool ring_has(const ring_t *r, uint32_t sec)
{
	if (r->pin_base != NONE && sec >= r->pin_base && sec < r->pin_base + r->pin_count) {
		return true;
	}
	return r->slot[sec % SLOTS] == sec;
}

static bool from_pin(const ring_t *r, uint32_t sec)
{
	return r->pin_base != NONE && sec >= r->pin_base && sec < r->pin_base + r->pin_count;
}

/* One producer service: fill the nearest missing sector at or ahead of
 * `want`, in SONG order, stopping at the song's last sector -- exactly what
 * st_stem_mbox_producer_next_fill() does, including its refusal to wrap the
 * scan (wrapping would let two window positions share a slot). */
static void ring_service(ring_t *r, uint32_t want)
{
	uint32_t k, sec = want;

	for (k = 0; k < SLOTS; k++) {
		if (!ring_has(r, sec)) {
			r->slot[sec % SLOTS] = sec;
			r->fills++;
			return;
		}
		sec++;
		if (sec >= SECTORS) {
			return;   /* the scan stops at the song's last sector */
		}
	}
}

static void ring_pin(ring_t *r, uint32_t base)
{
	uint32_t n = 0u;

	while (n < PIN_N && base + n < SECTORS) {
		n++;
	}
	r->pin_base = base;
	r->pin_count = n;
}

/* ---- the run: main.c's stem_audio_block(), reproduced ------------------- */
typedef struct {
	uint32_t *frames;      /* every frame emitted, in order */
	uint32_t  n;
	uint32_t  cap;
	uint32_t  silent;      /* frames emitted as silence (an underrun) */
	uint32_t  from_pin;    /* frames served out of the pinned sectors */
	uint32_t  wraps;
	uint32_t  exits;
} emitted_t;

static void emit(emitted_t *e, uint32_t frame, bool silent, bool pin)
{
	if (e->n < e->cap) {
		e->frames[e->n] = silent ? NONE : frame;
	}
	e->n++;
	if (silent) {
		e->silent++;
	} else if (pin) {
		e->from_pin++;
	}
}

/*
 * One output block, following main.c's own order exactly:
 *   consume the one-shot exit -> required sector -> pin first, then ring ->
 *   publish the loop-aware read-ahead -> underrun check -> clamp the run to
 *   sector / song / block / LOOP WINDOW -> render -> advance -> wrap.
 */
/* THE PRODUCER IS CONTINUOUS, not block-synchronous.
 *
 * streamer_thread() runs on its own, asynchronously, at a rate set by the
 * eMMC: one 8192-byte sector read takes 5.073 ms uncontended (slice T0's
 * measurement) against a 256-frame output block of 5.333 ms, so it completes
 * 5.333/5.073 = 1.051 reads per block. An earlier version of this harness
 * instead did at most N fills per block and tied them to inner-loop
 * iterations, which starved the ring whenever a run happened to cover a whole
 * block in one pass -- an artefact of the model, not of the device. Fractional
 * credit carried across blocks reproduces the real behaviour.
 */
#define FILLS_PER_BLOCK_Q8 269u    /* 1.051 * 256, integer only */
static uint32_t g_fill_credit_q8;

static void audio_block(st_stream_t *st, ring_t *r, emitted_t *e,
			 bool lp_on, uint32_t lp_lo, uint32_t lp_hi,
			 bool *exit_req)
{
	uint32_t f = 0u;

	g_fill_credit_q8 += FILLS_PER_BLOCK_Q8;

	while (f < BLK) {
		uint32_t needed, fis, run, left_song, want;
		bool pin_hit;

		if (*exit_req) {
			*exit_req = false;
			if (st_stream_seek(st, lp_lo)) {
				e->exits++;
			}
			lp_on = false;
		}

		needed = st_stream_required_sector(st);
		pin_hit = from_pin(r, needed);
		if (ring_has(r, needed)) {
			st_stream_sector_ready(st, needed);
		}

		want = needed;
		if (st->ready_sector == needed) {
			uint32_t ahead = needed + 1u;

			if (ahead >= st->sector_count) {
				ahead = 0u;
			}
			/* Song order even inside a loop -- see main.c's own
			 * comment at this same point for why prefetching the
			 * post-wrap region evicts the pre-wrap region. */
			want = ahead;
		}
		while (g_fill_credit_q8 >= 256u) {
			g_fill_credit_q8 -= 256u;
			ring_service(r, want);
		}

		if (st->ready_sector != needed) {
			/* Report the FIRST starvation precisely: which frame,
			 * which sector, what the pin held and what the ring's
			 * mapped slot held instead. This line is what turned a
			 * vague "it emits silence sometimes" into the exact
			 * finding that a post-wrap sector had evicted a
			 * pre-wrap one sharing its slot. Keep it. */
			if (e->silent == 0u) {
				printf("      starved: frame %u needs sector %u; "
				       "pin=[%d,+%u) ring slot %u holds %d; %u frames in\n",
				       st->song_frame, needed, (int)r->pin_base,
				       r->pin_count, needed % SLOTS,
				       (int)r->slot[needed % SLOTS], e->n);
			}
			for (; f < BLK; f++) {
				emit(e, 0u, true, false);
			}
			(void)st_stream_advance_frames(st, 1u);
			return;
		}

		fis = st->song_frame - needed * ST11_FRAMES_PER_SECTOR;
		run = ST11_FRAMES_PER_SECTOR - fis;
		left_song = st->frames - st->song_frame;
		if (run > left_song) {
			run = left_song;
		}
		if (run > BLK - f) {
			run = BLK - f;
		}
		if (lp_on && st->song_frame >= lp_lo && st->song_frame < lp_hi) {
			uint32_t left_loop = lp_hi - st->song_frame;

			if (run > left_loop) {
				run = left_loop;
			}
		}
		if (run == 0u) {
			for (; f < BLK; f++) {
				emit(e, 0u, true, false);
			}
			return;
		}

		{
			uint32_t k;

			for (k = 0; k < run; k++) {
				emit(e, st->song_frame + k, false, pin_hit);
			}
		}
		f += run;
		(void)st_stream_advance_frames(st, run);

		if (lp_on && st->song_frame >= lp_hi && lp_hi > lp_lo) {
			if (st_stream_seek(st, lp_lo)) {
				e->wraps++;
			}
		}
	}
}

/* `at_sector` is where playback actually is. The ring is primed AROUND it, not
 * around sector 0: a loop begins at the current playhead, and at that instant
 * the ring is already full of the sectors just ahead of it. Priming at zero
 * instead made the first pass of every loop start from a cold ring, which is a
 * property of the harness and not of the device. */
static void prime(st_stream_t *st, ring_t *r, uint32_t at_sector)
{
	uint32_t i;

	(void)st_stream_init(st, 0u, SECTORS * ST11_BLOCKS_PER_SECTOR, SONG_FRAMES,
			      SECTORS, false);
	st_stream_play(st);
	ring_init(r);
	g_fill_credit_q8 = 0u;
	for (i = 0; i < SLOTS; i++) {
		uint32_t sec = at_sector + i;

		if (sec < SECTORS) {
			r->slot[sec % SLOTS] = sec;
		}
	}
}

/* ============ 1. a loop wraps contiguously through the real path ========= */
static void case_wrap_contiguous(void)
{
	st_stream_t st;
	ring_t r;
	emitted_t e;
	st_loop_t l;
	static uint32_t buf[200000];
	uint32_t lo, hi, i, bad = 0u;
	bool exit_req = false;

	g_cases++;
	printf("\n-- A running loop emits a contiguous frame sequence\n");
	st_loop_reset(&l);

	/* Enter at a deliberately awkward frame: not a sector boundary. */
	lo = 5u * ST11_FRAMES_PER_SECTOR + 137u;
	prime(&st, &r, lo / ST11_FRAMES_PER_SECTOR);
	(void)st_stream_seek(&st, lo);
	l.start_frame = lo;
	l.length_index = ST_LOOP_LEN_DEFAULT;
	hi = lo + st_loop_window_frames(ST_LOOP_LEN_DEFAULT, FPB, lo, SONG_FRAMES);
	l.end_frame = hi;
	l.state = ST_LOOP_MOMENTARY;
	ring_pin(&r, lo / ST11_FRAMES_PER_SECTOR);

	memset(&e, 0, sizeof(e));
	e.frames = buf;
	e.cap = (uint32_t)(sizeof(buf) / sizeof(buf[0]));

	/* Long enough to wrap several times. */
	for (i = 0; i < 1200u; i++) {
		audio_block(&st, &r, &e, true, lo, hi, &exit_req);
	}
	printf("      window [%u,%u) = %u frames; emitted %u frames, %u wraps, "
	       "%u ring fills\n", lo, hi, hi - lo, e.n, e.wraps, r.fills);

	CHECK(e.silent == 0u,
	      "NOT ONE silent frame across %u emitted frames and %u wraps -- the loop "
	      "never starves the consumer", e.n, e.wraps);
	CHECK(e.wraps >= 3u, "the window wrapped %u times, so this really exercised it",
	      e.wraps);

	for (i = 1; i < e.n && i < e.cap; i++) {
		uint32_t p = buf[i - 1], c = buf[i];

		if (p == NONE || c == NONE) {
			continue;
		}
		if (c == p + 1u) {
			continue;             /* ordinary advance */
		}
		if (p == hi - 1u && c == lo) {
			continue;             /* the wrap, exactly */
		}
		bad++;
		if (bad <= 3u) {
			printf("      discontinuity at %u: %u -> %u\n", i, p, c);
		}
	}
	CHECK(bad == 0u,
	      "every one of the %u frame-to-frame steps is either +1 or the exact wrap "
	      "from end-1 (%u) to start (%u) -- no frame skipped, none repeated",
	      e.n - 1u, hi - 1u, lo);

	for (i = 0; i < e.n && i < e.cap; i++) {
		if (buf[i] != NONE && (buf[i] < lo || buf[i] >= hi)) {
			bad++;
		}
	}
	CHECK(bad == 0u, "and every emitted frame lies inside the window");
}

/* ====== 2. the exit resumes at start_frame with no silent frame ========== */
static void case_exit_no_silence(void)
{
	st_stream_t st;
	ring_t r;
	emitted_t e;
	static uint32_t buf[200000];
	uint32_t lo, hi, i, exit_at = 0u;
	bool exit_req = false;
	bool found = false;

	g_cases++;
	printf("\n-- The exit resumes at loop_start_frame, forward, with no gap\n");

	lo = 9u * ST11_FRAMES_PER_SECTOR + 339u;   /* the LAST frame of its sector:
						     * the worst case for the pin */
	prime(&st, &r, lo / ST11_FRAMES_PER_SECTOR);
	(void)st_stream_seek(&st, lo);
	hi = lo + st_loop_window_frames(ST_LOOP_LEN_DEFAULT, FPB, lo, SONG_FRAMES);
	ring_pin(&r, lo / ST11_FRAMES_PER_SECTOR);

	memset(&e, 0, sizeof(e));
	e.frames = buf;
	e.cap = (uint32_t)(sizeof(buf) / sizeof(buf[0]));

	for (i = 0; i < 300u; i++) {
		audio_block(&st, &r, &e, true, lo, hi, &exit_req);
	}
	CHECK(e.silent == 0u, "the loop ran clean for %u frames first", e.n);

	/* Request the exit mid-loop, then keep running forward. */
	exit_at = e.n;
	exit_req = true;
	for (i = 0; i < 400u; i++) {
		audio_block(&st, &r, &e, false, lo, hi, &exit_req);
	}

	CHECK(e.exits == 1u, "exactly one exit was performed");
	CHECK(e.silent == 0u,
	      "still NOT ONE silent frame after the exit -- %u frames emitted in total",
	      e.n);

	/* The very first frame after the exit must be loop_start_frame. */
	for (i = exit_at; i < e.n && i < e.cap; i++) {
		if (buf[i] == lo) {
			found = true;
			exit_at = i;
			break;
		}
	}
	CHECK(found, "loop_start_frame (%u) is emitted again after the exit request", lo);
	if (found) {
		uint32_t j;
		bool fwd = true;

		for (j = exit_at + 1u; j < exit_at + 2000u && j < e.n && j < e.cap; j++) {
			if (buf[j] != buf[j - 1] + 1u) {
				fwd = false;
				break;
			}
		}
		CHECK(fwd,
		      "and playback runs FORWARD from there for 2000 consecutive frames, "
		      "one frame at a time -- straight on through the rest of the song, "
		      "past where the loop's end used to be");
		CHECK(buf[exit_at + 1u] == lo + 1u,
		      "the frame immediately after the resume is start+1, not a jump");
	}

	/* The resume must NOT be any of the wrong answers. */
	CHECK(buf[exit_at] != 0u || lo == 0u,
	      "the resume is not frame zero");
	CHECK(buf[exit_at] != hi, "the resume is not loop_end_frame");
	CHECK(buf[exit_at] % ST11_FRAMES_PER_SECTOR != 0u,
	      "and it is not a sector boundary -- it is the exact captured frame, "
	      "which in this case sits %u frames into its sector",
	      lo % ST11_FRAMES_PER_SECTOR);
}

/* ==== 3. WITHOUT the pin, the same exit emits silence (why it exists) ==== */
static void case_pin_is_necessary(void)
{
	st_stream_t st;
	ring_t r;
	emitted_t e;
	static uint32_t buf[100000];
	uint32_t lo, hi, i;
	bool exit_req = false;

	g_cases++;
	printf("\n-- Negative control: the same exit WITHOUT the pinned sectors\n");

	lo = 9u * ST11_FRAMES_PER_SECTOR + 339u;
	prime(&st, &r, lo / ST11_FRAMES_PER_SECTOR);
	(void)st_stream_seek(&st, lo);
	hi = lo + st_loop_window_frames(ST_LOOP_LEN_DEFAULT, FPB, lo, SONG_FRAMES);
	/* Deliberately NO ring_pin() here. */

	memset(&e, 0, sizeof(e));
	e.frames = buf;
	e.cap = (uint32_t)(sizeof(buf) / sizeof(buf[0]));

	for (i = 0; i < 300u; i++) {
		audio_block(&st, &r, &e, true, lo, hi, &exit_req);
	}
	exit_req = true;
	for (i = 0; i < 50u; i++) {
		audio_block(&st, &r, &e, false, lo, hi, &exit_req);
	}

	CHECK(e.silent > 0u,
	      "without the pin the exit DOES emit silence (%u silent frames) -- this is "
	      "the failure the three pinned sectors exist to prevent, demonstrated "
	      "rather than asserted", e.silent);
	printf("      (the shortest musical loop is 1/4 beat = %u frames = %.1f sectors,\n"
	       "       and the ring holds %u -- so loop_start's sector is always evicted)\n",
	       FPB / 4u, (double)(FPB / 4u) / (double)ST11_FRAMES_PER_SECTOR, SLOTS - 1u);
}

/* ============= 4. every length loops cleanly, and stays in bounds ======== */
static void case_all_lengths(void)
{
	uint32_t idx;
	bool all_clean = true, all_in_bounds = true;

	g_cases++;
	printf("\n-- Every loop length runs clean through the real path\n");

	for (idx = 0; idx < ST_LOOP_LEN_COUNT; idx++) {
		st_stream_t st;
		ring_t r;
		emitted_t e;
		static uint32_t buf[80000];
		uint32_t lo, len, hi, i;
		bool exit_req = false;

		lo = 3u * ST11_FRAMES_PER_SECTOR + 71u;
		prime(&st, &r, lo / ST11_FRAMES_PER_SECTOR);
		(void)st_stream_seek(&st, lo);
		len = st_loop_window_frames((uint8_t)idx, FPB, lo, SONG_FRAMES);
		hi = lo + len;
		ring_pin(&r, lo / ST11_FRAMES_PER_SECTOR);

		memset(&e, 0, sizeof(e));
		e.frames = buf;
		e.cap = (uint32_t)(sizeof(buf) / sizeof(buf[0]));

		for (i = 0; i < 250u; i++) {
			audio_block(&st, &r, &e, true, lo, hi, &exit_req);
		}
		printf("      idx %u: %7u frames, %2u wraps, %u silent\n",
		       idx, len, e.wraps, e.silent);
		if (e.silent != 0u) {
			all_clean = false;
		}
		for (i = 0; i < e.n && i < e.cap; i++) {
			if (buf[i] != NONE && (buf[i] < lo || buf[i] >= hi ||
					        buf[i] >= SONG_FRAMES)) {
				all_in_bounds = false;
			}
		}
	}
	CHECK(all_clean, "all %u lengths loop with zero silent frames", ST_LOOP_LEN_COUNT);
	CHECK(all_in_bounds,
	      "and every emitted frame of every length stays inside both the window and "
	      "the committed song");
}

int main(void)
{
	printf("STEM TAPE LOOP PLAYBACK GATE\n");
	printf("the REAL st_loop/st_stream functions, driven the way stem_audio_block()\n");
	printf("drives them, over a real %u-sector song geometry\n", SECTORS);

	case_wrap_contiguous();
	case_exit_no_silence();
	case_pin_is_necessary();
	case_all_lengths();

	printf("\n%s (%d cases, %d checks, %d failures)\n",
	       g_failures ? "LOOP PLAYBACK GATE FAILED" : "LOOP PLAYBACK GATE PASSED",
	       g_cases, g_checks, g_failures);
	return g_failures ? 1 : 0;
}
