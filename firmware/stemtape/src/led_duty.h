/*
 * led_duty.h — Stem Tape LED Feedback Protocol v1: physical index/pin table
 * and level-to-duty math.
 *
 * PURE: no Zephyr, no PWM driver calls. This is the documented, host-tested
 * mirror of the index map in led_protocol.h and app.overlay's `pwm-leds`
 * child order; led_render.c's devicetree-backed PWM spec array must list the
 * same 8 GPIOs in the same order (checked by code review + this table, since
 * a host test cannot reach into a compiled devicetree).
 */

#ifndef STEMTAPE_LED_DUTY_H_
#define STEMTAPE_LED_DUTY_H_

#include <stdint.h>

#include "led_protocol.h"

typedef struct {
	uint8_t port; /* 0 = P0, 1 = P1 */
	uint8_t pin;
} led_physical_pin_t;

/* Indexed 0..7, exactly the led_protocol.h table. */
extern const led_physical_pin_t led_physical_pin_map[LED_PHYSICAL_COUNT];

/* Row ceiling for a physical index: LED_TRACK_MAX_PULSE_US for 0..3,
 * LED_PLAYBACK_MAX_PULSE_US for 4..7. An out-of-range index returns 0 (no
 * physical index is out of range in practice; the caller must not use this
 * to bypass validation). */
uint32_t led_row_max_pulse_us(uint8_t index);

/* Level 0..127 mapped linearly to 0..row-max-pulse for `index`'s row.
 * `level` is clamped to LED_LEVEL_MAX before any math ("clamp before
 * touching hardware"). level == 0 always returns exactly 0 ("0 must produce
 * a truly off output"); level == LED_LEVEL_MAX always returns exactly the
 * row's ceiling. */
uint32_t led_level_to_pulse_us(uint8_t index, uint8_t level);

#endif /* STEMTAPE_LED_DUTY_H_ */
