/*
 * led_duty.h — Stem Tape LED Feedback Protocol v1: the ONE authoritative
 * physical LED hardware table, level-to-duty math, and unchanged-frame
 * diffing.
 *
 * PURE: no Zephyr, no PWM driver calls. led_channel_table[] below is the
 * single source of truth for logical index -> physical role -> GPIO -> PWM
 * instance/channel -> gauge position. Every other consumer reads FROM this
 * table instead of re-deriving any of those fields on its own:
 *   - led_render.c's devicetree PWM spec array is cross-checked against this
 *     table's pwm_instance/pwm_channel at led_render_init() time (a runtime
 *     assertion, not just a comment) — it cannot silently drift from this
 *     table without the renderer failing to come up ready.
 *   - main.c's led_diag_sweep() prints straight from this table (no
 *     hand-derived "row < 4 ? ..." arithmetic, which is exactly the class of
 *     bug that once made the sweep report the side row's PWM channels
 *     backward).
 *   - the host tests (tests/test_led.c) check this table directly.
 *   - docs/stem-tape-led-feedback-v1.md mirrors it for the web team.
 */

#ifndef STEMTAPE_LED_DUTY_H_
#define STEMTAPE_LED_DUTY_H_

#include <stdbool.h>
#include <stdint.h>

#include "led_protocol.h"

/* No side-row gauge position (Track row channels 0..3). */
#define LED_GAUGE_STEP_NONE 0xFFu

typedef struct {
	uint8_t index;         /* protocol/physical index, == this entry's array position */
	uint8_t port;           /* 0 = P0, 1 = P1 */
	uint8_t pin;
	uint8_t pwm_instance;   /* 2 or 3 (NRF_PWM2 / NRF_PWM3) */
	uint8_t pwm_channel;    /* 0..3, channel within that PWM instance */
	uint8_t gauge_step;     /* 0..3: bottom-to-top position in the 4-step
				  * charging gauge (led_battery.h) for indices
				  * 4-7; LED_GAUGE_STEP_NONE for 0-3 */
	const char *role;       /* human-readable physical role, e.g. "Track 1"
				  * or "Side, nearest PLAY" */
} led_channel_t;

/* Indexed 0..7, exactly the led_protocol.h table — THE authoritative table;
 * see this header's top comment. */
extern const led_channel_t led_channel_table[LED_PHYSICAL_COUNT];

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
