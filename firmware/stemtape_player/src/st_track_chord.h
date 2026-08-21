/*
 * st_track_chord.h — decoding MULTIPLE simultaneously-held Track buttons from
 * the SP-1's one shared analog ladder, as a 4-bit mask.
 *
 * PURE: no Zephyr, no ADC, no clock. main.c reads the raw ladder value it
 * already reads and feeds it here; this module owns the band table, the
 * settling and the hysteresis, and answers with a settled mask.
 *
 * ======================================================================
 * WHY A MASK AT ALL
 * ======================================================================
 * PLAY and Track 1..4 share SAADC AIN0. There is no per-button digital
 * input. A chord is therefore not two readings -- it is ONE new voltage.
 * main.c's inherited decode_tracks() maps every voltage onto exactly one
 * button identity and has no notion of "two pressed", so a real two-finger
 * chord has always decoded as some single button the player never pressed.
 * That is the bug this module exists to end, and it matters more than the
 * feature does: silently soloing the wrong stem mid-performance is worse
 * than not supporting chords.
 *
 * ======================================================================
 * THE LADDER, AND WHERE THESE NUMBERS COME FROM
 * ======================================================================
 * Topology, inferred and then CONFIRMED (see below): each button sources
 * VDD into the ADC node through its own series resistor, and the node is
 * pulled down by a common resistor. Pressing two buttons puts their
 * resistors in PARALLEL, which LOWERS the source resistance and therefore
 * RAISES the voltage above either single press. Every chord reads higher
 * than its highest member, and the 15 masks are strictly ordered.
 *
 * Measured single-button centres, from the pinned Tape Looper's own
 * decode_tracks() (firmware/src/main.c @ a8dd127, hardware-validated on
 * real SP-1s for the whole life of that firmware):
 *
 *      T1 ~213    T2 ~403    T3 ~733    T4 ~1220    PLAY ~1823
 *
 * Measured CHORD bands in this repository: exactly ONE. Track 1 + Track 4,
 * raw 1280..1390 -- the bootloader DFU failsafe band, which main.c already
 * recognises and rejects. firmware/stemtape/README.md states this plainly
 * ("The pinned looper revision contains exactly one measured chord band...
 * All other chord bands are UNMEASURED") and lists the rest as still
 * requiring physical measurement. That is still true. Nothing below is a
 * hardware measurement of a chord except T1+T4.
 *
 * MODEL, AND ITS ONE VALIDATION POINT. Solving the five measured singles
 * for their resistor ratios and combining conductances predicts T1+T4 at
 * 1329. The measured band's centre is 1335. Six counts of residual out of
 * a ~4200-count full scale is a strong structural confirmation of the
 * topology -- but it is ONE point, and the singles themselves are quoted
 * as approximate. So the model is trusted for ORDERING and for SPACING,
 * and is NOT trusted to place a band edge to the count.
 *
 * ======================================================================
 * WHAT THAT BUYS, AND WHAT IT COSTS: THE HARDWARE FINDING
 * ======================================================================
 * Predicted centres, ascending, with PLAY in its true position:
 *
 *   T1 213 | T2 403 | T1+T2 578 | T3 733 | T1+T3 879 | T2+T3 1012 |
 *   T1+T2+T3 1136 | T4 1220 | T1+T4 1329 | T2+T4 1429 | T1+T2+T4 1523 |
 *   T3+T4 1609 | T1+T3+T4 1691 | T2+T3+T4 1768 | PLAY 1823 | ALL4 1841
 *
 * The four highest chords land ON TOP OF PLAY. T2+T3+T4 (1768) and all
 * four (1841) straddle PLAY (1823) within 55 and 18 counts respectively.
 * PLAY is the transport control and cannot be given up, so those masks
 * cannot be decoded on this hardware. The rule is simple and worth stating
 * once:
 *
 *      ANY CHORD CONTAINING BOTH TRACK 3 AND TRACK 4 IS UNUSABLE.
 *
 * That is exactly {T3+T4, T1+T3+T4, T2+T3+T4, T1+T2+T3+T4}. It is a
 * property of the resistor values, not of this code, and no amount of
 * filtering recovers it. The remaining eleven masks -- four singles, five
 * of the six pairs, two of the four triples -- are separated by 73 counts
 * or more and are decodable.
 *
 * ======================================================================
 * FAIL-SAFE BAND POLICY
 * ======================================================================
 * Because the chord centres are model-derived rather than measured, the
 * bands below are deliberately NARROW: each keeps only the central ~42% of
 * the gap to its neighbours, leaving a wide unclaimed guard zone on both
 * sides. A reading in a guard zone is UNKNOWN, and an unknown reading
 * HOLDS the last settled mask -- it never proposes a new one.
 *
 * The consequence is the one we want. If a real device's chord centre sits
 * further from the model than the band half-width, that chord simply does
 * not trigger: the player gets no solo, notices, and nothing musical
 * breaks. The failure mode is a MISSING chord, never a WRONG one. Widening
 * a band after measuring the real device is a one-line edit to the table.
 *
 * ======================================================================
 * SETTLING
 * ======================================================================
 * Two independent guards, because ADC noise and finger transit are
 * different problems:
 *
 *   SLEW GUARD. While the reading is moving faster than ST_CHORD_SLEW_MAX
 *   counts per call, nothing commits. A finger travelling from T1 up to
 *   T1+T4 sweeps through six other legal bands on the way; the sweep is
 *   fast, so the slew guard suppresses every one of them. This is what
 *   stops a chord being built out of the false intermediate states its own
 *   formation passes through.
 *
 *   SETTLE COUNT. A new mask must then be seen ST_CHORD_SETTLE_READS times
 *   consecutively before it replaces the settled mask. At main.c's ~8 ms
 *   control cadence that is ~24 ms -- the same discipline the inherited
 *   single-button debounce already uses, for the same documented reason
 *   (audio and USB activity couple into the button ladder while a song
 *   streams).
 *
 *   HYSTERESIS. Once a mask is settled its band is widened by
 *   ST_CHORD_HYSTERESIS counts on both sides, so a held chord sitting near
 *   its own edge cannot flicker out and back.
 *
 * RELEASE IS NOT DEBOUNCED THE SAME WAY. Going fully idle (below the T1
 * floor) settles in one read. Releasing every Track must restore the full
 * four-stem mix immediately; making the player wait 24 ms to hear the song
 * come back is a worse trade than the (harmless) risk of an early restore.
 */

#ifndef ST_TRACK_CHORD_H_
#define ST_TRACK_CHORD_H_

#include <stdbool.h>
#include <stdint.h>

/* Bit k == Track (k+1) held. Bit 0 is Track 1. */
#define ST_CHORD_T1 0x1u
#define ST_CHORD_T2 0x2u
#define ST_CHORD_T3 0x4u
#define ST_CHORD_T4 0x8u
#define ST_CHORD_MASK_ALL 0xFu

#define ST_CHORD_SETTLE_READS 3u
#define ST_CHORD_SLEW_MAX     40   /* counts/call above which nothing commits */
#define ST_CHORD_HYSTERESIS   6    /* widening applied to the settled band */

/* Below this the ladder is idle: no Track (and no PLAY) is held. Matches the
 * inherited decode_tracks()' own 110 floor, kept identical so idle detection
 * cannot disagree between the two decoders. */
#define ST_CHORD_IDLE_MAX 110

/* PLAY's floor. At or above this the reading belongs to the transport, never
 * to a Track chord -- see the hardware finding above. */
#define ST_CHORD_PLAY_FLOOR 1600

typedef struct {
	uint16_t lo;
	uint16_t hi;
	uint8_t  mask;
} st_chord_band_t;

typedef struct {
	uint8_t  settled;      /* last settled mask, 0..0xF */
	uint8_t  cand;         /* mask currently accumulating agreement */
	uint8_t  cand_count;
	bool     have_last;
	int      last_raw;
} st_track_chord_t;

void st_track_chord_reset(st_track_chord_t *c);

/*
 * Pure band lookup. True and *mask_out set iff `raw` falls inside a claimed
 * band; false for idle-vs-band guard zones, for every unclaimed guard zone,
 * and for everything at or above ST_CHORD_PLAY_FLOOR. `settled_mask` widens
 * that mask's own band by ST_CHORD_HYSTERESIS (pass 0 for no widening).
 */
bool st_track_chord_lookup(int raw, uint8_t settled_mask, uint8_t *mask_out);

/*
 * Feed one raw ladder reading; returns the mask that should be acted on now
 * (which is the PREVIOUS settled mask until a new one earns its place).
 * Total and deterministic: no clock, no allocation, no I/O.
 */
uint8_t st_track_chord_update(st_track_chord_t *c, int raw);

/* The band table itself, exposed so tests and the CI gate read the SAME rows
 * the firmware decodes against rather than a transcription of them. */
extern const st_chord_band_t st_chord_bands[];
extern const uint32_t        st_chord_band_count;

#endif /* ST_TRACK_CHORD_H_ */
