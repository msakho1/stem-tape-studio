/*
 * led_battery.c — see led_battery.h. PURE: no Zephyr, no ADC access.
 */

#include "led_battery.h"

uint8_t led_battery_step(uint8_t battery_cc_value)
{
	uint32_t step = ((uint32_t)battery_cc_value * (LED_BATTERY_STEP_COUNT + 1u))
			 / (LED_LEVEL_MAX + 1u);

	if (step > LED_BATTERY_STEP_COUNT) {
		step = LED_BATTERY_STEP_COUNT; /* defensive; 127 already yields exactly 4 */
	}
	return (uint8_t)step;
}

bool led_battery_is_low(uint8_t battery_cc_value)
{
	return led_battery_step(battery_cc_value) == LED_BATTERY_LOW_STEP;
}

void led_battery_frame(uint8_t battery_cc_value, uint8_t out[LED_PHYSICAL_COUNT])
{
	uint8_t step = led_battery_step(battery_cc_value);
	uint8_t i;

	for (i = 0; i < LED_TRACK_ROW_COUNT; i++) {
		out[i] = 0u; /* Track row: unchanged local-idle behavior */
	}
	for (i = 0; i < LED_SIDE_ROW_COUNT; i++) {
		uint8_t idx = (uint8_t)(LED_TRACK_ROW_COUNT + i);

		out[idx] = (i < step) ? (uint8_t)LED_LEVEL_MAX : 0u;
	}
}
