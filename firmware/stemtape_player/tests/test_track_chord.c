/*
 * test_track_chord.c — the Track-chord ADC decoder, host-tested.
 *
 * Drives the REAL st_track_chord_update()/st_track_chord_lookup() and the
 * REAL st_chord_bands[] table -- the same object file linked into the
 * firmware, the same function main.c's control loop calls. Nothing here
 * transcribes the table; every expectation is derived FROM it, so a band edit
 * moves the tests with it instead of leaving them asserting the old numbers.
 *
 * WHAT THIS PROVES: given a stream of raw ladder readings, the decoder settles
 * on the right mask, holds through guard zones and noise, and never names a
 * mask the player did not form.
 *
 * WHAT IT DOES NOT PROVE: that a real SP-1's ladder puts those chords at those
 * voltages. Only T1+T4 is a measured chord band (see st_track_chord.h); the
 * rest are model-derived, which is exactly why the bands are narrow and an
 * unrecognised reading holds rather than guesses.
 *
 *   cc -std=c11 -Wall -Wextra -I../src ../src/st_track_chord.c \
 *      test_track_chord.c -o test_track_chord && ./test_track_chord
 */

#include <stdio.h>
#include <string.h>

#include "st_track_chord.h"

static int g_checks, g_failures, g_cases;

#define CHECK(cond, ...) do { \
		g_checks++; \
		if (cond) { printf("[OK  ] " __VA_ARGS__); printf("\n"); } \
		else { g_failures++; printf("[FAIL] " __VA_ARGS__); \
		       printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); } \
	} while (0)

/* Rotating buffers: two mstr() calls in one printf must not alias, or a
 * message says "expected X, got X" while the values actually differ. */
static const char *mstr(uint8_t m)
{
	static char bufs[4][8];
	static unsigned turn;
	char *b = bufs[turn++ & 3u];

	b[0] = (m & ST_CHORD_T1) ? '1' : '0';
	b[1] = (m & ST_CHORD_T2) ? '1' : '0';
	b[2] = (m & ST_CHORD_T3) ? '1' : '0';
	b[3] = (m & ST_CHORD_T4) ? '1' : '0';
	b[4] = 0;
	return b;
}

static int band_centre(uint32_t i)
{
	return ((int)st_chord_bands[i].lo + (int)st_chord_bands[i].hi) / 2;
}

/* Hold a value steady long enough for the settle count to elapse. */
static uint8_t settle_at(st_track_chord_t *c, int raw, int reads)
{
	uint8_t m = c->settled;
	int i;

	for (i = 0; i < reads; i++) {
		m = st_track_chord_update(c, raw);
	}
	return m;
}

/* ===================== 1. every band decodes to itself ==================== */
static void case_every_band(void)
{
	st_track_chord_t c;
	uint32_t i;

	g_cases++;
	printf("\n-- Every band in the production table settles on its own mask\n");
	for (i = 0; i < st_chord_band_count; i++) {
		uint8_t want = st_chord_bands[i].mask;
		uint8_t got;

		st_track_chord_reset(&c);
		got = settle_at(&c, band_centre(i), 6);
		printf("      raw %4d -> %s\n", band_centre(i), mstr(got));
		CHECK(got == want, "band %u centre %d settles on %s", i, band_centre(i),
		      mstr(want));
	}
}

/* ============ 2. the table itself is ordered and non-overlapping ========== */
static void case_table_is_sane(void)
{
	uint32_t i;
	bool ordered = true, disjoint = true, below_play = true;
	uint8_t seen[16];

	g_cases++;
	printf("\n-- The band table is ordered, disjoint and clear of PLAY\n");
	memset(seen, 0, sizeof(seen));
	for (i = 0; i < st_chord_band_count; i++) {
		if (st_chord_bands[i].lo > st_chord_bands[i].hi) {
			ordered = false;
		}
		if (i > 0 && st_chord_bands[i].lo <= st_chord_bands[i - 1].hi) {
			disjoint = false;
		}
		/* Hysteresis widens a settled band by ST_CHORD_HYSTERESIS on each
		 * side; adjacent bands must stay disjoint even then, or a held
		 * chord could swallow its neighbour. */
		if (i > 0 &&
		    (int)st_chord_bands[i].lo - ST_CHORD_HYSTERESIS <=
		    (int)st_chord_bands[i - 1].hi + ST_CHORD_HYSTERESIS) {
			disjoint = false;
		}
		if (st_chord_bands[i].hi >= ST_CHORD_PLAY_FLOOR) {
			below_play = false;
		}
		seen[st_chord_bands[i].mask & 0xFu]++;
	}
	CHECK(ordered, "every band has lo <= hi");
	CHECK(disjoint, "no two bands overlap, even after both are widened by the "
			 "%d-count hysteresis", ST_CHORD_HYSTERESIS);
	CHECK(below_play, "every band ends below PLAY's floor (%d) -- a chord can "
			   "never be read as the transport control", ST_CHORD_PLAY_FLOOR);
	for (i = 0; i < 16u; i++) {
		if (seen[i] > 1u) {
			CHECK(false, "mask %s appears in more than one band", mstr((uint8_t)i));
		}
	}
	CHECK(true, "no mask is claimed by two different bands");

	/* THE HARDWARE FINDING, asserted rather than only documented: every
	 * chord containing BOTH Track 3 and Track 4 is absent from the table,
	 * because those four land on top of PLAY. If someone adds one back
	 * without new hardware evidence, this fails. */
	{
		bool none_t3t4 = true;

		for (i = 0; i < st_chord_band_count; i++) {
			uint8_t m = st_chord_bands[i].mask;

			if ((m & ST_CHORD_T3) && (m & ST_CHORD_T4)) {
				none_t3t4 = false;
			}
		}
		CHECK(none_t3t4,
		      "no band claims a chord holding BOTH Track 3 and Track 4 -- those "
		      "four masks collide with PLAY on this ladder and are undecodable");
	}
	CHECK(st_chord_band_count == 11u,
	      "the table carries exactly the 11 decodable masks (4 singles, 5 of 6 "
	      "pairs, 2 of 4 triples) -- got %u", st_chord_band_count);
}

/* ================== 3. all four singles, and all-released ================= */
static void case_singles_and_release(void)
{
	st_track_chord_t c;
	uint32_t i;

	g_cases++;
	printf("\n-- Singles, and releasing everything\n");
	st_track_chord_reset(&c);
	for (i = 0; i < st_chord_band_count; i++) {
		uint8_t m = st_chord_bands[i].mask;
		uint8_t got;

		if (m != ST_CHORD_T1 && m != ST_CHORD_T2 &&
		    m != ST_CHORD_T3 && m != ST_CHORD_T4) {
			continue;
		}
		st_track_chord_reset(&c);
		got = settle_at(&c, band_centre(i), 6);
		CHECK(got == m, "single %s decodes alone", mstr(m));

		/* Release settles in ONE read: restoring the full mix must not
		 * make the player wait out a debounce. */
		got = st_track_chord_update(&c, 0);
		CHECK(got == 0u, "%s released -> mask 0 in a single read", mstr(m));
	}
}

/* ============ 4. forming a chord: add a finger, then remove one =========== */
static void case_add_and_remove(void)
{
	st_track_chord_t c;
	uint8_t got;
	int t1 = -1, t4 = -1, t1t4 = -1;
	uint32_t i;

	g_cases++;
	printf("\n-- Forming and breaking T1 -> T1+T4 -> T4\n");
	for (i = 0; i < st_chord_band_count; i++) {
		if (st_chord_bands[i].mask == ST_CHORD_T1) t1 = band_centre(i);
		if (st_chord_bands[i].mask == ST_CHORD_T4) t4 = band_centre(i);
		if (st_chord_bands[i].mask == (ST_CHORD_T1 | ST_CHORD_T4)) t1t4 = band_centre(i);
	}
	CHECK(t1 > 0 && t4 > 0 && t1t4 > 0, "setup: the three bands exist");

	st_track_chord_reset(&c);
	got = settle_at(&c, t1, 6);
	CHECK(got == ST_CHORD_T1, "Track 1 alone: %s", mstr(got));

	/* Adding Track 4 sweeps the reading upward past several legal bands.
	 * The slew guard must suppress every one of them: at no point may an
	 * intermediate mask be committed. */
	{
		int v;
		bool saw_bogus = false;

		for (v = t1; v < t1t4; v += 150) {   /* a fast, realistic transit */
			uint8_t m = st_track_chord_update(&c, v);

			if (m != ST_CHORD_T1) {
				saw_bogus = true;
			}
		}
		CHECK(!saw_bogus,
		      "sweeping from T1 up to T1+T4 never commits an intermediate chord -- "
		      "the slew guard suppresses the bands the finger travels through");
	}
	got = settle_at(&c, t1t4, 6);
	CHECK(got == (ST_CHORD_T1 | ST_CHORD_T4), "chord formed: %s", mstr(got));

	/* Release Track 1, keep Track 4 down. Only that stem leaves the mask. */
	{
		int v;

		for (v = t1t4; v > t4; v -= 150) {
			(void)st_track_chord_update(&c, v);
		}
	}
	got = settle_at(&c, t4, 6);
	CHECK(got == ST_CHORD_T4,
	      "releasing Track 1 leaves exactly Track 4 held (%s) -- the other member "
	      "is unaffected", mstr(got));

	got = st_track_chord_update(&c, 0);
	CHECK(got == 0u, "releasing the last Track restores the full mix immediately");
}

/* ================= 5. guard zones HOLD, never guess ====================== */
static void case_guard_zone_holds(void)
{
	st_track_chord_t c;
	uint8_t got;
	int gap;
	uint32_t i;

	g_cases++;
	printf("\n-- Unclaimed readings hold the settled mask\n");

	/* Settle on the first band, then feed a value in the guard zone above
	 * it. The mask must not change and must not clear. */
	st_track_chord_reset(&c);
	got = settle_at(&c, band_centre(0), 6);
	CHECK(got == st_chord_bands[0].mask, "settled on %s", mstr(got));

	gap = ((int)st_chord_bands[0].hi + (int)st_chord_bands[1].lo) / 2;
	for (i = 0; i < 20u; i++) {
		got = st_track_chord_update(&c, gap);
	}
	CHECK(got == st_chord_bands[0].mask,
	      "20 consecutive readings in the guard zone at raw %d HOLD %s -- an "
	      "unclaimed voltage never renames or clears a held chord", gap,
	      mstr(st_chord_bands[0].mask));

	/* PLAY's territory likewise holds rather than clearing. */
	for (i = 0; i < 10u; i++) {
		got = st_track_chord_update(&c, ST_CHORD_PLAY_FLOOR + 200);
	}
	CHECK(got == st_chord_bands[0].mask,
	      "a reading up in PLAY's band holds too -- it is the transport's, not a "
	      "reason to drop the chord");
}

/* ============== 6. noise on a held chord cannot make it flicker =========== */
static void case_noise_does_not_flicker(void)
{
	st_track_chord_t c;
	uint8_t want, got;
	int centre, n;
	bool stable = true;
	uint32_t idx = 0;

	g_cases++;
	printf("\n-- A held chord rides out ADC noise\n");
	for (idx = 0; idx < st_chord_band_count; idx++) {
		if (st_chord_bands[idx].mask == (ST_CHORD_T1 | ST_CHORD_T2)) {
			break;
		}
	}
	CHECK(idx < st_chord_band_count, "setup: T1+T2 band exists");
	centre = band_centre(idx);
	want = st_chord_bands[idx].mask;

	st_track_chord_reset(&c);
	got = settle_at(&c, centre, 6);
	CHECK(got == want, "settled on %s at raw %d", mstr(want), centre);

	/* +/- a few counts, alternating, the way a real noisy rail behaves --
	 * and deliberately reaching past the un-widened band edge, which is
	 * what the hysteresis exists for. */
	for (n = 0; n < 200; n++) {
		int jitter = (n % 2) ? +4 : -4;
		int reach  = (n % 17 == 0) ? ((n % 34 == 0) ? +5 : -5) : 0;

		got = st_track_chord_update(&c, centre + jitter + reach);
		if (got != want) {
			stable = false;
		}
	}
	CHECK(stable,
	      "200 noisy reads around raw %d never move the mask off %s", centre,
	      mstr(want));
}

/* ========== 7. a brief wrong band cannot commit (settle count) =========== */
static void case_brief_glitch_rejected(void)
{
	st_track_chord_t c;
	uint8_t got;
	int a = band_centre(0), b = band_centre(1);
	uint32_t i;

	g_cases++;
	printf("\n-- A band seen for fewer than %u reads never commits\n",
	       ST_CHORD_SETTLE_READS);

	st_track_chord_reset(&c);
	(void)settle_at(&c, a, 6);

	/* Land exactly on band 1 for one read fewer than the settle count, then
	 * return. Because the slew guard also applies, step in gently. */
	for (i = 0; i + 1u < ST_CHORD_SETTLE_READS; i++) {
		got = st_track_chord_update(&c, b);
		CHECK(got == st_chord_bands[0].mask,
		      "read %u of a glitch onto %s still reports %s", i + 1u,
		      mstr(st_chord_bands[1].mask), mstr(st_chord_bands[0].mask));
	}
	got = st_track_chord_update(&c, a);
	CHECK(got == st_chord_bands[0].mask,
	      "the glitch ends and the original mask was never disturbed");
}

/* ============ 8. the measured DFU band really is T1+T4 now =============== */
static void case_measured_chord_band(void)
{
	st_track_chord_t c;
	uint8_t got;
	int v;
	bool all_ok = true;

	g_cases++;
	printf("\n-- The one MEASURED chord band (T1+T4, the old DFU failsafe)\n");

	/* main.c used to answer TRK_NONE for this whole band -- the truthful
	 * answer when the decoder had no way to say "two pressed". It can say
	 * it now, so 1+4 is a real two-stem solo instead of doing nothing. */
	for (v = 1306; v <= 1350; v += 4) {
		st_track_chord_reset(&c);
		got = settle_at(&c, v, 6);
		if (got != (ST_CHORD_T1 | ST_CHORD_T4)) {
			all_ok = false;
			printf("      raw %4d -> %s (expected 1001)\n", v, mstr(got));
		}
	}
	CHECK(all_ok,
	      "every raw value across the T1+T4 band settles on 1001 -- the only chord "
	      "in this table backed by a hardware measurement rather than the model");
}

/* ======== 9. no reading anywhere can be decoded as a wrong single ========= */
static void case_never_a_wrong_single(void)
{
	int v;
	bool ok = true;
	uint8_t m;

	g_cases++;
	printf("\n-- Exhaustive sweep: no voltage decodes to a mask outside its band\n");

	for (v = 0; v < 2100; v++) {
		if (!st_track_chord_lookup(v, 0u, &m)) {
			continue;   /* guard zone or PLAY: no claim made, which is safe */
		}
		if (m == 0u) {
			if (v > ST_CHORD_IDLE_MAX) {
				ok = false;
			}
			continue;
		}
		{
			bool inside = false;
			uint32_t i;

			for (i = 0; i < st_chord_band_count; i++) {
				if (st_chord_bands[i].mask == m &&
				    v >= (int)st_chord_bands[i].lo &&
				    v <= (int)st_chord_bands[i].hi) {
					inside = true;
				}
			}
			if (!inside) {
				ok = false;
				printf("      raw %4d claimed %s but is outside that band\n",
				       v, mstr(m));
			}
		}
	}
	CHECK(ok,
	      "sweeping every raw value 0..2099: a mask is only ever returned for a "
	      "value genuinely inside that mask's own band");
}

int main(void)
{
	printf("STEM TAPE TRACK-CHORD DECODER\n");
	printf("driving the REAL st_track_chord_update() and the REAL band table\n");
	printf("masks are printed T1..T4 left to right, so 1001 == Track 1 + Track 4\n");

	case_every_band();
	case_table_is_sane();
	case_singles_and_release();
	case_add_and_remove();
	case_guard_zone_holds();
	case_noise_does_not_flicker();
	case_brief_glitch_rejected();
	case_measured_chord_band();
	case_never_a_wrong_single();

	printf("\n%s (%d cases, %d checks, %d failures)\n",
	       g_failures ? "TRACK-CHORD TEST FAILED" : "TRACK-CHORD TEST PASSED",
	       g_cases, g_checks, g_failures);
	printf("NOTE: only the T1+T4 band is a hardware measurement. The other chord\n"
	       "      centres are model-derived, which is why the bands are narrow and\n"
	       "      an unrecognised reading holds. A chord may fail to trigger on a\n"
	       "      real device; it must never trigger the WRONG stems.\n");
	return g_failures ? 1 : 0;
}
