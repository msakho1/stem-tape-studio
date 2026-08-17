/*
 * led_render.h — Stem Tape M0 physical LED renderer: hardware PWM2/PWM3.
 *
 * NOT host-testable (touches the PWM driver/devicetree) — kept deliberately
 * thin. All brightness math is led_duty.c's pure led_level_to_pulse_us(),
 * which IS host-tested; this file only feeds that function's result to
 * pwm_set_pulse_dt(). No ISR: the nRF PWM peripheral generates the waveform
 * autonomously in hardware once programmed, so there is no software-PWM loop
 * and nothing here runs from interrupt context.
 */

#ifndef STEMTAPE_LED_RENDER_H_
#define STEMTAPE_LED_RENDER_H_

#include <stdint.h>

#include "led_protocol.h"

/* Verifies all 8 PWM channels are ready and drives every channel to off.
 * Returns 0 if every channel is ready, -1 otherwise. The readiness gate is
 * all-or-nothing (matching feldd's led_init()): if any single channel is
 * not ready, led_render_apply() no-ops entirely rather than partially drive
 * a device in an unverified state. */
int led_render_init(void);

/* Renders a complete 8-value 0..127 frame in one call. The caller is
 * responsible for taking a consistent snapshot beforehand (see main.c's
 * led_snapshot_active()) — this function does no locking itself, so it must
 * only ever be given a buffer that cannot be mutated by another thread while
 * this call runs. */
void led_render_apply(const uint8_t levels[LED_PHYSICAL_COUNT]);

#endif /* STEMTAPE_LED_RENDER_H_ */
