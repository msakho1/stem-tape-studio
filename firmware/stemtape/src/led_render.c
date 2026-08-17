/*
 * led_render.c — see led_render.h.
 *
 * PWM2 (track row, channels 0..3) / PWM3 (playback row, channels 0..3),
 * 1024 us period, active-high — the proven nRF52840 mapping used by
 * bnjreece/feldd-sp1-firmware's src/led.c + app.overlay (MIT-licensed;
 * hardware-tested on this same board), adapted minimally: M0 keeps its own
 * lower per-row microsecond ceilings (LED_TRACK_MAX_PULSE_US /
 * LED_PLAYBACK_MAX_PULSE_US) instead of feldd's 0..100% global brightness
 * scale, and channels are driven directly rather than through an on/off +
 * brightness abstraction. The eight `pwm-leds` child node labels below are
 * declared in app.overlay in this same order; see led_protocol.h for the
 * index -> GPIO table both mirror.
 */

#include "led_render.h"

#include <stdbool.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/pwm/pwm.h>
#include <zephyr/drivers/pwm.h>

#include "led_duty.h"

static const struct pwm_dt_spec led_pwm[LED_PHYSICAL_COUNT] = {
	PWM_DT_SPEC_GET(DT_NODELABEL(stemtape_led_t1)), /* idx 0: Track 1, P0.29 */
	PWM_DT_SPEC_GET(DT_NODELABEL(stemtape_led_t2)), /* idx 1: Track 2, P0.26 */
	PWM_DT_SPEC_GET(DT_NODELABEL(stemtape_led_t3)), /* idx 2: Track 3, P1.15 */
	PWM_DT_SPEC_GET(DT_NODELABEL(stemtape_led_t4)), /* idx 3: Track 4, P1.14 */
	PWM_DT_SPEC_GET(DT_NODELABEL(stemtape_led_p1)), /* idx 4: Playback 1, P0.01 */
	PWM_DT_SPEC_GET(DT_NODELABEL(stemtape_led_p2)), /* idx 5: Playback 2, P1.12 */
	PWM_DT_SPEC_GET(DT_NODELABEL(stemtape_led_p3)), /* idx 6: Playback 3, P0.00 */
	PWM_DT_SPEC_GET(DT_NODELABEL(stemtape_led_p4)), /* idx 7: Playback 4, P1.13 */
};

static bool ready;

int led_render_init(void)
{
	uint8_t i;
	uint8_t off[LED_PHYSICAL_COUNT] = { 0 };

	ready = true;
	for (i = 0; i < LED_PHYSICAL_COUNT; i++) {
		if (!pwm_is_ready_dt(&led_pwm[i])) {
			ready = false;
		}
	}
	/* All-or-nothing readiness gate, matching feldd's led_init(): if every
	 * channel is ready, prove it by writing an explicit duty-0 to each one;
	 * if any single channel is not, led_render_apply() below no-ops
	 * entirely rather than partially drive a device in an unverified
	 * state. */
	led_render_apply(off);
	return ready ? 0 : -1;
}

void led_render_apply(const uint8_t levels[LED_PHYSICAL_COUNT])
{
	uint8_t i;

	if (!ready) {
		return;
	}
	for (i = 0; i < LED_PHYSICAL_COUNT; i++) {
		uint32_t pulse = led_level_to_pulse_us(i, levels[i]);

		(void)pwm_set_pulse_dt(&led_pwm[i], PWM_USEC(pulse));
	}
}
