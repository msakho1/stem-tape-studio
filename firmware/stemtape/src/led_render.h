/*
 * led_render.h — Stem Tape M0 physical LED renderer: hardware PWM2/PWM3.
 *
 * NOT host-testable (touches the PWM driver/devicetree) — kept deliberately
 * thin. All brightness math is led_duty.c's pure led_level_to_pulse_us()
 * and led_duty_diff_frame(), which ARE host-tested; this file only feeds
 * their results to pwm_set_pulse_dt() and propagates its return code. No
 * ISR: the nRF PWM peripheral generates each channel's waveform
 * autonomously in hardware once programmed, so there is no software-PWM
 * loop and nothing here runs from interrupt context.
 *
 * PHYSICAL ATOMICITY, ACCURATELY STATED: a call to this module renders all
 * eight channels by issuing up to eight separate pwm_set_pulse_dt() calls
 * (one per changed channel — see led_duty_diff_frame()). Each call is an
 * independent register write to a shared nRF PWM peripheral instance
 * (PWM2 serves the four Track channels, PWM3 serves the four side
 * channels); per Zephyr's nrfx PWM driver
 * (drivers/pwm/pwm_nrfx.c, zephyrproject-rtos/zephyr), updating one
 * channel's duty re-triggers that instance's whole EasyDMA sequence
 * playback, which can restart the waveform for every channel sharing that
 * instance, not just the one that changed. This means multiple channels on
 * the SAME PWM instance that change together in one commit are NOT
 * guaranteed to visually transition at the same instant — up to one
 * LED_PWM_PERIOD_US (1024 us) of stagger between them is physically
 * possible. This has NOT been measured on real hardware. Firmware-state
 * atomicity (led_frame_commit() copying staged[] to active[] as one
 * transaction) is unaffected and unrelated to this hardware limitation —
 * see led_frame.h.
 */

#ifndef STEMTAPE_LED_RENDER_H_
#define STEMTAPE_LED_RENDER_H_

#include <stdbool.h>
#include <stdint.h>

#include "led_protocol.h"

/* Verifies all 8 PWM channels are ready and drives every channel to a
 * proven-off state. Returns 0 if every channel is ready AND every one of
 * the 8 proving writes succeeded; -1 otherwise. The readiness gate is
 * all-or-nothing (matching feldd's led_init()): if any single channel is
 * not ready, or any proving write fails, led_render_is_ready() latches
 * false and led_render_apply() no-ops entirely rather than partially drive
 * a device in an unverified state. */
int led_render_init(void);

/* True only if led_render_init() verified every one of the 8 channels.
 * "Do not answer the CC91 capability query as supported unless all eight
 * outputs initialized successfully" reads this (via
 * led_capability_should_answer()) before responding. */
bool led_render_is_ready(void);

/* Cumulative count of pwm_set_pulse_dt() calls that returned a nonzero
 * (error) status since boot. Exposed for CDC diagnostics. */
uint32_t led_render_error_count(void);

/*
 * Renders a complete 8-value 0..127 frame. Skips any channel whose value is
 * unchanged since the last successfully-applied render (led_duty_diff_frame())
 * — "do not resend all eight PWM values every 5 ms when nothing changed" —
 * so a steady frame costs zero PWM register writes per call.
 *
 * Returns 0 if every attempted write succeeded (including "no writes were
 * needed"); a negative count of failed writes otherwise. The caller (main.c)
 * must release host ownership and fail safely on a nonzero return — this
 * function does not do that itself, since it has no concept of "ownership".
 *
 * The caller is responsible for taking a consistent snapshot beforehand
 * (see main.c's led_snapshot_active()) — this function does no locking
 * itself, so it must only ever be given a buffer that cannot be mutated by
 * another thread while this call runs.
 */
int led_render_apply(const uint8_t levels[LED_PHYSICAL_COUNT]);

#endif /* STEMTAPE_LED_RENDER_H_ */
