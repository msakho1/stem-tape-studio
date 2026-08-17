/*
 * led_battery.h — Stem Tape M0 local battery/Play LED baseline.
 *
 * PURE: no Zephyr, no ADC access. main.c computes the same 0..127 battery
 * reading it already sends as MIDI CC24 (ladder_read() >> 5, clamped) and
 * passes it in here; this module never touches hardware directly.
 *
 * This is the device's STOCK local behavior, always available with no host
 * connected: the side row (indices 4-7, see led_protocol.h) is a 4-step
 * battery meter. The side LED nearest PLAY (LED_IDX_SIDE_PLAY) additionally
 * shows full brightness while a leased host frame reports playing — that
 * composition happens in main.c's render-source selection (led_frame.h's
 * led_render_select()), not here: this module only ever produces the
 * battery-only baseline. See docs/stem-tape-led-feedback-v1.md
 * "Battery / Play baseline" for the full composition rule.
 */

#ifndef STEMTAPE_LED_BATTERY_H_
#define STEMTAPE_LED_BATTERY_H_

#include <stdbool.h>
#include <stdint.h>

#include "led_protocol.h"

/*
 * Battery step 0..LED_BATTERY_STEP_COUNT from a 0..127 battery reading
 * (the same scale as MIDI CC24). Quintile split: step = value * (N+1) / 128,
 * clamped to N, so 0 -> step 0 (lowest) and 127 -> step 4 (full).
 */
uint8_t led_battery_step(uint8_t battery_cc_value);

/* True iff led_battery_step() would return LED_BATTERY_LOW_STEP: the bottom
 * step of the meter. UNMEASURED against real battery voltage — see
 * led_protocol.h. Safety precedence treats this as outranking a leased host
 * frame (led_render_select()'s `low_battery` argument). */
bool led_battery_is_low(uint8_t battery_cc_value);

/*
 * Fills the local battery baseline into a complete 8-value frame: indices
 * 0-3 (Track row) are always 0 (unchanged local-idle behavior); indices 4-7
 * (side row) light `led_battery_step(battery_cc_value)` of the 4 LEDs at
 * full brightness, filled ascending from LED_IDX_SIDE_PLAY toward
 * LED_IDX_SIDE_FUNCTION (an implementation choice, not a hardware-confirmed
 * visual convention — see led_protocol.h).
 */
void led_battery_frame(uint8_t battery_cc_value, uint8_t out[LED_PHYSICAL_COUNT]);

#endif /* STEMTAPE_LED_BATTERY_H_ */
