/*
 * st_fx_catalog.h — Stem Tape FX bank/algorithm catalog, ported VERBATIM
 * from src/machine/fx12.ts (the twelve-effect model: four banks of three
 * algorithms). Do not rename, omit, or invent algorithms — this table is
 * the id/label/button-mapping/default-macro source of truth; if fx12.ts
 * ever changes, update this table to match, by hand.
 *
 * PURE: this is a routing/selection catalog only. The actual per-algorithm
 * DSP audio processing (the Web Audio graphs in src/audio/fx/banks.ts) is
 * NOT ported here — that is real-time embedded audio DSP work deferred
 * beyond this release (see the firmware README's "Deferred" section).
 * What IS implemented and host-tested is which algorithm is selected, in
 * which scope, latched or not — the part st_gesture.c's FX Track grammar
 * needs.
 */

#ifndef STEMTAPE_PLAYER_FX_CATALOG_H_
#define STEMTAPE_PLAYER_FX_CATALOG_H_

#include <stdbool.h>
#include <stdint.h>

/* Bank array index (NOT physical button index — see ST_FX_BANK_BY_BUTTON). */
#define ST_FX_BANK_TONE   0u
#define ST_FX_BANK_MOD    1u
#define ST_FX_BANK_MOTION 2u
#define ST_FX_BANK_SPACE  3u
#define ST_FX_BANK_COUNT  4u

#define ST_FX_ALGO_COUNT_PER_BANK 3u

typedef struct {
	const char *id;      /* fx12.ts AlgorithmId, verbatim (e.g. "exciter", never "isolator") */
	const char *label;
	uint8_t     default_macro_q8; /* fx12.ts defaultMacro (0..1) as a 0..255 fixed point */
	bool        heavy;   /* fx12.ts `heavy`: may be rejected on weak devices */
} st_fx_algorithm_t;

typedef struct {
	const char *id;
	const char *label;
	uint8_t     button_index;   /* fx12.ts BankDef.buttonIndex: 0-based physical Track button */
	st_fx_algorithm_t algorithms[ST_FX_ALGO_COUNT_PER_BANK];
} st_fx_bank_t;

/* Declared in SIGNAL ORDER exactly like fx12.ts's BANKS: source -> TONE ->
 * MOD -> MOTION -> SPACE -> fader. Array index != button_index for MOD and
 * MOTION -- see ST_FX_BANK_BY_BUTTON below, which mirrors fx12.ts's own
 * BANK_BY_BUTTON derivation instead of re-deriving it differently here. */
static const st_fx_bank_t ST_FX_BANKS[ST_FX_BANK_COUNT] = {
	[ST_FX_BANK_TONE] = {
		.id = "tone", .label = "TONE", .button_index = 0,
		.algorithms = {
			{ "filter",  "Filter",         0x80u, false },  /* 0.5 */
			{ "exciter", "Exciter",        0x66u, false },  /* 0.4 -- NOT "isolator" */
			{ "dirt",    "Dirt / Crusher", 0x59u, false },  /* 0.35 */
		},
	},
	[ST_FX_BANK_MOD] = {
		.id = "mod", .label = "MOD", .button_index = 3,
		.algorithms = {
			{ "reelFlange",   "Reel Flange",   0x73u, false }, /* 0.45 */
			{ "formantShift", "Formant Shift", 0x80u, false }, /* 0.5  */
			{ "gate",         "Rhythmic Gate", 0x80u, false }, /* 0.5  */
		},
	},
	[ST_FX_BANK_MOTION] = {
		.id = "motion", .label = "MOTION", .button_index = 1,
		.algorithms = {
			{ "echo",      "Tempo Echo",      0x80u, false }, /* 0.5  */
			{ "pitchEcho", "Pitch Echo",      0x80u, false }, /* 0.5  */
			{ "scatter",   "Granular Scatter",0x66u, true  }, /* 0.4, heavy */
		},
	},
	[ST_FX_BANK_SPACE] = {
		.id = "space", .label = "SPACE", .button_index = 2,
		.algorithms = {
			{ "reverb",  "Reverb",          0x73u, false }, /* 0.45 */
			{ "shimmer", "Shimmer",         0x73u, true  }, /* 0.45, heavy */
			{ "freeze",  "Spectral Freeze", 0x80u, true  }, /* 0.5,  heavy */
		},
	},
};

/* fx12.ts's BANK_BY_BUTTON, precomputed by hand from button_index above so
 * the firmware never needs a runtime derivation loop:
 *   button 0 (Track1) -> TONE, button 1 (Track2) -> MOTION,
 *   button 2 (Track3) -> SPACE, button 3 (Track4) -> MOD (RHYTHM). */
static const uint8_t ST_FX_BANK_BY_BUTTON[ST_FX_BANK_COUNT] = {
	ST_FX_BANK_TONE, ST_FX_BANK_MOTION, ST_FX_BANK_SPACE, ST_FX_BANK_MOD,
};

static inline uint8_t st_fx_bank_of_button(uint8_t button_index /* 0..3 */)
{
	return (button_index < ST_FX_BANK_COUNT) ? ST_FX_BANK_BY_BUTTON[button_index] : ST_FX_BANK_TONE;
}

/* FREEZE_FEEDBACK_MAX from src/audio/fx/banks.ts: hard ceiling on the
 * Spectral Freeze loop gain, as a 0..255 fixed point (0.82 -> 0xD1). Kept
 * here as a citation for the (deferred) DSP implementation so the bound is
 * never re-derived differently when that work lands. */
#define ST_FX_FREEZE_FEEDBACK_MAX_Q8 0xD1u

#endif /* STEMTAPE_PLAYER_FX_CATALOG_H_ */
