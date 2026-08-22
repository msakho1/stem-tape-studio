/*
 * led_battery.c — see led_battery.h. PURE: no Zephyr, no ADC/GPIO access.
 */

#include "led_battery.h"

#include "led_duty.h"

/* [looper a8dd127:4614]: static const int batt_thr[3] = { 2020, 2140, 2260 }; */
static const int32_t batt_thr[3] = {
	(int32_t)LED_BATTERY_THR_1,
	(int32_t)LED_BATTERY_THR_2,
	(int32_t)LED_BATTERY_THR_3,
};

void led_battery_gauge_reset(led_battery_gauge_t *g)
{
	g->ema = -1;
	g->level = 0u;
	g->ever_valid = false;
	g->last_read_ok = false;
}

void led_battery_gauge_update(led_battery_gauge_t *g, bool valid, int32_t raw_adc)
{
	uint8_t nl;
	int k;

	g->last_read_ok = valid;
	if (!valid) {
		return; /* sticky: EMA/level unchanged on a failed read */
	}
	g->ever_valid = true;

	/* [looper a8dd127:4618-4620]: bavg = (bavg<0) ? braw : bavg + (braw-bavg)/8
	 * Integer DIVISION (truncates toward zero), not a right-shift (which
	 * would truncate toward negative infinity for a negative numerator) —
	 * matched exactly here rather than approximated, since raw_adc - ema
	 * can be negative on a falling reading. */
	g->ema = (g->ema < 0) ? raw_adc
			      : g->ema + (raw_adc - g->ema) / (1 << LED_BATTERY_EMA_SHIFT);

	/* [looper a8dd127:4630-4632]: for k in 0..2, if bavg > batt_thr[k], nl = k+2 */
	nl = 1u;
	for (k = 0; k < 3; k++) {
		if (g->ema > batt_thr[k]) {
			nl = (uint8_t)(k + 2);
		}
	}

	if (g->level == 0u) {
		g->level = nl; /* first valid sample seeds the sticky level directly */
	} else if (nl > g->level &&
		   g->ema > batt_thr[g->level - 1u] + (int32_t)LED_BATTERY_HYSTERESIS_COUNTS) {
		/* [looper a8dd127:4633-4635] */
		g->level = nl;
	} else if (nl < g->level &&
		   g->ema < batt_thr[g->level - 2u] - (int32_t)LED_BATTERY_HYSTERESIS_COUNTS) {
		/* [looper a8dd127:4636-4637] */
		g->level = nl;
	}
}

led_battery_state_t led_battery_classify(const led_battery_gauge_t *g,
					  bool charger_present, bool charging_now)
{
	/* Charger-status FAULT: nCHG asserted while nPGOOD is deasserted is a
	 * combination the BQ24232 should never produce (charging requires
	 * input power present) — flag it distinctly and never as "low". */
	if (charging_now && !charger_present) {
		return LED_BATTERY_FAULT;
	}
	if (!g->ever_valid) {
		return LED_BATTERY_UNAVAILABLE;
	}
	if (!g->last_read_ok) {
		return LED_BATTERY_FAULT;
	}
	if (charger_present) {
		return charging_now ? LED_BATTERY_CHARGING : LED_BATTERY_CHARGE_COMPLETE;
	}
	return (g->level <= LED_BATTERY_LOW_LEVEL) ? LED_BATTERY_LOW : LED_BATTERY_CHARGER_ABSENT;
}

bool led_battery_state_is_low(led_battery_state_t state)
{
	return state == LED_BATTERY_LOW;
}

void led_battery_gauge_frame(const led_battery_gauge_t *g, bool charging_now,
			      bool blink_phase, uint8_t out[LED_PHYSICAL_COUNT])
{
	uint8_t i;

	for (i = 0; i < LED_TRACK_ROW_COUNT; i++) {
		out[i] = 0u;
	}
	for (i = LED_TRACK_ROW_COUNT; i < LED_PHYSICAL_COUNT; i++) {
		uint8_t step = led_channel_table[i].gauge_step; /* 0..3, bottom-to-top */
		bool lit;

		if (g->level == 0u) {
			lit = false; /* never seeded: off, not a fabricated level */
		} else if ((uint8_t)(step + 1u) < g->level) {
			lit = true; /* strictly below the current level: solid */
		} else if ((uint8_t)(step + 1u) == g->level) {
			/* [looper a8dd127:4644]: on = charging() ? bl : 1 */
			lit = charging_now ? blink_phase : true;
		} else {
			lit = false;
		}
		out[i] = lit ? (uint8_t)LED_LEVEL_MAX : 0u;
	}
}
