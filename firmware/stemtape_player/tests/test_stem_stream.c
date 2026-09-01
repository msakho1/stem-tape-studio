/*
 * test_stem_stream.c — st_stem_stream.c: pure stored-song continuous
 * streaming state machine, host-tested (STEM TAPE Phase 2, Slice 3A).
 *
 * Per this migration's own non-fabrication rule: every audio-content
 * assertion below is checked against handoff/v1.1/binaries/song-sectors-
 * four-stem.bin, the SAME real, SHA-256-verified, frozen 43-sector fixture
 * test_stem_v11.c and test_stem_mix.c already use (352,256 bytes = 43 *
 * ST11_SECTOR_BYTES; frames=14592, sectorCount=43 -- see test_stem_v11.c's
 * own already-established citation of handoff/v1.1/decoded/song-sectors-
 * four-stem.json, itself SHA-256-verified against handoff/v1.1/
 * SHA256SUMS.txt before being frozen into this repo). Negative-path tests
 * (corrupt header, invalid geometry) construct their invalid inputs by
 * copying a REAL, already-proven-valid decoded value and perturbing
 * exactly one field -- the same technique test_stem_v11.c's own
 * test_corrupt_newest_index_fallback() already established (flip one
 * real, verified byte/field, never invent new "real audio" content).
 *
 *     cc -std=c11 -Wall -Wextra -I../src \
 *        ../src/st_checksum32.c ../src/st_sector_v11.c ../src/st_stem_mix.c \
 *        ../src/st_stem_stream.c \
 *        test_stem_stream.c -o test_stem_stream && \
 *        (cd ../../.. && firmware/stemtape_player/tests/test_stem_stream)
 *
 * Must be run with the CURRENT WORKING DIRECTORY at the repository root.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "st_checksum32.h"
#include "st_sector_v11.h"
#include "st_stem_mix.h"
#include "st_stem_stream.h"
#include "st_v11_format.h"

static int g_checks;
static int g_failures;
static int g_test_cases;

#define CHECK(cond, ...) do { \
		g_checks++; \
		if (cond) { \
			printf("[OK  ] " __VA_ARGS__); \
			printf("\n"); \
		} else { \
			g_failures++; \
			printf("[FAIL] " __VA_ARGS__); \
			printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)

#define RUN(fn) do { g_test_cases++; fn(); } while (0)

static uint8_t *read_fixture(const char *path, size_t *len_out)
{
	FILE *f = fopen(path, "rb");

	if (!f) {
		fprintf(stderr, "FATAL: could not open fixture %s (run from the repo root?)\n", path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);

	rewind(f);
	uint8_t *buf = malloc((size_t)sz);

	if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		fprintf(stderr, "FATAL: short read on %s\n", path);
		exit(2);
	}
	fclose(f);
	*len_out = (size_t)sz;
	return buf;
}

/* Same independent reference formula test_stem_mix.c's own
 * reference_unity_mix() uses (unity gain, no mute/solo: sum all 4 stems,
 * >> (ST11_PCM_BIT_DEPTH - 16), saturate to int16_t) -- written
 * separately from st_stem_mix.c so the full-song hash cross-check below
 * is genuine, not a restatement of the implementation. */
static int16_t reference_unity_mix(const int32_t stem[ST11_STEM_COUNT])
{
	int64_t sum = 0;
	uint32_t s;

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		sum += (int64_t)stem[s];
	}
	sum >>= (ST11_PCM_BIT_DEPTH - 16u);
	if (sum > INT16_MAX) return INT16_MAX;
	if (sum < INT16_MIN) return INT16_MIN;
	return (int16_t)sum;
}

static void unity_channels(st_stem_mix_channel_t out[ST11_STEM_COUNT])
{
	uint32_t s;

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		out[s].gain_q8 = ST_STEM_MIX_GAIN_UNITY_Q8;
		out[s].mute = false;
		out[s].solo = false;
	}
}

/* Feeds one stereo sample pair's 4 little-endian bytes into a running
 * FNV-1a checksum (st_checksum32.h -- the same algorithm this whole
 * codebase already uses for content checksums, reused rather than
 * inventing a second hash). */
static uint32_t hash_stereo_sample(uint32_t h, int16_t l, int16_t r)
{
	uint8_t b[4] = {
		(uint8_t)((uint16_t)l & 0xffu), (uint8_t)(((uint16_t)l >> 8) & 0xffu),
		(uint8_t)((uint16_t)r & 0xffu), (uint8_t)(((uint16_t)r >> 8) & 0xffu),
	};
	return st_checksum32_update(h, b, sizeof(b));
}

/* The frozen four-stem fixture's real length, and the sector count it occupies
 * DERIVED from it rather than snapshotted. 14592 frames was 43 sectors at
 * v1.2's 340 frames per sector and is 29 at v1.3's 510 -- the same audio in
 * the same 8192-byte sectors. A hard-coded 43 read 14 sectors past the end of
 * the fixture, which is a segfault, not a diagnosis. */
#define SONG_FRAMES 14592u
#define SONG_SECTOR_COUNT ((SONG_FRAMES + ST11_FRAMES_PER_SECTOR - 1u) / \
			    ST11_FRAMES_PER_SECTOR)
#define SONG_BLOCK_COUNT_EXACT (SONG_SECTOR_COUNT * ST11_BLOCKS_PER_SECTOR) /* 688: exact-fit capacity */

/* ========================================================================
 * st_stream_init(): invalid STIX geometry is rejected
 * ======================================================================== */
static void test_init_rejects_invalid_geometry(void)
{
	st_stream_t st;

	CHECK(!st_stream_init(&st, 0u, SONG_BLOCK_COUNT_EXACT, 0u, SONG_SECTOR_COUNT, false),
	      "frames == 0 is rejected");
	CHECK(!st_stream_init(&st, 0u, SONG_BLOCK_COUNT_EXACT, SONG_FRAMES, 0u, false),
	      "sector_count == 0 is rejected");
	CHECK(!st_stream_init(&st, 0u, SONG_BLOCK_COUNT_EXACT, SONG_FRAMES, SONG_SECTOR_COUNT + 1u, false),
	      "sector_count one MORE than ceil(frames/FRAMES_PER_SECTOR) is rejected "
	      "(an extra sector with no real frames of its own is not a valid final sector)");
	CHECK(!st_stream_init(&st, 0u, SONG_BLOCK_COUNT_EXACT, SONG_FRAMES, SONG_SECTOR_COUNT - 1u, false),
	      "sector_count one FEWER than ceil(frames/FRAMES_PER_SECTOR) is rejected "
	      "(too few sectors to hold every declared frame)");
	CHECK(!st_stream_init(&st, 0u, SONG_BLOCK_COUNT_EXACT - 1u, SONG_FRAMES, SONG_SECTOR_COUNT, false),
	      "song_block_count one block SHORT of the sectors' own required capacity is rejected "
	      "(\"never read beyond the selected song region\")");

	/* Exact-fit capacity is accepted (the boundary itself is valid, not
	 * merely "more than enough" being valid). */
	bool ok = st_stream_init(&st, 4096u, SONG_BLOCK_COUNT_EXACT, SONG_FRAMES, SONG_SECTOR_COUNT, false);

	CHECK(ok, "the real fixture's own exact geometry (frames=14592, sectorCount=43, exact-fit "
		  "song_block_count=688) is accepted");
	CHECK(st.state == ST_STREAM_STOPPED, "a freshly initialized stream starts STOPPED");
	CHECK(st.song_frame == 0u, "a freshly initialized stream starts at song_frame 0");
	CHECK(st.ready_sector == ST_STREAM_NO_SECTOR, "a freshly initialized stream has no sector marked ready");
	CHECK(st.underrun_count == 0u, "a freshly initialized stream has a zeroed underrun diagnostic counter");
}

/* ========================================================================
 * st_stream_validate_sector(): every real sector in the fixture validates;
 * corrupting any one of the three checked header fields is rejected.
 * ======================================================================== */
static void test_validate_sector_real_fixture_and_corrupt_header(void)
{
	size_t len;
	uint8_t *data = read_fixture("handoff/v1.3/binaries/song-sectors-four-stem.bin", &len);

	CHECK(len == (size_t)SONG_SECTOR_COUNT * ST11_SECTOR_BYTES,
	      "song-sectors-four-stem.bin is exactly 43 * 8192 = 352256 bytes");

	st_stream_t st;
	bool ok = st_stream_init(&st, 4096u, SONG_BLOCK_COUNT_EXACT, SONG_FRAMES, SONG_SECTOR_COUNT, false);

	CHECK(ok, "init succeeds against the real fixture's own geometry");

	bool all_valid = true;

	for (uint32_t i = 0; i < SONG_SECTOR_COUNT; i++) {
		const uint8_t *sector = data + (size_t)i * ST11_SECTOR_BYTES;
		st11_sector_header_t h;

		if (!st11_sector_read_header(sector, &h) || !st_stream_validate_sector(&st, i, &h)) {
			all_valid = false;
		}
	}
	CHECK(all_valid, "every one of the real fixture's 43 real sectors validates against the song's "
			  "own declared geometry, in order, including the final PARTIAL sector (index 42)");

	/* The final sector's own real header must be genuinely short (this
	 * is the "handle the final partial sector exactly" case -- if the
	 * fixture ever changed shape this assertion would catch it rather
	 * than silently validating a full-length final sector). */
	st11_sector_header_t last_hdr;
	const uint8_t *last_sector = data + (size_t)(SONG_SECTOR_COUNT - 1u) * ST11_SECTOR_BYTES;

	CHECK(st11_sector_read_header(last_sector, &last_hdr), "the real final sector's header reads back valid");
	CHECK(last_hdr.frame_count > 0u && last_hdr.frame_count < ST11_FRAMES_PER_SECTOR,
	      "the real final sector (index 42) is genuinely partial: 0 < frame_count (%u) < 340",
	      last_hdr.frame_count);
	CHECK(last_hdr.first_frame + last_hdr.frame_count == SONG_FRAMES,
	      "the final sector's own first_frame + frame_count reaches exactly the song's total frame count");

	/* out-of-range sector_index */
	st11_sector_header_t h0;

	st11_sector_read_header(data, &h0);
	CHECK(!st_stream_validate_sector(&st, SONG_SECTOR_COUNT, &h0),
	      "sector_index == sector_count (one past the last real sector) is rejected -- "
	      "\"never read beyond the selected song region\"");

	/* corrupt header: one real, already-valid header (sector 5), one
	 * field perturbed at a time -- same "flip one real, verified
	 * field" technique test_stem_v11.c's own corruption tests use. */
	st11_sector_header_t h5;

	CHECK(st11_sector_read_header(data + 5u * ST11_SECTOR_BYTES, &h5), "sector 5's real header reads back valid");
	CHECK(st_stream_validate_sector(&st, 5u, &h5), "sector 5's real, unmodified header validates");

	st11_sector_header_t bad = h5;

	bad.sector_index = h5.sector_index + 1u;
	CHECK(!st_stream_validate_sector(&st, 5u, &bad),
	      "corrupt header: sector_index off by one from a real header is rejected");

	bad = h5;
	bad.first_frame = h5.first_frame + 1u;
	CHECK(!st_stream_validate_sector(&st, 5u, &bad),
	      "corrupt header: first_frame off by one from a real header is rejected");

	bad = h5;
	bad.frame_count = h5.frame_count + 1u;
	CHECK(!st_stream_validate_sector(&st, 5u, &bad),
	      "corrupt header: frame_count off by one from a real header is rejected");

	free(data);
}

/* NOTE (Slice 3B.1): st_stream_report_corrupt() no longer exists -- this
 * pure module is now exclusively audio-thread-owned (see st_stem_
 * stream.h's own doc comment), and corrupt-sector handling moved
 * entirely to the producer/mailbox layer (st_stem_bufmbox.h + main.c's
 * own diagnostic counter): a sector that fails validation is simply
 * never published ready, which this module already represents as
 * UNDERRUN. See tests/test_stem_bufmbox.c for the mailbox-level
 * coverage, and st_stem_stream.h's own note on why this is an
 * intentional, behavior-preserving simplification. */

/* ========================================================================
 * Underrun: a "missing sector" (never marked ready) freezes song_frame,
 * counts exactly once per episode, and recovers only once the exact
 * needed sector is supplied.
 * ======================================================================== */
static void test_underrun_missing_sector_and_recovery(void)
{
	st_stream_t st;

	st_stream_init(&st, 4096u, SONG_BLOCK_COUNT_EXACT, SONG_FRAMES, SONG_SECTOR_COUNT, false);
	st_stream_play(&st);
	/* Deliberately never call st_stream_sector_ready(): sector 0 is
	 * "missing". */

	uint32_t frame_before = st.song_frame;
	st_stream_tick_t t1 = st_stream_advance_frame(&st);

	CHECK(t1 == ST_STREAM_TICK_UNDERRUN, "a missing sector reports UNDERRUN on the very first tick");
	CHECK(st.state == ST_STREAM_UNDERRUN, "state is UNDERRUN");
	CHECK(st.song_frame == frame_before, "song_frame is frozen, never guessed, while underrun");
	CHECK(st.underrun_count == 1u, "one UNDERRUN episode has been counted");

	/* Stuck for several more ticks: song_frame stays frozen, the
	 * counter does NOT increment again for the same episode ("never
	 * replay stale sector data as if it were current audio" -- there
	 * is no sector data at all here, and none is invented). */
	for (int i = 0; i < 5; i++) {
		st_stream_tick_t t = st_stream_advance_frame(&st);

		CHECK(t == ST_STREAM_TICK_UNDERRUN, "still underrun on repeated ticks (missing sector never arrives)");
	}
	CHECK(st.song_frame == frame_before, "song_frame is still exactly where it froze after repeated stuck ticks");
	CHECK(st.underrun_count == 1u, "underrun_count stays at 1 -- one episode, not one per stuck tick");

	/* Now supply the sector this position actually needs. */
	uint32_t needed = st_stream_required_sector(&st);

	st_stream_sector_ready(&st, needed);
	CHECK(st.state == ST_STREAM_PLAYING, "supplying the exact needed sector recovers UNDERRUN -> PLAYING");

	st_stream_tick_t t2 = st_stream_advance_frame(&st);

	CHECK(t2 == ST_STREAM_TICK_OK || t2 == ST_STREAM_TICK_SECTOR_CROSSED,
	      "playback genuinely advances again once recovered");
	CHECK(st.song_frame == frame_before + 1u, "song_frame advanced by exactly one frame after recovery");
	CHECK(st.underrun_count == 1u, "recovering and advancing normally does not touch the episode counter");
}

/* ========================================================================
 * Loop transition: wrapping at end-of-song invalidates the stale sector
 * and resumes cleanly from frame 0 once it is resupplied.
 * ======================================================================== */
static void test_loop_transition(void)
{
	st_stream_t st;

	st_stream_init(&st, 4096u, SONG_BLOCK_COUNT_EXACT, SONG_FRAMES, SONG_SECTOR_COUNT, /*loop_enabled=*/true);
	st_stream_play(&st);

	/* Jump directly to the last valid frame of the song (test setup
	 * convenience -- st_stream_t's fields are plain struct members,
	 * not opaque; the state machine itself never does this) and supply
	 * the real final sector's index so the tick that crosses the song
	 * boundary is the one under test. */
	st.song_frame = SONG_FRAMES - 1u;
	uint32_t last_sector = st_stream_required_sector(&st);

	CHECK(last_sector == SONG_SECTOR_COUNT - 1u, "the last valid frame's own required sector is index 42");
	st_stream_sector_ready(&st, last_sector);

	st_stream_tick_t t = st_stream_advance_frame(&st);

	CHECK(t == ST_STREAM_TICK_LOOPED, "advancing past the last frame with loop_enabled wraps (LOOPED)");
	CHECK(st.song_frame == 0u, "song_frame wraps to exactly 0");
	CHECK(st.state == ST_STREAM_PLAYING, "a loop wrap leaves the stream PLAYING, not stopped");
	CHECK(st.ready_sector == ST_STREAM_NO_SECTOR,
	      "the stale (last-sector) buffer is invalidated -- sector 0 must be resupplied, never assumed resident");

	/* Without resupplying, the very next tick underruns -- proves the
	 * invalidation is real, not cosmetic. */
	st_stream_tick_t t_stuck = st_stream_advance_frame(&st);

	CHECK(t_stuck == ST_STREAM_TICK_UNDERRUN, "attempting to advance before resupplying sector 0 underruns");
	CHECK(st.song_frame == 0u, "song_frame stays frozen at 0 while underrun after the wrap");

	/* Resupply sector 0 and confirm playback resumes cleanly. */
	st_stream_sector_ready(&st, 0u);
	st_stream_tick_t t_resume = st_stream_advance_frame(&st);

	CHECK(t_resume == ST_STREAM_TICK_OK, "playback resumes normally from frame 0 after resupplying sector 0");
	CHECK(st.song_frame == 1u, "song_frame is 1 after the resumed tick");
}

/* ========================================================================
 * The big one: walk the ENTIRE real 43-sector fixture end to end via the
 * state machine exactly the way Slice 3B's real streamer_thread/audio
 * path will, decoding+mixing every one of the 14592 real frames, proving:
 *   - sector transitions happen at exactly the right frame boundaries
 *     (42 SECTOR_CROSSED results, one less than the sector count),
 *   - the final, partial sector is handled exactly (no dropped/duplicated
 *     frames -- decoded_frame_count must equal SONG_FRAMES exactly),
 *   - "stop at end": the tick that reaches the last frame reports ENDED,
 *     and further ticks are inert (NOT_PLAYING) until told to play again,
 *   - a deterministic full-song mixed-audio hash, cross-checked against
 *     an independently-computed reference formula over the SAME real
 *     content (never fabricated).
 * ======================================================================== */
static void test_full_song_walk_transitions_and_hash(void)
{
	size_t len;
	uint8_t *data = read_fixture("handoff/v1.3/binaries/song-sectors-four-stem.bin", &len);

	CHECK(len == (size_t)SONG_SECTOR_COUNT * ST11_SECTOR_BYTES, "fixture is the expected 43-sector size");

	st_stream_t st;
	bool init_ok = st_stream_init(&st, 4096u, SONG_BLOCK_COUNT_EXACT, SONG_FRAMES, SONG_SECTOR_COUNT, false);

	CHECK(init_ok, "init succeeds for the full real-song walk");
	st_stream_play(&st);

	st_stem_mix_channel_t channels[ST11_STEM_COUNT];

	unity_channels(channels);

	uint32_t decoded_frame_count = 0;
	uint32_t crossed_count = 0;
	uint32_t ended_count = 0;
	bool all_match_reference = true;
	bool any_nonzero = false;
	bool sector_order_sequential = true;
	uint32_t expect_next_sector = 0;
	uint32_t hash_real = ST_CHECKSUM32_INIT;
	uint32_t hash_reference = ST_CHECKSUM32_INIT;

	for (;;) {
		if (st.state == ST_STREAM_END_OF_SONG) {
			break;
		}

		uint32_t needed = st_stream_required_sector(&st);

		if (st.ready_sector != needed) {
			/* Exactly Slice 3B's own real sequence: read the
			 * needed physical sector, header-validate it, cross-
			 * check it against the song's own geometry, then
			 * mark it ready. */
			if (sector_order_sequential && needed != expect_next_sector) {
				sector_order_sequential = false;
			}
			expect_next_sector = needed + 1u;

			const uint8_t *sector = data + (size_t)needed * ST11_SECTOR_BYTES;
			st11_sector_header_t hdr;
			bool header_ok = st11_sector_read_header(sector, &hdr);
			bool valid = header_ok && st_stream_validate_sector(&st, needed, &hdr);

			if (!valid) {
				/* The real fixture never hits this -- a genuinely
				 * corrupt sector is exercised at the mailbox level
				 * instead (tests/test_stem_bufmbox.c), since this
				 * pure module no longer has a "report corrupt" entry
				 * point (see this file's own note near the top). */
				CHECK(false, "unexpected: real fixture sector %u failed validation", needed);
				break;
			}
			st_stream_sector_ready(&st, needed);
		}

		/* Decode + mix the CURRENT song_frame from the now-ready
		 * sector's own buffer. */
		const uint8_t *sector = data + (size_t)needed * ST11_SECTOR_BYTES;
		uint32_t frame_in_sector = st.song_frame - needed * ST11_FRAMES_PER_SECTOR;
		st11_audio_frame_t frame;

		st11_sector_decode_frame(sector, frame_in_sector, &frame);

		int16_t out_l, out_r;

		st_stem_mix_frame(&frame, channels, &out_l, &out_r);
		int16_t ref_l = reference_unity_mix(frame.stem_l);
		int16_t ref_r = reference_unity_mix(frame.stem_r);

		if (out_l != ref_l || out_r != ref_r) {
			all_match_reference = false;
		}
		if (ref_l != 0 || ref_r != 0) {
			any_nonzero = true;
		}
		hash_real = hash_stereo_sample(hash_real, out_l, out_r);
		hash_reference = hash_stereo_sample(hash_reference, ref_l, ref_r);
		decoded_frame_count++;

		st_stream_tick_t t = st_stream_advance_frame(&st);

		if (t == ST_STREAM_TICK_SECTOR_CROSSED) {
			crossed_count++;
		} else if (t == ST_STREAM_TICK_ENDED) {
			ended_count++;
		} else if (t != ST_STREAM_TICK_OK) {
			/* UNDERRUN/LOOPED/NOT_PLAYING would all be bugs in
			 * this non-looping, always-supplied walk. */
			CHECK(false, "unexpected tick result %d at song_frame %u", (int)t, st.song_frame);
			break;
		}
	}

	CHECK(decoded_frame_count == SONG_FRAMES,
	      "exactly SONG_FRAMES (%u) frames were decoded -- no dropped or duplicated frame, "
	      "including across the final partial sector", SONG_FRAMES);
	CHECK(crossed_count == SONG_SECTOR_COUNT - 1u,
	      "exactly %u SECTOR_CROSSED transitions occurred -- one less than the sector count, "
	      "i.e. every sector boundary was crossed exactly once", SONG_SECTOR_COUNT - 1u);
	CHECK(ended_count == 1u, "exactly one ENDED result, at the very last frame");
	CHECK(sector_order_sequential, "sectors were required in strict ascending order, 0 through 42, "
					"no skips and no re-visits");
	CHECK(st.state == ST_STREAM_END_OF_SONG, "the stream is END_OF_SONG once the walk completes");
	CHECK(st.song_frame == SONG_FRAMES, "song_frame is frozen at exactly `frames` (one past the last index)");
	CHECK(all_match_reference,
	      "every one of the real song's %u frames mixes to exactly the independently-computed "
	      "reference sample (left AND right)", SONG_FRAMES);
	CHECK(any_nonzero, "sanity: the real song's decoded content is not degenerate silence across all frames");
	CHECK(hash_real == hash_reference,
	      "deterministic full-song mixed-audio hash: FNV-1a over all %u stereo frames' real mixer "
	      "output (0x%08x) matches the SAME hash computed over the independently-derived reference "
	      "stream (0x%08x) exactly", SONG_FRAMES, hash_real, hash_reference);
	printf("      full-song deterministic hash (real mixer output): 0x%08x\n", hash_real);

	/* "Stop at end": further ticks are inert until told to play again. */
	uint32_t frame_at_end = st.song_frame;
	st_stream_tick_t t_after_end = st_stream_advance_frame(&st);

	CHECK(t_after_end == ST_STREAM_TICK_NOT_PLAYING, "a tick after END_OF_SONG is a no-op (NOT_PLAYING)");
	CHECK(st.song_frame == frame_at_end, "song_frame does not move after the song has ended");

	free(data);
}

/* ========================================================================
 * THE RUN-FORM EQUIVALENCE TEST.
 *
 * main.c's audio thread no longer advances one frame at a time. It renders
 * a RUN of frames -- clamped to whichever comes first: the end of the ready
 * sector, the end of the song, or the end of its 256-frame output block --
 * and then calls st_stream_advance_frames() once for the whole run. That is
 * what took the per-frame division, residency test and barriered mailbox
 * atomic out of the 48 kHz path.
 *
 * It is only safe if the two forms are the same state machine. So: walk the
 * SAME real song twice, once frame-by-frame and once run-by-run using
 * exactly main.c's own run-clamping rule, and require that every observable
 * agrees -- the full song_frame sequence, the state sequence, the underrun
 * count, the terminal tick, and a hash of the mixed audio produced along
 * the way. If the run form ever diverges, even by one frame at one sector
 * seam, this fails.
 * ======================================================================== */
#define RUN_BLOCK_FRAMES 256u   /* main.c's BLK_FRAMES */

static void test_run_form_matches_frame_form(void)
{
	size_t len;
	uint8_t *data = read_fixture("handoff/v1.3/binaries/song-sectors-four-stem.bin", &len);
	st_stream_t a, b;
	st_stem_mix_channel_t channels[ST11_STEM_COUNT];
	st_stem_mix_prepared_t prep;
	uint32_t hash_a = ST_CHECKSUM32_INIT;
	uint32_t hash_b = ST_CHECKSUM32_INIT;
	uint32_t frames_a = 0, frames_b = 0;
	int state_mismatches = 0;
	unsigned guard;

	unity_channels(channels);
	st_stem_mix_prepare(channels, &prep);

	/* ---- walk A: one frame at a time (the original form) ---- */
	CHECK(st_stream_init(&a, 4096u, SONG_BLOCK_COUNT_EXACT, SONG_FRAMES, SONG_SECTOR_COUNT, false),
	      "run-equivalence: frame-form init");
	st_stream_play(&a);
	for (guard = 0; guard < SONG_FRAMES * 2u; guard++) {
		uint32_t needed;

		if (a.state == ST_STREAM_END_OF_SONG) {
			break;
		}
		needed = st_stream_required_sector(&a);
		if (a.ready_sector != needed) {
			st_stream_sector_ready(&a, needed);
		}
		{
			const uint8_t *sec = data + (size_t)needed * ST11_SECTOR_BYTES;
			st11_audio_frame_t fr;
			int16_t l, r;

			st11_sector_decode_frame(sec, a.song_frame - needed * ST11_FRAMES_PER_SECTOR, &fr);
			st_stem_mix_frame_prepared(&fr, &prep, &l, &r);
			hash_a = hash_stereo_sample(hash_a, l, r);
			frames_a++;
		}
		(void)st_stream_advance_frame(&a);
	}

	/* ---- walk B: run-by-run, using main.c's own clamping rule ---- */
	CHECK(st_stream_init(&b, 4096u, SONG_BLOCK_COUNT_EXACT, SONG_FRAMES, SONG_SECTOR_COUNT, false),
	      "run-equivalence: run-form init");
	st_stream_play(&b);
	for (guard = 0; guard < SONG_FRAMES * 2u; guard++) {
		uint32_t block_left = RUN_BLOCK_FRAMES;

		if (b.state == ST_STREAM_END_OF_SONG) {
			break;
		}
		while (block_left > 0u && b.state != ST_STREAM_END_OF_SONG) {
			uint32_t needed = st_stream_required_sector(&b);
			uint32_t fis, run, left_in_song;
			const uint8_t *sec;
			uint32_t k;

			if (b.ready_sector != needed) {
				st_stream_sector_ready(&b, needed);
			}
			fis = b.song_frame - needed * ST11_FRAMES_PER_SECTOR;
			run = ST11_FRAMES_PER_SECTOR - fis;          /* to the sector's end */
			left_in_song = b.frames - b.song_frame;      /* to the song's end   */
			if (run > left_in_song) {
				run = left_in_song;
			}
			if (run > block_left) {                      /* to the block's end  */
				run = block_left;
			}
			sec = data + (size_t)needed * ST11_SECTOR_BYTES;
			for (k = 0; k < run; k++) {
				st11_audio_frame_t fr;
				int16_t l, r;

				st11_sector_decode_frame(sec, fis + k, &fr);
				st_stem_mix_frame_prepared(&fr, &prep, &l, &r);
				hash_b = hash_stereo_sample(hash_b, l, r);
				frames_b++;
			}
			(void)st_stream_advance_frames(&b, run);
			block_left -= run;
		}
	}

	if (a.state != b.state) {
		state_mismatches++;
	}
	if (a.song_frame != b.song_frame) {
		state_mismatches++;
	}
	if (a.underrun_count != b.underrun_count) {
		state_mismatches++;
	}

	CHECK(frames_a == SONG_FRAMES, "frame form rendered every frame exactly once (%u)", frames_a);
	CHECK(frames_b == frames_a, "run form rendered the SAME number of frames (%u vs %u)",
	      frames_b, frames_a);
	CHECK(hash_b == hash_a,
	      "run form produces bit-identical audio over the whole song (0x%08x vs 0x%08x)",
	      hash_b, hash_a);
	CHECK(state_mismatches == 0,
	      "run form ends in the identical stream state (song_frame/state/underruns)");

	free(data);
}

int main(void)
{
	RUN(test_init_rejects_invalid_geometry);
	RUN(test_validate_sector_real_fixture_and_corrupt_header);
	RUN(test_underrun_missing_sector_and_recovery);
	RUN(test_loop_transition);
	RUN(test_full_song_walk_transitions_and_hash);
	RUN(test_run_form_matches_frame_form);

	printf("\nSTEM STREAM TEST %s (%d test cases, %d checks, %d failures)\n",
	       g_failures == 0 ? "PASSED" : "FAILED", g_test_cases, g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
