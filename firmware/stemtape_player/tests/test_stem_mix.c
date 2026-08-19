/*
 * test_stem_mix.c — st_stem_mix.c: deterministic fixed-point 4-stem-to-
 * stereo mixdown, host-tested.
 *
 * Two kinds of input, deliberately kept separate:
 *   1. REAL decoded audio content, from handoff/v1.1/binaries/song-
 *      sectors-four-stem.bin -- the same real fixture test_stem_v11.c's
 *      own test_song_sectors_fixture() already proves decodes correctly
 *      via st11_sector_read_header()/st11_sector_decode_frame() -- fed
 *      through the mixer under unity gain / no mute / no solo, with the
 *      expected mixed sample computed independently in this file (via
 *      the SAME documented gain/shift/saturate formula st_stem_mix.h's
 *      own doc comment specifies, written independently of st_stem_mix.c
 *      itself) -- proving the implementation matches its own documented
 *      contract on real audio content, not merely "some plausible bytes
 *      came out".
 *   2. Hand-picked boundary int32_t sample values (never claimed to be
 *      real recorded audio) for solo/mute-interaction and saturation
 *      edge cases -- st11_audio_frame_t is a plain struct of already-
 *      decoded samples with no encoding step to bypass, so constructing
 *      one directly to hit a specific arithmetic boundary (the same way
 *      any numeric unit test picks INT16_MIN/MAX-adjacent inputs) is not
 *      "fabricating a fixture" in the sense this suite's non-fabrication
 *      rule forbids -- it is the standard, necessary way to test a pure
 *      function's overflow/clamp behavior, and is never used to invent
 *      an "expected" song-content result.
 *
 *     cc -std=c11 -Wall -Wextra -I../src \
 *        ../src/st_sector_v11.c ../src/st_stem_mix.c \
 *        test_stem_mix.c -o test_stem_mix && \
 *        (cd ../../.. && firmware/stemtape_player/tests/test_stem_mix)
 *
 * Must be run with the CURRENT WORKING DIRECTORY at the repository root.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "st_sector_v11.h"
#include "st_stem_mix.h"
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

/* Independently reproduces st_stem_mix.h's own documented formula (unity
 * gain, no mute/solo: sum all 4 stems, then >> (ST11_PCM_BIT_DEPTH - 16),
 * then saturate to int16_t) -- written separately from st_stem_mix.c so
 * this is a genuine cross-check, not a restatement of the implementation. */
static int16_t reference_unity_mix(const int32_t stem[ST11_STEM_COUNT])
{
	int64_t sum = 0;
	uint32_t s;

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		sum += (int64_t)stem[s];
	}
	sum >>= (ST11_PCM_BIT_DEPTH - 16u);
	if (sum > INT16_MAX) {
		return INT16_MAX;
	}
	if (sum < INT16_MIN) {
		return INT16_MIN;
	}
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

/*
 * handoff/v1.1/binaries/song-sectors-four-stem.bin's own sector 0, frame 0
 * -- the same real fixture and the same st11_sector_read_header()/
 * st11_sector_decode_frame() calls test_stem_v11.c's own
 * test_song_sectors_fixture() already proves decode correctly (matching
 * the companion's declared per-stem FNV-1a checksums exactly). Mixed here
 * under unity gain / no mute / no solo and compared against
 * reference_unity_mix()'s independent computation.
 */
static void test_mix_real_decoded_frame_unity_gain(void)
{
	size_t len;
	uint8_t *data = read_fixture("handoff/v1.1/binaries/song-sectors-four-stem.bin", &len);

	CHECK(len >= ST11_SECTOR_BYTES, "song-sectors-four-stem.bin has at least one full sector");

	st11_sector_header_t h;

	CHECK(st11_sector_read_header(data, &h), "sector 0 header reads back as valid ('STSC')");

	st11_audio_frame_t frame;

	st11_sector_decode_frame(data, 0u, &frame);

	st_stem_mix_channel_t channels[ST11_STEM_COUNT];

	unity_channels(channels);

	int16_t out_l, out_r;

	st_stem_mix_frame(&frame, channels, &out_l, &out_r);

	int16_t expect_l = reference_unity_mix(frame.stem_l);
	int16_t expect_r = reference_unity_mix(frame.stem_r);

	CHECK(out_l == expect_l,
	      "real sector 0/frame 0: mixed left sample (%d) matches the independently-computed reference (%d)",
	      out_l, expect_l);
	CHECK(out_r == expect_r,
	      "real sector 0/frame 0: mixed right sample (%d) matches the independently-computed reference (%d)",
	      out_r, expect_r);

	/* Sanity: this real frame is not degenerate silence (a mixer that
	 * always returned 0 would trivially "pass" the two checks above). */
	CHECK(expect_l != 0 || expect_r != 0,
	      "sanity: sector 0/frame 0's real decoded content does not mix down to exact silence");

	free(data);
}

/*
 * Phase 2 slice 2 integration test: replays the EXACT real production
 * call sequence main.c's own streamer_thread()/looper_audio_block() now
 * perform for stored-song playback --
 *   1. st11_sector_read_header() once, to get the sector's own real,
 *      authoritative frame_count (never assumed to be a full
 *      ST11_FRAMES_PER_SECTOR) -- exactly streamer_thread()'s own boot-
 *      time read.
 *   2. st11_sector_decode_frame() + st_stem_mix_frame(), once per frame,
 *      sequentially from frame 0 through frame_count-1 -- exactly
 *      looper_audio_block()'s own PASS C loop, unity gain / unmuted / no
 *      solo (this slice's own fixed channel state; per-stem fader/mute/
 *      solo is a later Phase 3 slice).
 *
 * main.c itself is not host-testable (requires the Zephyr kernel -- see
 * this whole suite's established, already-published "honest coverage
 * boundary" limitation for main.c-specific code), so this cannot invoke
 * main.c's own compiled functions directly. What it DOES prove, honestly
 * and without fabrication: given the REAL song-sectors-four-stem.bin
 * fixture's own real sector 0 bytes, replaying main.c's exact new call
 * sequence over EVERY one of that sector's real frames produces, for
 * every single frame, the same result an independently-written reference
 * formula computes -- i.e. the audio DATA TRANSFORMATION the new
 * playback path performs is correct across the whole sector, not just
 * spot-checked at frame 0.
 */
static void test_playback_path_replays_production_sequence_over_full_sector(void)
{
	size_t len;
	uint8_t *data = read_fixture("handoff/v1.1/binaries/song-sectors-four-stem.bin", &len);

	CHECK(len >= ST11_SECTOR_BYTES, "song-sectors-four-stem.bin has at least one full sector");

	st11_sector_header_t h;

	CHECK(st11_sector_read_header(data, &h), "sector 0 header reads back as valid ('STSC') -- matches "
						  "streamer_thread()'s own boot-time read exactly");
	CHECK(h.frame_count == ST11_FRAMES_PER_SECTOR,
	      "sector 0's own real frameCount == 340 (a full, non-final sector -- matches "
	      "test_stem_v11.c's own already-established citation for this exact fixture)");

	st_stem_mix_channel_t channels[ST11_STEM_COUNT];

	unity_channels(channels);

	uint32_t f;
	bool all_match = true;
	bool any_nonzero = false;

	for (f = 0; f < h.frame_count; f++) {
		st11_audio_frame_t frame;

		st11_sector_decode_frame(data, f, &frame);

		int16_t out_l, out_r;

		st_stem_mix_frame(&frame, channels, &out_l, &out_r);

		int16_t expect_l = reference_unity_mix(frame.stem_l);
		int16_t expect_r = reference_unity_mix(frame.stem_r);

		if (out_l != expect_l || out_r != expect_r) {
			all_match = false;
		}
		if (expect_l != 0 || expect_r != 0) {
			any_nonzero = true;
		}
	}

	CHECK(all_match,
	      "production sequence replay: every one of sector 0's 340 real frames mixes to exactly the "
	      "independently-computed reference sample (left AND right)");
	CHECK(any_nonzero, "sanity: sector 0's real decoded content is not degenerate silence across all 340 frames");

	free(data);
}

/* frame_l/frame_r for stem s hold the SAME synthetic per-stem constant, so
 * a channel's contribution (or lack of it) is unambiguous from the output
 * alone: stem 0 -> 1000, stem 1 -> 2000, stem 2 -> 3000, stem 3 -> 4000
 * (well inside the 24-bit domain, chosen only so each stem's presence/
 * absence is separately distinguishable in the summed result -- not
 * claimed to be real audio). */
static void build_synthetic_frame(st11_audio_frame_t *frame)
{
	uint32_t s;

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		int32_t v = (int32_t)((s + 1u) * 1000u);

		frame->stem_l[s] = v;
		frame->stem_r[s] = v;
	}
}

static void test_mix_mute_silences_one_stem(void)
{
	st11_audio_frame_t frame;

	build_synthetic_frame(&frame);

	st_stem_mix_channel_t channels[ST11_STEM_COUNT];

	unity_channels(channels);
	channels[2].mute = true; /* silence the 3000-valued stem */

	int32_t stem_without_2[ST11_STEM_COUNT] = { frame.stem_l[0], frame.stem_l[1], 0, frame.stem_l[3] };
	int16_t expect_l = reference_unity_mix(stem_without_2);

	int16_t out_l, out_r;

	st_stem_mix_frame(&frame, channels, &out_l, &out_r);

	CHECK(out_l == expect_l,
	      "mute: muting stem 2 gives the same result as if its contribution were exactly 0 (got %d, expected %d)",
	      out_l, expect_l);
}

static void test_mix_solo_isolates_one_stem(void)
{
	st11_audio_frame_t frame;

	build_synthetic_frame(&frame);

	st_stem_mix_channel_t channels[ST11_STEM_COUNT];

	unity_channels(channels);
	channels[1].solo = true; /* isolate the 2000-valued stem */

	int32_t only_stem_1[ST11_STEM_COUNT] = { 0, frame.stem_l[1], 0, 0 };
	int16_t expect_l = reference_unity_mix(only_stem_1);

	int16_t out_l, out_r;

	st_stem_mix_frame(&frame, channels, &out_l, &out_r);

	CHECK(out_l == expect_l,
	      "solo: soloing stem 1 mixes as if only stem 1 were present, all others silent (got %d, expected %d)",
	      out_l, expect_l);
}

static void test_mix_mute_wins_over_solo_on_same_stem(void)
{
	st11_audio_frame_t frame;

	build_synthetic_frame(&frame);

	st_stem_mix_channel_t channels[ST11_STEM_COUNT];

	unity_channels(channels);
	channels[0].solo = true;
	channels[0].mute = true; /* soloed AND muted -- mute must win */

	int16_t out_l, out_r;

	st_stem_mix_frame(&frame, channels, &out_l, &out_r);

	/* Every stem is silent: stem 0 is muted (so silent despite its own
	 * solo), and every OTHER stem is silenced by stem 0's solo. */
	CHECK(out_l == 0 && out_r == 0,
	      "mute-over-solo: a stem that is both soloed and muted contributes nothing, and its solo still "
	      "silences every OTHER stem -- total output is exact silence");
}

static void test_mix_no_channels_active_is_silence(void)
{
	st11_audio_frame_t frame;

	build_synthetic_frame(&frame);

	st_stem_mix_channel_t channels[ST11_STEM_COUNT];
	uint32_t s;

	unity_channels(channels);
	for (s = 0; s < ST11_STEM_COUNT; s++) {
		channels[s].mute = true;
	}

	int16_t out_l, out_r;

	st_stem_mix_frame(&frame, channels, &out_l, &out_r);

	CHECK(out_l == 0 && out_r == 0, "all 4 stems muted: output is exact silence");
}

/*
 * Boundary values only, never claimed as real audio content (see this
 * file's own doc comment): every stem at the maximum representable
 * signed-24-bit sample (2^23 - 1 = 8388607), summed at unity gain. Four
 * stems summed near full-scale legitimately exceeds the 16-bit output
 * range once shifted down, so this proves the saturation clamp actually
 * engages rather than silently wrapping.
 */
static void test_mix_saturates_at_positive_full_scale(void)
{
	st11_audio_frame_t frame;
	uint32_t s;

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		frame.stem_l[s] = 8388607;
		frame.stem_r[s] = 8388607;
	}

	st_stem_mix_channel_t channels[ST11_STEM_COUNT];

	unity_channels(channels);

	int16_t out_l, out_r;

	st_stem_mix_frame(&frame, channels, &out_l, &out_r);

	CHECK(out_l == INT16_MAX && out_r == INT16_MAX,
	      "4 stems at +full-scale (24-bit), unity gain: mixed output clamps to INT16_MAX exactly (got %d/%d)",
	      out_l, out_r);
}

/* Same shape, negative full scale (-2^23), plus a caller-supplied gain
 * well above unity, exercising BOTH overflow sources the header's own
 * doc comment calls out (several stems summed near full-scale, AND
 * above-unity gain). */
static void test_mix_saturates_at_negative_full_scale_with_high_gain(void)
{
	st11_audio_frame_t frame;
	uint32_t s;

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		frame.stem_l[s] = -8388608;
		frame.stem_r[s] = -8388608;
	}

	st_stem_mix_channel_t channels[ST11_STEM_COUNT];

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		channels[s].gain_q8 = ST_STEM_MIX_GAIN_UNITY_Q8 * 4; /* 4x boost */
		channels[s].mute = false;
		channels[s].solo = false;
	}

	int16_t out_l, out_r;

	st_stem_mix_frame(&frame, channels, &out_l, &out_r);

	CHECK(out_l == INT16_MIN && out_r == INT16_MIN,
	      "4 stems at -full-scale with 4x gain: mixed output clamps to INT16_MIN exactly, no wraparound "
	      "(got %d/%d)",
	      out_l, out_r);
}

int main(void)
{
	RUN(test_mix_real_decoded_frame_unity_gain);
	RUN(test_playback_path_replays_production_sequence_over_full_sector);
	RUN(test_mix_mute_silences_one_stem);
	RUN(test_mix_solo_isolates_one_stem);
	RUN(test_mix_mute_wins_over_solo_on_same_stem);
	RUN(test_mix_no_channels_active_is_silence);
	RUN(test_mix_saturates_at_positive_full_scale);
	RUN(test_mix_saturates_at_negative_full_scale_with_high_gain);

	printf("\n");
	printf("%d distinct test cases, %d assertion checks\n", g_test_cases, g_checks);
	if (g_failures) {
		printf("STEM MIX TEST FAILED (%d of %d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("STEM MIX TEST PASSED (%d test cases, %d checks)\n", g_test_cases, g_checks);
	return 0;
}
