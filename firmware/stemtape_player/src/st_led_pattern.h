/*
 * st_led_pattern.h — Stem Tape standalone player: local semantic LED
 * renderer, driven entirely by audio-frame/firmware time. USB/browser
 * input is never required to animate it (task section 8).
 *
 * PURE: no Zephyr, no PWM. Physical inventory matches the M0 target's
 * already-verified table (firmware/stemtape/src/led_duty.h): exactly eight
 * LEDs, indices 0-3 the four Track LEDs, indices 4-7 the four side LEDs.
 * This module does not depend on firmware/stemtape's files (a different
 * app target) but uses the SAME physical convention.
 *
 * Composition model (task: "an explicit priority table so temporary
 * indications restore the underlying state correctly"):
 *
 *   1. A one-shot overlay (st_led_oneshot_t), if currently active, renders
 *      the ENTIRE frame and wins outright.
 *   2. Otherwise, a prioritized BASE state (st_led_select_base()) is
 *      chosen fresh from current inputs every call, and renders the
 *      complete frame with its own continuous overlays (loop/FX-momentary/
 *      gate/scrub) layered on top of the stock playback/battery display.
 *
 * "Restore" is automatic and cannot go stale: once a one-shot expires
 * (checked against `now_ms`, no infinite blink placeholder), the very next
 * render call recomputes the base state from whatever the CURRENT inputs
 * are — never a snapshot taken before the interruption.
 */

#ifndef STEMTAPE_PLAYER_LED_PATTERN_H_
#define STEMTAPE_PLAYER_LED_PATTERN_H_

#include <stdbool.h>
#include <stdint.h>

#define ST_LED_COUNT           8u
#define ST_LED_TRACK_ROW_COUNT 4u
#define ST_LED_SIDE_ROW_COUNT  4u
#define ST_LED_LEVEL_MAX       255u

/* Side-row index nearest PLAY (matches the M0 target's LED_IDX_SIDE_PLAY
 * convention: index 4, the side LED whose local baseline is the Play
 * indicator / battery step 1). */
#define ST_LED_SIDE_PLAY 4u

typedef struct {
	uint8_t level[ST_LED_COUNT];
} st_led_frame_t;

/* ---- per-stem status (task: "empty, loaded, muted, soloed, and linked
 * stems remain visually distinguishable") ---- */
typedef enum {
	ST_LED_STEM_EMPTY = 0,
	ST_LED_STEM_LOADED,
	ST_LED_STEM_MUTED,
	ST_LED_STEM_SOLOED,
	ST_LED_STEM_LINKED,
} st_led_stem_status_t;

typedef struct {
	st_led_stem_status_t stem_status[ST_LED_TRACK_ROW_COUNT];
	uint8_t  active_stem;       /* 0..3 */
	uint8_t  stem_activity[ST_LED_TRACK_ROW_COUNT]; /* 0..255, per-stem VU-style
							  * level; supplied by the
							  * (deferred) audio engine */
	bool     playing;

	uint8_t  battery_level;     /* 0..4 quarters, 0xFF = unknown/unavailable
				      * (never rendered as "empty" -- mirrors
				      * the M0 target's battery-gauge safety
				      * rule) */
	bool     charging;
	bool     charge_complete;

	bool     loop_active;       /* momentary or latched */

	bool     fx_momentary_active; uint8_t fx_momentary_bank; /* 0..3 */
	bool     fx_latched[4];                                   /* solid per bank */

	bool     gate_active;       /* audio-clock-synced rapid pulse */
	uint32_t gate_period_ms;    /* from the BPM/frame clock, not wall-clock polling */

	bool     scrub_active;
	int8_t   scrub_direction;   /* +1 / -1 */
	uint8_t  scrub_speed_index; /* 0..3 -- also visibly sets the chase rate,
				      * "four scrub levels: Track LEDs indicate
				      * the selected level" */
} st_led_inputs_t;

typedef enum {
	ST_LED_BASE_IDLE = 0,        /* stock playback/battery display */
	ST_LED_BASE_LOADING,
	ST_LED_BASE_TRANSFER,        /* all four Track LEDs blink together (docs/stem-tape-transfer-v1.md §7) */
	ST_LED_BASE_LOW_BATTERY,
	ST_LED_BASE_STORAGE_ERROR,
} st_led_base_t;

/* Priority (highest first): storage error is the most safety-relevant
 * (something is actually wrong with the media); low battery outranks
 * ordinary operation but not a storage error; transfer and loading are
 * mutually exclusive operational states; idle is the default. Mirrors the
 * M0 target's led_render_select() shape/reasoning exactly. */
st_led_base_t st_led_select_base(bool storage_error, bool low_battery, bool transferring,
				  bool loading);

/* Renders the complete base-state frame (idle stock playback/VU + active
 * stem brighten + stem-status distinguishability + battery/charging side
 * row + loop/FX-momentary/gate/scrub continuous overlays), or the
 * TRANSFER/LOADING/LOW_BATTERY/STORAGE_ERROR pattern when `base` selects
 * one of those. */
void st_led_render_base(st_led_base_t base, const st_led_inputs_t *in, uint32_t now_ms,
			 st_led_frame_t *out);

/* ---- one-shot overlays ---- */
typedef enum {
	ST_LED_ONESHOT_NONE = 0,
	ST_LED_ONESHOT_BOOT_FLASH,       /* task: "all four Track LEDs flash together once" */
	ST_LED_ONESHOT_FX_LATCH_FLASH,   /* task: FX latch/unlatch -> flash once, then restore */
	ST_LED_ONESHOT_REJECTION,        /* an input was rejected (e.g. upload CRC failure) */
} st_led_oneshot_id_t;

typedef struct {
	st_led_oneshot_id_t id;
	uint32_t started_ms;
	uint32_t duration_ms; /* firmware-time expiry -- never an infinite placeholder */
} st_led_oneshot_t;

/* Starts (or replaces) the active one-shot with its documented duration. */
void st_led_oneshot_start(st_led_oneshot_t *o, st_led_oneshot_id_t id, uint32_t now_ms);

/* True iff a one-shot is currently within its duration (expiry is checked
 * here, wrap-safe, the same `(uint32_t)(now - started) < duration` rule
 * used throughout this codebase's other firmware-time checks). */
bool st_led_oneshot_active(const st_led_oneshot_t *o, uint32_t now_ms);

void st_led_render_oneshot(const st_led_oneshot_t *o, uint32_t now_ms, st_led_frame_t *out);

/*
 * Top-level entry point: one-shot wins outright while active, otherwise
 * the base state (recomputed fresh from `in` every call) renders. This is
 * the single function main.c calls once per render tick.
 */
void st_led_render(const st_led_oneshot_t *oneshot, st_led_base_t base,
		    const st_led_inputs_t *in, uint32_t now_ms, st_led_frame_t *out);

#endif /* STEMTAPE_PLAYER_LED_PATTERN_H_ */
