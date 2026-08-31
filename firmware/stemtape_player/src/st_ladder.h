/*
 * st_ladder.h — THE classifier for the SP-1's shared AIN0 control ladder.
 *
 * PURE: no Zephyr, no ADC, no clock. main.c takes ONE ladder sample per
 * control pass and hands it here; this module owns the band table, the
 * settling and the hysteresis, and answers with the settled physical state:
 * idle, a 4-bit Track mask, or PLAY.
 *
 * ======================================================================
 * ONE SAMPLE, ONE CLASSIFIER
 * ======================================================================
 * PLAY and Track 1..4 share SAADC AIN0. There is no per-button digital
 * input, so a chord is not two readings -- it is ONE new voltage, and PLAY
 * is just the highest rung of the same ladder. Splitting that one voltage
 * between two independent decoders is how the two can disagree about what
 * the player is physically holding, so there is exactly one decoder and it
 * answers for the whole ladder including PLAY.
 *
 * This REPLACES the st15 arrangement, in which main.c's inherited
 * decode_tracks() and a separate chord decoder each interpreted the same
 * raw value on their own. Whenever a Stem Tape song is selected this module
 * is the sole owner; decode_tracks() survives only for the inherited Tape
 * Looper behaviour with no stem song, which is not ours to redefine.
 *
 * ======================================================================
 * WHERE THE NUMBERS COME FROM -- MEASURED, NOT MODELLED
 * ======================================================================
 * docs/ladder-measured.json holds the physical capture (build st16-cal, on
 * real hardware) that every band below is derived from, together with the
 * derivation rule and the observed jitter. It is the provenance, and
 * tests/test_ladder.c re-derives this table from it, so a hand-edited band
 * cannot silently diverge from the measurement.
 *
 * Topology, confirmed by the fit: each button sources VDD into the ADC node
 * through its own series resistor; the node is pulled down by a common
 * resistor. Pressing two buttons puts their resistors in PARALLEL, raising
 * the voltage above either single press. So every chord reads higher than
 * its highest member -- and, on this hardware, the 15 masks come out in
 * exact numerical mask order: mask 1 is the lowest voltage, mask 15 the
 * highest, with PLAY above all of them.
 *
 * ALL FIFTEEN MASKS ARE DECODABLE. st15's header claimed every chord
 * containing both Track 3 and Track 4 collided with PLAY. Measurement
 * disproves it: T3+T4 1559, T1+T3+T4 1628, T2+T3+T4 1695, all four 1755,
 * PLAY 1813. The tightest clearance on the whole ladder is 58 counts, and
 * the worst jitter the capture produced was 6. The false claim came from a
 * model anchored on 1335 -- the midpoint of the WIDE 1280..1390 DFU
 * tolerance band -- instead of the true T1+T4 centre of 1309.
 *
 * ======================================================================
 * BANDS AND THE GUARD ZONES BETWEEN THEM
 * ======================================================================
 * Each band is its measured centre +/- min(25, 40% of the nearest gap), so
 * no band can ever reach a neighbour's centre and adjacent controls cannot
 * alias. Everything between bands is guard zone.
 *
 * A guard-zone reading is UNKNOWN and HOLDS the last settled state. It never
 * proposes a new one. The failure mode is therefore a MISSING chord, never a
 * wrong one -- the player hears no solo, notices, and nothing musical breaks.
 *
 * ======================================================================
 * SETTLING -- AND WHY THERE IS NO SLEW GUARD
 * ======================================================================
 * st15 had one, and it is the reason chords did nothing on real hardware:
 *
 *     if (|raw - last_raw| > 40) return c->settled;   // no progress
 *
 * main.c's own ladder_read() comment records that audio and USB activity
 * couple into the shared BTN_COM rail while a song streams. Any sustained
 * coupling wider than that threshold made every pass a no-progress pass, so
 * the mask latched at zero for as long as the song played -- and the
 * calibration that would have caught it could only run with the transport
 * STOPPED, where the coupling is absent.
 *
 * It also bought nothing. A slew guard exists to reject a value sweeping
 * through legal bands on its way somewhere -- the failure mode of a FADER.
 * This is a switched resistor ladder: closing a contact steps the node in
 * microseconds, four orders of magnitude below the ~8 ms control cadence, so
 * there is no sweep to reject. Two mechanisms already cover the real cases,
 * and neither can deadlock:
 *
 *   GUARD ZONES. A reading that is not positively in a band is a NON-VOTE:
 *   it holds the settled state and leaves the pending candidate untouched.
 *   It carries no evidence for any state, so it argues neither way. It
 *   cannot build a false candidate -- only an in-band reading ever names one
 *   -- and it cannot stall the decoder, which clearing or eroding the
 *   candidate would: 50%-duty coupling would oscillate the count and never
 *   let it reach the threshold, the same class of deadlock as the slew
 *   guard, with a longer fuse.
 *
 *   SETTLE COUNT. A new state must be seen ST_LADDER_SETTLE_READS times
 *   consecutively before it replaces the settled state (~24 ms at main.c's
 *   cadence) -- the same discipline the inherited single-button debounce
 *   uses, for the same documented reason.
 *
 *   HYSTERESIS. Once settled, that row's band widens by ST_LADDER_HYSTERESIS
 *   on both sides, so a held state resting near its own edge cannot flicker.
 *
 * An ADC ERROR (main.c's ladder_read() returns -1) is UNKNOWN, not idle.
 * st15 classified it as idle and dropped a held chord instantly on a single
 * failed conversion.
 *
 * RELEASE IS DEBOUNCED SEPARATELY, and more lightly. ST_LADDER_RELEASE_READS
 * agreeing idle readings clear the held state -- enough that a single
 * coupled dip cannot drop a chord mid-phrase, few enough (~16 ms) that
 * letting go still feels instant. The press path stays at
 * ST_LADDER_SETTLE_READS because committing the WRONG chord is the more
 * expensive mistake.
 */

#ifndef ST_LADDER_H_
#define ST_LADDER_H_

#include <stdbool.h>
#include <stdint.h>

/* Bit k == Track (k+1) held. Bit 0 is Track 1. */
#define ST_LADDER_T1 0x1u
#define ST_LADDER_T2 0x2u
#define ST_LADDER_T3 0x4u
#define ST_LADDER_T4 0x8u
#define ST_LADDER_MASK_ALL 0xFu

#define ST_LADDER_SETTLE_READS 3u

/* Consecutive IDLE readings that confirm a release. Deliberately shorter
 * than the press path: restoring the full four-stem mix must feel immediate.
 * Deliberately more than one: a single coupled dip to idle is indistinguish-
 * able from a release, and dropping a held chord on it would flicker. */
#define ST_LADDER_RELEASE_READS 2u
#define ST_LADDER_HYSTERESIS   6   /* widening applied to the settled row */

/* Below this the ladder is idle: no Track and no PLAY is held. Identical to
 * the inherited decode_tracks()' own floor so idle can never be disputed
 * between the two decoders in the mixed classic/stem build. */
#define ST_LADDER_IDLE_MAX 110

/* Rows in the band table: the 15 Track masks, then PLAY. */
#define ST_LADDER_ROWS 16u

typedef enum {
	ST_LADDER_UNKNOWN = 0,  /* guard zone, or ADC error -- HOLD */
	ST_LADDER_IDLE,
	ST_LADDER_TRACKS,       /* .mask is 1..15 */
	ST_LADDER_PLAY,
} st_ladder_class_t;

typedef struct {
	st_ladder_class_t cls;
	uint8_t           mask;   /* meaningful only for ST_LADDER_TRACKS */
} st_ladder_read_t;

typedef struct {
	uint16_t lo;
	uint16_t hi;
	uint8_t  mask;   /* 1..15 on a Track row; 0 on the PLAY row */
	uint8_t  play;   /* 1 on the PLAY row */
} st_ladder_band_t;

typedef struct {
	uint8_t           settled_mask;   /* 0..15; 0 while PLAY or idle */
	bool              settled_play;
	st_ladder_class_t cand_cls;
	uint8_t           cand_mask;
	uint8_t           cand_count;
} st_ladder_t;

void st_ladder_reset(st_ladder_t *l);

/*
 * Pure band lookup. `settled_mask`/`settled_play` name the currently settled
 * row so it -- and only it -- gets ST_LADDER_HYSTERESIS of extra width.
 * Returns ST_LADDER_UNKNOWN for any guard zone and for raw < 0.
 */
st_ladder_read_t st_ladder_classify(int raw, uint8_t settled_mask, bool settled_play);

/*
 * Feed ONE raw ladder reading. Total and deterministic: no clock, no
 * allocation, no I/O, and no path that can stop making progress. Read the
 * result with st_ladder_mask()/st_ladder_play().
 */
void st_ladder_update(st_ladder_t *l, int raw);

/* THE settled Track mask -- the single value both the mixer and the LEDs
 * consume, so they cannot hold different opinions about what is held. */
static inline uint8_t st_ladder_mask(const st_ladder_t *l)
{
	return l->settled_mask;
}

/* THE settled PLAY state, from the same sample as the mask above. */
static inline bool st_ladder_play(const st_ladder_t *l)
{
	return l->settled_play;
}

/* The band table itself, exposed so the tests and the CI gate read the SAME
 * rows the firmware decodes against rather than a transcription of them. */
extern const st_ladder_band_t st_ladder_bands[ST_LADDER_ROWS];

#endif /* ST_LADDER_H_ */
