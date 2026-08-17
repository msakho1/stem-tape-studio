/*
 * led_render_policy.h — Stem Tape LED Feedback Protocol v1: the render
 * write/retry/fault-latch POLICY, factored out of led_render.c so it can be
 * exercised on the host with a mocked physical write, exactly like
 * led_frame.c/led_duty.c/led_battery.c.
 *
 * PURE: no Zephyr, no PWM driver calls. led_render.c (Zephyr-only, NOT
 * host-testable) is now a thin adapter: it defines one function that calls
 * pwm_set_pulse_dt() and hands it to this module as `led_channel_write_fn`.
 * The host tests hand the SAME functions here a mock write function that can
 * be told to fail on demand, so "a failed write is not cached as successful",
 * "the same requested level is retried", "an unrecovered failure clears
 * readiness and host ownership" etc. are proven against the real policy
 * logic, not a parallel reimplementation of it.
 */

#ifndef STEMTAPE_LED_RENDER_POLICY_H_
#define STEMTAPE_LED_RENDER_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

#include "led_protocol.h"

/* Attempt to physically drive `index` to `pulse_us`. Returns 0 on success,
 * matching pwm_set_pulse_dt()'s convention; any nonzero value is a failure
 * and is preserved verbatim for diagnostics (led_render_policy_last_failure()).
 * `ctx` is opaque and passed through unchanged: real firmware code needs
 * none (the devicetree PWM spec table is compiled in), a host-test mock uses
 * it to hold its own fail-injection state. */
typedef int (*led_channel_write_fn)(uint8_t index, uint32_t pulse_us, void *ctx);

/* How many CONSECUTIVE write failures on the SAME channel latch the whole
 * renderer not-ready. This is a firmware policy choice, not a hardware fact
 * or a calibrated value — "retry it deterministically" (every apply() call
 * retries a dirty channel) before "latch...if recovery fails". */
#define LED_RENDER_MAX_CONSECUTIVE_FAILS 3u

typedef struct {
	bool     ready;
	bool     cache_valid[LED_PHYSICAL_COUNT];  /* true only once a write for this
						     * channel has actually SUCCEEDED
						     * since the last (re)init */
	uint8_t  cached_level[LED_PHYSICAL_COUNT]; /* valid iff cache_valid[i] */
	bool     dirty[LED_PHYSICAL_COUNT];        /* a write for this channel is
						     * outstanding and must be retried */
	uint8_t  fail_streak[LED_PHYSICAL_COUNT];
	uint32_t error_count;    /* cumulative failed writes; never reset by reinit */
	uint8_t  last_fail_index; /* LED_PHYSICAL_COUNT == "no failure recorded yet" */
	int      last_fail_rc;
} led_render_policy_t;

/* Zeroed / not-ready starting state. Call once before the first bringup. */
void led_render_policy_init(led_render_policy_t *p);

/*
 * Bring-up AND the only recovery path after a latched fault ("define an
 * explicit recovery or renderer reinitialization path"): attempts to drive
 * every one of the 8 channels to 0 through write_fn, discarding all prior
 * per-channel cache/dirty/fail-streak state first. Returns (and leaves
 * p->ready as) true only if every one of the 8 proving writes succeeded this
 * call — "led_render_is_ready() must not remain true after an unrecovered
 * runtime write failure", and symmetrically here, it must not become true
 * again until every channel is proven usable. Cumulative error_count and
 * last_fail_* history are NOT reset (they are diagnostic counters, not
 * session state — same rule as led_frame_release()).
 */
bool led_render_policy_bringup(led_render_policy_t *p, led_channel_write_fn write_fn, void *ctx);

/*
 * Renders `levels` through write_fn. Not ready -> returns -1 immediately and
 * writes nothing at all (suppression applies to physical writes, not just
 * the MIDI capability response).
 *
 * Otherwise, for each channel: skipped ONLY if it is already known-good at
 * exactly the requested level (cache_valid && !dirty && cached_level ==
 * levels[i]) — "successful channels are not unnecessarily rewritten".
 * Every other channel is (re)written:
 *   - success: cache updated to `levels[i]`, dirty/fail_streak cleared —
 *     "do not update an LED's cached level until pwm_set_pulse_dt()
 *     succeeds" holds by construction, since the cache is only ever touched
 *     in this branch.
 *   - failure: cache_valid cleared (the physical state is now unknown, never
 *     presented as still-good), dirty set so the exact same `levels[i]`
 *     requested this call is retried on the very next apply() — "the same
 *     requested level is retried" — fail_streak incremented, error_count and
 *     last_fail_* updated. Once any channel's fail_streak reaches
 *     LED_RENDER_MAX_CONSECUTIVE_FAILS, `ready` latches false for the WHOLE
 *     renderer and no further channels are attempted this call.
 *
 * On a fresh fault latch (ready was true on entry, false on exit), this call
 * additionally issues one best-effort forced 0us write to every channel
 * before returning — "put the outputs into the documented safe state". This
 * is best-effort only and its result is not tracked: if the underlying fault
 * also breaks this write, that is simply the documented limit of what
 * firmware alone can guarantee once the PWM path itself is unhealthy.
 *
 * Returns 0 if every attempted write succeeded (including "nothing needed
 * writing"), or a negative count of failed writes otherwise.
 */
int led_render_policy_apply(led_render_policy_t *p, const uint8_t levels[LED_PHYSICAL_COUNT],
			     led_channel_write_fn write_fn, void *ctx);

bool led_render_policy_is_ready(const led_render_policy_t *p);
uint32_t led_render_policy_error_count(const led_render_policy_t *p);

/* Most recent failing channel index (LED_PHYSICAL_COUNT if none ever
 * recorded) and its write_fn return code — "expose the exact failing LED
 * index and return code through CDC". */
void led_render_policy_last_failure(const led_render_policy_t *p, uint8_t *index, int *rc);

#endif /* STEMTAPE_LED_RENDER_POLICY_H_ */
