/*
 * led_duty.h — Stem Tape LED Feedback Protocol v1: physical index/pin table,
 * level-to-duty math, and unchanged-frame diffing.
 *
 * PURE: no Zephyr, no PWM driver calls. This is the documented, host-tested
 * mirror of the index map in led_protocol.h and app.overlay's `pwm-leds`
 * child order; led_render.c's devicetree-backed PWM spec array must list the
 * same 8 GPIOs in the same order (checked by code review + this table, since
 * a host test cannot reach into a compiled devicetree).
 */

#ifndef STEMTAPE_LED_DUTY_H_
#define STEMTAPE_LED_DUTY_H_

#include <stdbool.h>
#include <stdint.h>

#include "led_protocol.h"

typedef struct {
	uint8_t port; /* 0 = P0, 1 = P1 */
	uint8_t pin;
} led_physical_pin_t;

/* Indexed 0..7, exactly the led_protocol.h table. */
extern const led_physical_pin_t led_physical_pin_map[LED_PHYSICAL_COUNT];

/* Row ceiling for a physical index: LED_TRACK_MAX_PULSE_US for 0..3,
 * LED_SIDE_MAX_PULSE_US for 4..7. An out-of-range index returns 0 (no
 * physical index is out of range in practice; the caller must not use this
 * to bypass validation). */
uint32_t led_row_max_pulse_us(uint8_t index);

/* Level 0..127 mapped linearly to 0..row-max-pulse for `index`'s row.
 * `level` is clamped to LED_LEVEL_MAX before any math ("clamp before
 * touching hardware"). level == 0 always returns exactly 0 ("0 must produce
 * a truly off output"); level == LED_LEVEL_MAX always returns exactly the
 * row's ceiling. */
uint32_t led_level_to_pulse_us(uint8_t index, uint8_t level);

/*
 * Per-channel diff against a cache, used to skip redundant PWM writes: for
 * each index i, changed[i] = (new_levels[i] != cached[i]); cached[i] is then
 * updated to new_levels[i] whenever it changed (cached[i] is left alone when
 * unchanged, which is a no-op since they were already equal). Returns true
 * if at least one channel changed (false = nothing to write at all).
 *
 * PURE: two plain 8-byte buffers in, a changed-mask buffer out. led_render.c
 * uses this to decide which of the 8 pwm_set_pulse_dt() calls to actually
 * issue on a given render; it never sends physical writes for channels that
 * did not change, which is the extent of what "not resending every 5 ms"
 * means here. It is NOT a claim that the channels that DO get written
 * update the physical outputs simultaneously — see led_render.h.
 */
bool led_duty_diff_frame(uint8_t cached[LED_PHYSICAL_COUNT],
			  const uint8_t new_levels[LED_PHYSICAL_COUNT],
			  bool changed[LED_PHYSICAL_COUNT]);

#endif /* STEMTAPE_LED_DUTY_H_ */
