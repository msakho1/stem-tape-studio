/*
 * led_render.c — see led_render.h.
 *
 * PWM2 (Track row, channels 0..3) / PWM3 (side row, channels 0..3),
 * 1024 us period, active-high — the proven nRF52840 mapping used by
 * bnjreece/feldd-sp1-firmware's src/led.c + app.overlay (MIT-licensed;
 * hardware-tested on this same board), adapted minimally: M0 keeps its own
 * lower per-row microsecond ceilings (LED_TRACK_MAX_PULSE_US /
 * LED_SIDE_MAX_PULSE_US) instead of feldd's 0..100% global brightness
 * scale, and channels are driven directly rather than through an on/off +
 * brightness abstraction. The eight `pwm-leds` child node labels below are
 * declared in app.overlay; see led_protocol.h for the index -> GPIO table
 * both mirror. The devicetree node order is fixed by the proven electrical
 * wiring (PWM channel <-> GPIO), NOT by protocol index — led_pwm[] below
 * maps protocol index -> node explicitly so the two can differ (they do,
 * for the side row: see led_protocol.h's physical-inventory comment).
 */

#include "led_render.h"

#include <stdbool.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/pwm/pwm.h>
#include <zephyr/drivers/pwm.h>

#include "led_duty.h"

/* Index -> devicetree node, by GPIO (see led_protocol.h's table). Node
 * labels name the fixed electrical PWM channel; the array position is the
 * protocol index. */
static const struct pwm_dt_spec led_pwm[LED_PHYSICAL_COUNT] = {
	PWM_DT_SPEC_GET(DT_NODELABEL(stemtape_led_track1)),        /* idx 0: P0.29, PWM2 ch0 */
	PWM_DT_SPEC_GET(DT_NODELABEL(stemtape_led_track2)),        /* idx 1: P0.26, PWM2 ch1 */
	PWM_DT_SPEC_GET(DT_NODELABEL(stemtape_led_track3)),        /* idx 2: P1.15, PWM2 ch2 */
	PWM_DT_SPEC_GET(DT_NODELABEL(stemtape_led_track4)),        /* idx 3: P1.14, PWM2 ch3 */
	PWM_DT_SPEC_GET(DT_NODELABEL(stemtape_led_side_play)),     /* idx 4: P1.13, PWM3 ch3 */
	PWM_DT_SPEC_GET(DT_NODELABEL(stemtape_led_side_mid1)),     /* idx 5: P0.00, PWM3 ch2 */
	PWM_DT_SPEC_GET(DT_NODELABEL(stemtape_led_side_mid2)),     /* idx 6: P1.12, PWM3 ch1 */
	PWM_DT_SPEC_GET(DT_NODELABEL(stemtape_led_side_function)), /* idx 7: P0.01, PWM3 ch0 */
};

static bool ready;
static uint32_t error_count;
static uint8_t cached_levels[LED_PHYSICAL_COUNT];
static bool cache_primed;

int led_render_init(void)
{
	uint8_t i;
	uint8_t off[LED_PHYSICAL_COUNT] = { 0 };
	bool all_channels_ready = true;

	for (i = 0; i < LED_PHYSICAL_COUNT; i++) {
		if (!pwm_is_ready_dt(&led_pwm[i])) {
			all_channels_ready = false;
		}
	}
	/* Force the first apply() to write every channel regardless of the
	 * (zero-initialized, therefore possibly already-matching) cache. */
	cache_primed = false;
	ready = all_channels_ready;

	/* Prove every channel by writing an explicit duty-0, and require that
	 * proof to succeed too: "led_render_init() must fail if any
	 * device/channel is unavailable" now covers write failures, not just
	 * the readiness check. */
	if (ready && led_render_apply(off) != 0) {
		ready = false;
	}
	return ready ? 0 : -1;
}

bool led_render_is_ready(void)
{
	return ready;
}

uint32_t led_render_error_count(void)
{
	return error_count;
}

int led_render_apply(const uint8_t levels[LED_PHYSICAL_COUNT])
{
	uint8_t i;
	bool changed[LED_PHYSICAL_COUNT];
	int failures = 0;

	if (!ready) {
		return -1;
	}

	if (!cache_primed) {
		/* First call after init: the cache has no meaning yet, so
		 * force every channel to be treated as changed. */
		for (i = 0; i < LED_PHYSICAL_COUNT; i++) {
			changed[i] = true;
			cached_levels[i] = levels[i];
		}
		cache_primed = true;
	} else if (!led_duty_diff_frame(cached_levels, levels, changed)) {
		return 0; /* nothing changed: zero PWM writes this call */
	}

	for (i = 0; i < LED_PHYSICAL_COUNT; i++) {
		int rc;

		if (!changed[i]) {
			continue; /* "do not resend... when nothing changed" */
		}
		rc = pwm_set_pulse_dt(&led_pwm[i], PWM_USEC(led_level_to_pulse_us(i, levels[i])));
		if (rc != 0) {
			failures++;
			error_count++;
		}
	}
	return failures ? -failures : 0;
}
