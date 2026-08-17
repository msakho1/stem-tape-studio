/*
 * led_duty.c — see led_duty.h. PURE: no Zephyr, no PWM driver calls.
 */

#include "led_duty.h"

/* Must match led_protocol.h's table and app.overlay's pwm-leds child order,
 * index for index. */
const led_physical_pin_t led_physical_pin_map[LED_PHYSICAL_COUNT] = {
	[LED_IDX_TRACK1]    = { 0u, 29u }, /* P0.29 */
	[LED_IDX_TRACK2]    = { 0u, 26u }, /* P0.26 */
	[LED_IDX_TRACK3]    = { 1u, 15u }, /* P1.15 */
	[LED_IDX_TRACK4]    = { 1u, 14u }, /* P1.14 */
	[LED_IDX_PLAYBACK1] = { 0u,  1u }, /* P0.01 */
	[LED_IDX_PLAYBACK2] = { 1u, 12u }, /* P1.12 */
	[LED_IDX_PLAYBACK3] = { 0u,  0u }, /* P0.00 */
	[LED_IDX_PLAYBACK4] = { 1u, 13u }, /* P1.13 */
};

uint32_t led_row_max_pulse_us(uint8_t index)
{
	if (index >= LED_PHYSICAL_COUNT) {
		return 0u;
	}
	return (index < LED_TRACK_ROW_COUNT) ? LED_TRACK_MAX_PULSE_US
					      : LED_PLAYBACK_MAX_PULSE_US;
}

uint32_t led_level_to_pulse_us(uint8_t index, uint8_t level)
{
	uint32_t row_max = led_row_max_pulse_us(index);

	if (level > LED_LEVEL_MAX) {
		level = LED_LEVEL_MAX; /* clamp before touching hardware */
	}
	if (level == 0u) {
		return 0u; /* truly off, no rounding artifact possible */
	}
	/* Round-to-nearest linear scale; level == LED_LEVEL_MAX collapses to
	 * exactly row_max (LED_LEVEL_MAX * row_max + LED_LEVEL_MAX/2) /
	 * LED_LEVEL_MAX == row_max, since the remainder term is always
	 * < LED_LEVEL_MAX. */
	return ((uint32_t)level * row_max + (LED_LEVEL_MAX / 2u)) / LED_LEVEL_MAX;
}
