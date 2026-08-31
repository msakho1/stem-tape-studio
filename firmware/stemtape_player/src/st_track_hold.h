/*
 * st_track_hold.h — pure momentary hold-to-solo classifier.
 *
 * CORRECTION (Phase 3 control-matrix): the first version of the hold-to-
 * solo gesture toggled trk[ti].solo once, at release, if the hold had
 * lasted >= TRACK_HOLD_SOLO_MS -- a LATCH, not the momentary behavior
 * actually specified. This module is the corrected, pure replacement:
 * decoupled from main.c's own ladder-debounce/episode-attribution
 * machinery (proven, unchanged, NOT touched by this correction -- see
 * main.c's own comment at its call site), it only answers "given how
 * long this exact button has been continuously, debounced-real,
 * physically held, is solo active right now for it" -- one instance per
 * track, each fed that track's own press state every scan pass.
 *
 * MOMENTARY, NOT LATCHED:
 *   - held < threshold_ms, still pressed: not yet active.
 *   - held >= threshold_ms, still pressed: active, from that exact pass
 *     onward, every pass, until released.
 *   - the pass `pressed` becomes false: ALWAYS inactive again, whether or
 *     not the threshold was ever reached -- release unconditionally
 *     clears. This is what makes it momentary: there is no toggle, no
 *     latch, nothing for a second press to undo.
 *   - a hold that reached active must never also be read by the caller as
 *     a tap-to-mute gesture -- main.c's own release handler reads
 *     `solo_active` (still true from the pass just before release; this
 *     module's own release-clearing runs on a LATER pass -- see main.c's
 *     own comment on why that ordering is safe) to decide whether to
 *     suppress its existing, unchanged tap-to-mute action for that
 *     release.
 *
 * INDEPENDENT PER INSTANCE: nothing here assumes there is only one track
 * ever held. The real SP-1 hardware this ships on decodes PLAY/TRACK1-4
 * from one shared resistor ladder (see main.c's own decode_tracks()
 * comment), so only one track can physically be reported held at a time
 * today -- but this module's own state is not aware of that limitation
 * and does not encode it: one instance per track, each entirely
 * independent, so a future scanner that CAN report simultaneous holds
 * needs no change here, only a different main.c calling pattern.
 *
 * PURE: no I/O, no Zephyr, no time source of its own -- the caller
 * supplies `held_ms` (already computed from its own clock) every call.
 */

#ifndef STEMTAPE_PLAYER_TRACK_HOLD_H_
#define STEMTAPE_PLAYER_TRACK_HOLD_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	/* Owned exclusively by st_track_hold_tick() -- callers read this
	 * (e.g. to decide whether to suppress a tap-to-mute action at
	 * release) but never assign it directly. */
	bool solo_active;
} st_track_hold_t;

/* Cold-boot / song-load / mode-change reset: solo is never carried across
 * any of those (see this header's own doc comment; also enforced
 * structurally by release always clearing regardless of reset). */
void st_track_hold_reset(st_track_hold_t *s);

/*
 * Call once per scan pass for this one track, with `pressed` reflecting
 * THIS pass's already-debounced physical state (main.c's own
 * `committed == TRK_i`) and `held_ms` the real elapsed time since this
 * exact press began (main.c's own `now_ms - press_t[ti]`; meaningless
 * and ignored while `pressed` is false). Returns the resulting
 * `solo_active` state after this call -- assign it directly to the
 * caller's own per-stem solo flag every pass, for every track, every
 * pass (idempotent; safe and correct to call even for tracks that are
 * not currently pressed, which is exactly how release gets detected and
 * cleared).
 */
bool st_track_hold_tick(st_track_hold_t *s, bool pressed, uint32_t held_ms, uint32_t threshold_ms);

#endif /* STEMTAPE_PLAYER_TRACK_HOLD_H_ */
