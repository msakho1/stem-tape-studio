/*
 * st_led_mvp.h — THE single semantic owner of all eight Stem Tape LEDs.
 *
 * PURE: no Zephyr, no GPIO, no timers, no clock. main.c reads the real
 * runtime state, fills st_led_inputs_t, calls st_led_mvp_decide(), and
 * renders the resulting frame through the existing TIMER3/GPIO soft-PWM
 * driver. This module never touches hardware and never reads a global.
 *
 * WHY IT EXISTS. Before this, three different things owned the LEDs at
 * once: the inherited Tape Looper masks (show_song_leds() painting the
 * side row as a 16-song bank/position display, plus the standby chase on
 * the track row), an ad-hoc per-stem peak meter, and a separate semantic
 * design in st_led_pattern.c/led_render.c that was never linked into the
 * firmware at all. The physical result on a one-song device was a side
 * LED blinking to say "song 1 of 16" -- correct for the Tape Looper,
 * meaningless here. One owner, one frame, one renderer replaces all of it.
 *
 * AUTHORITY. The behaviour below is taken from, in the priority order the
 * product owner set:
 *   1. docs/FIRMWARE_CONTRACT_V1.md
 *   2. docs/firmware-contract-v1.json
 *   3. docs/stem-tape-led-feedback-v1.md
 * with the product owner's explicit MVP decisions overriding all three.
 * Where they conflict, the conflict is named in st_led_mvp.c at the exact
 * decision it affects rather than quietly resolved.
 *
 * The track-LED vocabulary is the contract's own words, not a new
 * language: firmware-contract-v1.json's `stem.solo` row specifies
 * "soloed stem solid, non-solo stems faint", its heads mute row specifies
 * "muted head faint", and its stem-select rows specify "active stem LED
 * brightens". So: SOLID = audible, GHOST ("faint") = loaded but silent.
 * There is no third track state in the approved MVP contract.
 *
 * NOT IN THIS MODULE, deliberately: error codes, storage-error animation,
 * diagnostic animation during ordinary use, song bank/position indication,
 * a standby chase, heads, FX, cues, recording and overdub. Each is either
 * explicitly excluded by the product owner or out of scope for this task.
 */

#ifndef ST_LED_MVP_H_
#define ST_LED_MVP_H_

#include <stdbool.h>
#include <stdint.h>

#define ST_LED_TRACK_COUNT 4u
#define ST_LED_SIDE_COUNT  4u
#define ST_LED_COUNT       (ST_LED_TRACK_COUNT + ST_LED_SIDE_COUNT)

/* Frame indices. 0..3 are the four Track LEDs above buttons 1..4; 4..7 are
 * the side row. These match docs/stem-tape-led-feedback-v1.md section 1's
 * authoritative table AND main.c's own pinned GPIO arrays:
 *
 *   index 0..3 -> track_leds[0..3] = P0.29, P0.26, P1.15, P1.14
 *   index 4..7 -> leds[0..3]       = P1.13, P0.00, P1.12, P0.01
 *
 * DIRECTION CAVEAT, carried from the LED protocol doc rather than silently
 * dropped: that document states the PLAY-end/FUNCTION-end direction of the
 * side row is NOT confirmed -- two community sources number the same four
 * GPIOs in opposite orders. Index 4 is this firmware's best-effort
 * inference for "nearest PLAY". If the physical device shows the transport
 * light at the FUNCTION end instead, the fix is to change
 * ST_LED_SIDE_TRANSPORT to 7 and reverse the battery order; nothing else
 * in this module depends on the direction. */
#define ST_LED_TRACK_FIRST      0u
#define ST_LED_SIDE_FIRST       4u
#define ST_LED_SIDE_TRANSPORT   4u   /* side LED nearest PLAY: transport only */
#define ST_LED_SIDE_BATT_FIRST  5u   /* the remaining three: battery only */
#define ST_LED_SIDE_BATT_COUNT  3u

/*
 * The physical vocabulary the existing TIMER3 soft-PWM driver can express,
 * and nothing beyond it -- this enum is deliberately not richer than the
 * hardware, so a semantic decision can never ask for a level the renderer
 * would have to approximate.
 *
 *   OFF   : pin low every frame.
 *   GHOST : lit one frame in five at the same proven 52 us window -> about
 *           1/5 of dim. This is the contract's "faint". It exists so a
 *           muted-but-loaded stem is distinguishable from an empty one.
 *   SOLID : lit every frame at the dim window. The contract's "solid".
 */
typedef enum {
	ST_LED_OFF = 0,
	ST_LED_GHOST,
	ST_LED_SOLID,
} st_led_mode_t;

/* ---- battery, ported from the approved local gauge ---------------------
 * Six states, and the two safety rules that matter: an unavailable or
 * failed reading is NEVER "low", and a level that was never seeded is
 * NEVER rendered as a specific charge. Constants are copied verbatim from
 * firmware/stemtape/src/led_battery.h, which took them from the pinned
 * Tape Looper source, which itself labels them PLACEHOLDERS until
 * calibrated. That provisional status is inherited unchanged here -- these
 * are not a final hardware calibration and must not be presented as one. */
#define ST_LED_BATT_THR_1  2020u  /* level 1->2 */
#define ST_LED_BATT_THR_2  2140u  /* level 2->3 */
#define ST_LED_BATT_THR_3  2260u  /* level 3->4 */
#define ST_LED_BATT_HYSTERESIS_COUNTS 18u
#define ST_LED_BATT_EMA_SHIFT          3u   /* ema += (raw - ema) / 8 */
#define ST_LED_BATT_LEVEL_COUNT        4u
#define ST_LED_BATT_LOW_LEVEL          1u   /* level 1 == at/below the low threshold */

typedef enum {
	ST_LED_BATT_UNAVAILABLE = 0,  /* no valid sample has ever landed. Never low. */
	ST_LED_BATT_FAULT,            /* read failed after a prior good one, or an
				       * impossible charger pin combination. Never low. */
	ST_LED_BATT_CHARGER_ABSENT,   /* on battery, valid, above the low threshold */
	ST_LED_BATT_CHARGING,         /* charger present and actively charging */
	ST_LED_BATT_CHARGE_COMPLETE,  /* charger present, charging finished */
	ST_LED_BATT_LOW,              /* on battery, valid, at/below the low threshold */
} st_led_batt_state_t;

typedef struct {
	int32_t ema;          /* smoothed raw ADC code; -1 == never seeded */
	uint8_t level;        /* sticky 1..4; 0 == never seeded (no fabricated reading) */
	bool    ever_valid;   /* has any valid sample ever landed? */
	bool    last_read_ok; /* did the MOST RECENT read attempt succeed? */
} st_led_batt_gauge_t;

void st_led_batt_reset(st_led_batt_gauge_t *g);

/* Folds in one raw ADC sample. When `valid` is false the EMA and level are
 * left exactly as they were -- sticky, so one missed sample never blanks
 * the display -- and only last_read_ok records the miss. */
void st_led_batt_update(st_led_batt_gauge_t *g, bool valid, int32_t raw_adc);

/* `charger_present` == nPGOOD asserted, `charging_now` == nCHG asserted.
 * Both pins are open-drain active-LOW on the BQ24232; main.c does that
 * inversion at the pin and passes plain booleans. */
st_led_batt_state_t st_led_batt_classify(const st_led_batt_gauge_t *g,
					  bool charger_present, bool charging_now);

/* ---- the inputs the decision is a pure function of ---------------------
 * Every field is real runtime state read from the production firmware.
 * Nothing here is a mode flag chosen by the LED layer for its own
 * convenience. */
typedef struct {
	/* Transport / selection. */
	bool song_selected;   /* a valid Stem Tape song is loaded and playable */
	bool playing;         /* transport is running */

	/* Temporary overlay. While true the four Track LEDs blink together and
	 * NOTHING else about the underlying state is forgotten -- when it goes
	 * false the frame is recomputed from live state, never restored from a
	 * snapshot taken when the transfer began. */
	bool transfer_active;
	bool transfer_blink_on;   /* phase supplied by the caller */

	/* Per stem, index 0..3 == Track 1..4 == Vocals/Drums/Bass/Instruments. */
	bool stem_loaded[ST_LED_TRACK_COUNT];   /* content exists in the song */
	bool stem_audible[ST_LED_TRACK_COUNT];  /* the mixer's OWN audibility rule */
	bool solo_active;                       /* any track button held for solo */
	bool stem_soloed[ST_LED_TRACK_COUNT];   /* this stem is one of the soloed ones */

	/* Battery. */
	st_led_batt_state_t batt_state;
	uint8_t             batt_level;     /* 1..4; 0 == never seeded */
	bool                batt_blink_on;  /* ~1 Hz phase, supplied by the caller */
} st_led_inputs_t;

typedef struct {
	uint8_t mode[ST_LED_COUNT];   /* st_led_mode_t per LED */
} st_led_frame_t;

/*
 * THE decision. Pure, total, and deterministic: the same inputs always
 * produce the same eight modes, with no internal state and no clock of its
 * own (both blink phases are inputs). Every LED is always assigned, so a
 * frame can never carry a stale value from a previous call.
 */
void st_led_mvp_decide(const st_led_inputs_t *in, st_led_frame_t *out);

#endif /* ST_LED_MVP_H_ */
