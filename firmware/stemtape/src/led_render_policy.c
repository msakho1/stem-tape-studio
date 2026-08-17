/*
 * led_render_policy.c — see led_render_policy.h. PURE: no Zephyr, no PWM
 * driver calls.
 */

#include "led_render_policy.h"

#include "led_duty.h"

void led_render_policy_init(led_render_policy_t *p)
{
	uint8_t i;

	p->ready = false;
	for (i = 0; i < LED_PHYSICAL_COUNT; i++) {
		p->cache_valid[i] = false;
		p->cached_level[i] = 0u;
		p->dirty[i] = false;
		p->fail_streak[i] = 0u;
	}
	p->error_count = 0u;
	p->last_fail_index = LED_PHYSICAL_COUNT;
	p->last_fail_rc = 0;
}

static void record_failure(led_render_policy_t *p, uint8_t index, int rc)
{
	p->cache_valid[index] = false;
	p->dirty[index] = true;
	p->fail_streak[index]++;
	p->error_count++;
	p->last_fail_index = index;
	p->last_fail_rc = rc;
	if (p->fail_streak[index] >= LED_RENDER_MAX_CONSECUTIVE_FAILS) {
		p->ready = false;
	}
}

static void record_success(led_render_policy_t *p, uint8_t index, uint8_t level)
{
	p->cache_valid[index] = true;
	p->cached_level[index] = level;
	p->dirty[index] = false;
	p->fail_streak[index] = 0u;
}

bool led_render_policy_bringup(led_render_policy_t *p, led_channel_write_fn write_fn, void *ctx)
{
	uint8_t i;

	for (i = 0; i < LED_PHYSICAL_COUNT; i++) {
		p->cache_valid[i] = false;
		p->cached_level[i] = 0u;
		p->dirty[i] = false;
		p->fail_streak[i] = 0u;
	}
	p->ready = true; /* only ever survives below if every channel proves out */

	for (i = 0; i < LED_PHYSICAL_COUNT; i++) {
		int rc = write_fn(i, led_level_to_pulse_us(i, 0u), ctx);

		if (rc == 0) {
			record_success(p, i, 0u);
		} else {
			p->ready = false;
			p->dirty[i] = true;
			p->fail_streak[i] = 1u;
			p->error_count++;
			p->last_fail_index = i;
			p->last_fail_rc = rc;
		}
	}
	return p->ready;
}

int led_render_policy_apply(led_render_policy_t *p, const uint8_t levels[LED_PHYSICAL_COUNT],
			     led_channel_write_fn write_fn, void *ctx)
{
	uint8_t i;
	int failures = 0;

	if (!p->ready) {
		return -1;
	}

	for (i = 0; i < LED_PHYSICAL_COUNT; i++) {
		bool already_good = p->cache_valid[i] && !p->dirty[i] &&
				     p->cached_level[i] == levels[i];
		int rc;

		if (already_good) {
			continue; /* "not unnecessarily rewritten" */
		}
		rc = write_fn(i, led_level_to_pulse_us(i, levels[i]), ctx);
		if (rc == 0) {
			record_success(p, i, levels[i]);
		} else {
			record_failure(p, i, rc);
			failures++;
			if (!p->ready) {
				break; /* latched mid-loop: stop hammering a bad bus */
			}
		}
	}

	if (!p->ready) {
		/* Fresh (or still-mid-call) fault: force a best-effort safe
		 * state. Untracked on purpose — see led_render_policy.h. */
		for (i = 0; i < LED_PHYSICAL_COUNT; i++) {
			(void)write_fn(i, 0u, ctx);
		}
	}
	return failures ? -failures : 0;
}

bool led_render_policy_is_ready(const led_render_policy_t *p)
{
	return p->ready;
}

uint32_t led_render_policy_error_count(const led_render_policy_t *p)
{
	return p->error_count;
}

void led_render_policy_last_failure(const led_render_policy_t *p, uint8_t *index, int *rc)
{
	*index = p->last_fail_index;
	*rc = p->last_fail_rc;
}
