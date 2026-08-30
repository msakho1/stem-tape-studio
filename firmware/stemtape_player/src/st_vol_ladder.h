/*
 * st_vol_ladder.h -- the AIN1 (Volume + FWD/RWD) resistor-ladder decode.
 *
 * Header-only and pure, for one reason: these band edges are now LOAD-BEARING
 * for a real gesture -- FX mode is entered by pressing Volume- and Volume+
 * together -- and while this lived as a `static` function inside main.c it
 * could not be host-tested at all. tests/test_vol_ladder.c exercises the
 * SAME function the firmware calls, against the same physical measurement
 * that set the edges, so a hand-edited threshold fails the build instead of
 * silently breaking the gesture on hardware.
 *
 * The companion rail AIN0 (Track + PLAY) is decoded by src/st_ladder.c
 * against docs/ladder-measured.json. This is the same idea for AIN1.
 *
 * ======================================================================
 * MEASURED ON HARDWARE, st20-VOLCAL capture, 2026-08-30
 * ======================================================================
 * Four buttons share this one analog rail. They source VDD through
 * individual resistors into a pulled-down node, so pressing two puts their
 * resistors in PARALLEL and the node reads HIGHER than either alone. The
 * capture confirms that model on this rail:
 *
 *     nothing pressed      1 ..   10
 *     Volume-            732 ..  742
 *     Volume+           1821 .. 1830
 *     BOTH together     2019 .. 2029
 *
 * Full provenance, including what was NOT measured, is in
 * docs/ain1-measured.json.
 *
 * WHY THIS MEASUREMENT EXISTED. Before it, the top band was "anything
 * >= 1500", so Volume+ and the chord both returned VOL_UP and were
 * indistinguishable. That was the single blocker on FX entry.
 *
 * WHERE THE SPLIT GOES. ST_VOL_CHORD_MIN is the MIDPOINT of the measured
 * 1830..2019 gap: 95 counts clear of both neighbours, symmetric, so noise in
 * either direction has equal room. Deliberately NOT a centre-plus-tolerance
 * band like st_ladder.c builds for AIN0 -- this is the top of the rail, so
 * nothing exists above the chord to collide with and one threshold is the
 * whole decision.
 *
 * THE TWO TEMPO BANDS BELOW ARE NOT FROM THIS CAPTURE. FWD/RWD were never
 * pressed during it. They are the inherited, unverified values they have
 * always been, left exactly as found. Nothing measured falls inside them, so
 * they do not conflict -- they are simply still unproven, and the test file
 * asserts only what was actually measured.
 */
#ifndef ST_VOL_LADDER_H
#define ST_VOL_LADDER_H

#include <stdbool.h>

enum vol_btn {
	VOL_NONE = -1,
	VOL_TEMPO_DOWN,
	VOL_DOWN,
	VOL_TEMPO_UP,
	VOL_UP,
	VOL_BOTH, /* Vol- and Vol+ together: the FX overlay entry chord */
};

#define ST_VOL_CHORD_RAW 2024  /* centre of the measured 2019..2029 plateau */
#define ST_VOL_CHORD_MIN 1925  /* midpoint of the measured 1830..2019 gap   */

static inline enum vol_btn st_vol_decode(int v)
{
	if (v <  200) return VOL_NONE;       /* MEASURED    1 ..   10 */
	if (v <  560) return VOL_TEMPO_DOWN; /* ~404, UNVERIFIED      */
	if (v <  950) return VOL_DOWN;       /* MEASURED  732 ..  742 */
	if (v < 1500) return VOL_TEMPO_UP;   /* ~1220, UNVERIFIED     */
	if (v >= ST_VOL_CHORD_MIN)
		return VOL_BOTH;             /* MEASURED 2019 .. 2029 */
	return VOL_UP;                       /* MEASURED 1821 .. 1830 */
}

/*
 * True when the raw AIN1 reading is the two-volume chord. Kept separate from
 * st_vol_decode() so the FX overlay asks a question with exactly one meaning
 * rather than pattern-matching an enum at its call site. The two are required
 * to agree at every input, which the test asserts across the whole 12-bit
 * range rather than at sampled points.
 */
static inline bool st_vol_is_chord(int v)
{
	return v >= ST_VOL_CHORD_MIN;
}

#endif /* ST_VOL_LADDER_H */
