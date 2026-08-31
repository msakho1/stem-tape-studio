/*
 * led_battery.h — Stem Tape M0 local battery/charging LED gauge.
 *
 * PURE: no Zephyr, no ADC/GPIO access. main.c reads the real BQ24232 charger
 * status pins and the AIN4 battery-divider ADC and passes the results in
 * here; this module never touches hardware directly.
 *
 * This ports the SP-1 Tape Looper's OWN documented standby charging gauge
 * [looper a8dd127: charger_init()/usb_present()/charging() lines 4260-4290,
 * gauge lines 4591-4647] rather than inventing a new one: 1..4 quarter-level
 * LEDs solid below the current level, the current level's LED SOLID when not
 * charging and BLINKING while charging, all four solid once the charge cycle
 * completes. The battery threshold constants below are copied verbatim from
 * that source, which itself labels them "PLACEHOLDERS until calibrated" /
 * "Interim calibration" — M0 inherits that same provisional status
 * unchanged; do not treat them as a final hardware calibration. See
 * docs/stem-tape-led-feedback-v1.md "Battery / charging baseline".
 *
 * SAFETY: an unavailable or failed ADC reading, or a contradictory
 * charger-status reading, must never be classified as an empty/low battery
 * and must never suppress a valid host LED frame — see
 * led_battery_state_is_low() and led_battery_classify().
 */

#ifndef STEMTAPE_LED_BATTERY_H_
#define STEMTAPE_LED_BATTERY_H_

#include <stdbool.h>
#include <stdint.h>

#include "led_protocol.h"

/* ---- provisional calibration constants [looper a8dd127:4602-4606,4614] ---
 * RAW 12-bit AIN4 battery-divider codes (gain 1/6, 0.6 V internal ref).
 * "Interim calibration 2026-07-20: full anchor MEASURED at raw ~2380
 * (resting, plugged-not-charging = ~4.21 V); empty end is a ~3.35 V physics
 * estimate pending a real low reading" — quoted verbatim from the pinned
 * source, which already flags these as provisional. NOT re-derived or
 * guessed for M0; if you need a firmer calibration, re-run the SP-1 bench
 * measurement the looper's comment describes and update both sources
 * together. */
#define LED_BATTERY_THR_1  2020u  /* [looper a8dd127:4614] batt_thr[0]: level 1->2 */
#define LED_BATTERY_THR_2  2140u  /* [looper a8dd127:4614] batt_thr[1]: level 2->3 */
#define LED_BATTERY_THR_3  2260u  /* [looper a8dd127:4614] batt_thr[2]: level 3->4 */
#define LED_BATTERY_HYSTERESIS_COUNTS  18u  /* [looper a8dd127:4634,4636] */
#define LED_BATTERY_EMA_SHIFT          3u   /* [looper a8dd127:4620]: bavg += (raw-bavg)/8 */

/*
 * Every state the local gauge can be in. Exactly six are host-visible safety
 * states plus the always-present UNAVAILABLE start state:
 *   - UNAVAILABLE: no valid ADC sample has EVER been folded in (e.g. still
 *     early after boot). Never "low".
 *   - FAULT: either (a) the most recent ADC read attempt failed after at
 *     least one prior valid sample, or (b) the charger status pins report a
 *     combination the BQ24232 should never produce (nCHG asserted while
 *     nPGOOD is deasserted — charging without power present). Never "low".
 *   - CHARGER_ABSENT: on battery, valid reading, ABOVE the low threshold —
 *     ordinary operation.
 *   - CHARGING: charger present (nPGOOD asserted) and actively charging
 *     (nCHG asserted).
 *   - CHARGE_COMPLETE: charger present, not actively charging (charge cycle
 *     finished, or the charger has otherwise stopped without a fault
 *     indication).
 *   - LOW: on battery, valid reading, AT/BELOW the low threshold — the ONLY
 *     state that may preempt an owned host frame.
 */
typedef enum {
	LED_BATTERY_UNAVAILABLE = 0,
	LED_BATTERY_FAULT,
	LED_BATTERY_CHARGER_ABSENT,
	LED_BATTERY_CHARGING,
	LED_BATTERY_CHARGE_COMPLETE,
	LED_BATTERY_LOW,
} led_battery_state_t;

/* Sticky smoothed reading + gauge level. Owned by main.c (like
 * led_frame_state_t), fed one ADC sample at a time. */
typedef struct {
	int32_t ema;          /* smoothed raw ADC code; -1 == never seeded */
	uint8_t level;         /* sticky gauge level 1..LED_BATTERY_GAUGE_LEVEL_COUNT;
				 * 0 == never seeded (no fabricated reading) */
	bool    ever_valid;    /* has at least one valid ADC sample ever landed? */
	bool    last_read_ok;  /* did the MOST RECENT read attempt succeed? */
} led_battery_gauge_t;

/* Cold-boot / reinit state: no reading has ever landed. */
void led_battery_gauge_reset(led_battery_gauge_t *g);

/*
 * Folds in one raw ADC sample. `valid` is false when the read itself failed
 * (main.c's ladder_read() returned < 0): the EMA and level are left exactly
 * as they were — sticky, exactly mirroring the pinned gauge's
 * `if (braw >= 0) bavg = ...` [looper a8dd127:4618-4620] — so a single
 * missed sample never blanks or resets the display, and `last_read_ok`
 * alone (not the level) reflects the miss for FAULT classification.
 */
void led_battery_gauge_update(led_battery_gauge_t *g, bool valid, int32_t raw_adc);

/*
 * Classifies the overall state from the gauge plus the two charger status
 * GPIOs (already-read booleans; main.c reads nPGOOD/nCHG directly, both
 * LOW = active [looper a8dd127:129-130,4282-4290]). `charger_present` ==
 * nPGOOD asserted; `charging_now` == nCHG asserted (meaningful only when
 * paired with `charger_present`; see the FAULT case above).
 */
led_battery_state_t led_battery_classify(const led_battery_gauge_t *g,
					  bool charger_present, bool charging_now);

/* True ONLY for LED_BATTERY_LOW — the sole state allowed to preempt an owned
 * host frame (led_render_select()'s `low_battery` argument). UNAVAILABLE,
 * FAULT, CHARGER_ABSENT, CHARGING and CHARGE_COMPLETE all return false. */
bool led_battery_state_is_low(led_battery_state_t state);

/*
 * Fills the local gauge frame: Track row (0-3) always 0. Side row (4-7)
 * shows `g->level` (1..LED_BATTERY_GAUGE_LEVEL_COUNT) LEDs solid, ascending
 * from LED_IDX_SIDE_PLAY toward LED_IDX_SIDE_FUNCTION per led_channel_table's
 * gauge_step (led_duty.h) — the SAME order used everywhere else, not a
 * separate assumption. The current (topmost lit) level's LED is solid
 * UNLESS `charging_now`, in which case it follows `blink_phase` instead —
 * "solid quarter-level LEDs, the next level blinking while charging, and all
 * four solid when charging is complete" [looper a8dd127:4639-4647]. When the
 * gauge has never been seeded (g->level == 0), the entire side row is left
 * OFF — never fabricated as a specific charge level.
 */
void led_battery_gauge_frame(const led_battery_gauge_t *g, bool charging_now,
			      bool blink_phase, uint8_t out[LED_PHYSICAL_COUNT]);

#endif /* STEMTAPE_LED_BATTERY_H_ */
