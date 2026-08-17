/*
 * led_render.h — Stem Tape M0 physical LED renderer: hardware PWM2/PWM3.
 *
 * NOT host-testable (touches the PWM driver/devicetree) — kept deliberately
 * thin: it is now just an adapter binding pwm_set_pulse_dt() to
 * led_render_policy.c's PURE (and host-tested) write/retry/fault-latch
 * policy. No ISR: the nRF PWM peripheral generates each channel's waveform
 * autonomously in hardware once programmed, so there is no software-PWM
 * loop and nothing here runs from interrupt context.
 *
 * PHYSICAL ATOMICITY, ACCURATELY STATED: a call to this module renders up to
 * eight independent register writes (one per channel actually needing one —
 * see led_render_policy.h), each an independent write to a shared nRF PWM
 * peripheral instance (PWM2 serves the four Track channels, PWM3 serves the
 * four side channels); per Zephyr's nrfx PWM driver
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

/* Verifies all 8 PWM channels are devicetree-ready, that each one's
 * configured devicetree channel matches led_channel_table's pwm_channel for
 * that index (a runtime cross-check against the ONE authoritative table —
 * see led_duty.h — not just a comment that could silently drift), and drives
 * every channel to a proven-off state through led_render_policy_bringup().
 * Returns 0 only if every one of those checks and all 8 proving writes
 * succeeded; -1 otherwise. led_render_is_ready() latches false on any
 * failure rather than partially drive a device in an unverified state. */
int led_render_init(void);

/* True only if every one of the 8 outputs is currently proven usable: latched
 * true by led_render_init()/led_render_reinit() and latched back to false by
 * led_render_apply() if a channel fails LED_RENDER_MAX_CONSECUTIVE_FAILS
 * consecutive writes at runtime — it does NOT stay true after an unrecovered
 * runtime write failure. "Do not answer the CC91 capability query as
 * supported unless all eight outputs [are] usable" reads this (via
 * led_capability_should_answer()) before responding. */
bool led_render_is_ready(void);

/* Cumulative count of failed physical writes since boot (never reset by
 * led_render_reinit()). Exposed for CDC diagnostics. */
uint32_t led_render_error_count(void);

/* Most recent failing channel index (LED_PHYSICAL_COUNT if none ever
 * recorded) and its pwm_set_pulse_dt() return code — "expose the exact
 * failing LED index and return code through CDC". */
void led_render_last_failure(uint8_t *index, int *rc);

/* Explicit recovery / reinitialization path, callable any time (e.g. from a
 * CDC command) after a runtime fault has latched led_render_is_ready() false.
 * Re-runs the exact same all-8-channels proving sequence as
 * led_render_init(); "recovery restores capability only after all eight
 * channels are usable" holds because it is the identical bringup routine. */
int led_render_reinit(void);

/*
 * Renders a complete 8-value 0..127 frame. Not ready -> returns -1 and
 * writes nothing at all. Otherwise skips any channel already proven at the
 * requested level (led_render_policy.h) — a steady, fully-healthy frame
 * costs zero PWM register writes per call. A channel whose write fails is
 * left dirty for a deterministic retry on the next call; its cached level is
 * NOT updated until a write for it actually succeeds. If a channel fails
 * LED_RENDER_MAX_CONSECUTIVE_FAILS consecutive times, the WHOLE renderer
 * latches not-ready (led_render_is_ready() flips false) and the outputs are
 * forced to a best-effort safe (all-off) state.
 *
 * Returns 0 if every attempted write succeeded (including "no writes were
 * needed"); a negative count of failed writes otherwise. The caller (main.c)
 * must release host ownership when led_render_is_ready() has gone false —
 * this function does not do that itself, since it has no concept of
 * "ownership".
 *
 * The caller is responsible for taking a consistent snapshot beforehand
 * (see main.c's led_snapshot_active()) — this function does no locking
 * itself, so it must only ever be given a buffer that cannot be mutated by
 * another thread while this call runs.
 */
int led_render_apply(const uint8_t levels[LED_PHYSICAL_COUNT]);

#endif /* STEMTAPE_LED_RENDER_H_ */
