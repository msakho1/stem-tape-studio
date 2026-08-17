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
 * declared in app.overlay; led_duty.h's led_channel_table[] is the
 * authoritative index -> GPIO -> PWM instance/channel table both mirror.
 * The devicetree node order is fixed by the proven electrical wiring (PWM
 * channel <-> GPIO), NOT by protocol index — led_pwm[] below maps protocol
 * index -> node explicitly so the two can differ (they do, for the side
 * row: see led_protocol.h's physical-inventory comment). led_render_init()
 * cross-checks every entry's compiled `.channel` against
 * led_channel_table[i].pwm_channel at runtime, so this array cannot
 * silently drift from the authoritative table without the renderer
 * refusing to come up ready.
 *
 * All write/retry/fault-latch POLICY lives in led_render_policy.c (PURE,
 * host-tested); this file only supplies the one function that actually
 * touches the PWM driver.
 */

#include "led_render.h"

#include <stdbool.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/pwm/pwm.h>
#include <zephyr/drivers/pwm.h>

#include "led_duty.h"
#include "led_render_policy.h"

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

static led_render_policy_t g_policy;

static int hw_write(uint8_t index, uint32_t pulse_us, void *ctx)
{
	ARG_UNUSED(ctx);
	return pwm_set_pulse_dt(&led_pwm[index], PWM_USEC(pulse_us));
}

static int bring_up(void)
{
	uint8_t i;
	bool all_ready = true;

	for (i = 0; i < LED_PHYSICAL_COUNT; i++) {
		if (!pwm_is_ready_dt(&led_pwm[i])) {
			all_ready = false;
			continue;
		}
		/* Cross-check against the ONE authoritative table (led_duty.h):
		 * this devicetree spec's compiled channel must match
		 * led_channel_table[i].pwm_channel exactly, or the wiring
		 * documented there has drifted from what app.overlay actually
		 * declares — fail closed rather than drive the wrong channel. */
		if (led_pwm[i].channel != led_channel_table[i].pwm_channel) {
			all_ready = false;
		}
	}
	if (!all_ready) {
		return -1;
	}
	return led_render_policy_bringup(&g_policy, hw_write, NULL) ? 0 : -1;
}

int led_render_init(void)
{
	led_render_policy_init(&g_policy);
	return bring_up();
}

int led_render_reinit(void)
{
	return bring_up();
}

bool led_render_is_ready(void)
{
	return led_render_policy_is_ready(&g_policy);
}

uint32_t led_render_error_count(void)
{
	return led_render_policy_error_count(&g_policy);
}

void led_render_last_failure(uint8_t *index, int *rc)
{
	led_render_policy_last_failure(&g_policy, index, rc);
}

int led_render_apply(const uint8_t levels[LED_PHYSICAL_COUNT])
{
	return led_render_policy_apply(&g_policy, levels, hw_write, NULL);
}
