/*
 * st_stem_meter.h — per-stem output level metering and its mapping to a
 * perceptual LED brightness (STEM TAPE Phase 4, beat-pulse feedback).
 *
 * WHAT THIS REPLACES, AND WHY: the first beat-pulse wiring computed ONE
 * `on_beat` boolean for the whole song (from the STIX record's tempo) and
 * handed the SAME value to all four track LEDs. Every audible stem
 * therefore lit and darkened together -- uniform flashing that carries no
 * per-stem information at all. The product behavior is per-stem: each
 * track light tracks THAT stem's own instantaneous output level, so the
 * kick makes the drum light punch, a sustained pad glows steadily, and a
 * silent stem stays dark -- the pulse is a consequence of the audio, not
 * of a tempo number. A stem that is muted, or silenced by another stem's
 * solo, contributes no level and so reads dark, with no separate rule.
 *
 * WHY A SEPARATE PURE MODULE: the envelope and the brightness curve are
 * exactly the kind of arithmetic that is easy to get subtly wrong (a
 * decay that stalls at low amplitude and leaves a light stuck faintly on
 * forever; a linear map that makes everything below -20 dBFS look
 * identically black). Both are pure functions of (previous state, new
 * peak, elapsed ms), so they are host-tested in
 * tests/test_stem_meter.c against explicit expected values rather than
 * eyeballed on hardware.
 *
 * DOMAIN: peaks are absolute sample magnitudes in the stored 24-bit PCM
 * domain (see st_v11_format.h's ST11_PCM_BIT_DEPTH) -- the SAME domain
 * st11_sector_decode_frame() produces and st_stem_mix_frame() consumes,
 * taken BEFORE the 24->16 output shift, so metering resolution does not
 * depend on the output stage.
 *
 * THREADING: an st_stem_meter_t is owned entirely by whichever single
 * thread calls st_stem_meter_update() on it (in the real firmware: the
 * control thread, inside led_service()). The audio thread's only
 * contribution is the raw per-block peak it publishes through its own
 * atomic; this module never touches an atomic and never blocks.
 */

#ifndef STEMTAPE_PLAYER_STEM_METER_H_
#define STEMTAPE_PLAYER_STEM_METER_H_

#include <stdint.h>

/* Full-scale magnitude in the 24-bit stored domain. */
#define ST_STEM_METER_FULL_SCALE 8388607u

/*
 * Release time constant. The envelope falls by this fraction of its own
 * current value per elapsed millisecond-scale step (see the update
 * function): a proportional fall, so the DECIBEL rate of decay is
 * constant and the light drops away at a musically even rate from any
 * starting level, instead of plummeting from loud and then crawling
 * through the last few percent.
 *
 * 250 ms is deliberately slower than a kick transient (so a hit reads as
 * a visible punch rather than a 1-frame blink the eye misses) and faster
 * than a musical bar at any usable tempo (so consecutive hits are seen as
 * separate pulses, not one smeared glow).
 */
#define ST_STEM_METER_RELEASE_MS 250u

/*
 * Magnitudes at or below this read as fully dark. Two jobs: it keeps
 * dither noise and the last inaudible tail of a decaying envelope from
 * leaving a light faintly lit forever, and it guarantees a genuinely
 * silent stem is visibly OFF rather than merely dim. ~-72 dBFS.
 */
#define ST_STEM_METER_FLOOR 2048u

typedef struct {
	uint32_t env;   /* current envelope, 24-bit magnitude domain */
} st_stem_meter_t;

/* Zeroes the envelope (fully dark). */
void st_stem_meter_reset(st_stem_meter_t *m);

/*
 * Advances the envelope by `dt_ms` and applies `peak` (this interval's
 * largest absolute sample magnitude for this stem, already 0 for a stem
 * that is muted or solo-silenced).
 *
 * ATTACK IS INSTANT, RELEASE IS TIMED: a new peak at or above the current
 * envelope takes effect immediately -- a transient must not be smoothed
 * away, since the transient IS the thing being displayed. Only the fall
 * is time-shaped.
 *
 * dt_ms is passed in rather than assumed so the behavior is identical
 * whether the caller runs every 25 ms or every 5 ms, and so the tests can
 * assert exact envelope values over an exact elapsed time. A dt_ms of 0
 * applies the peak with no decay.
 */
void st_stem_meter_update(st_stem_meter_t *m, uint32_t peak, uint32_t dt_ms);

/*
 * Maps the current envelope to a 0..255 LED brightness on a LOGARITHMIC
 * curve (~1.5 dB per step over the top of the range).
 *
 * Linear would be useless here: musical material spends almost all of its
 * time in the top 20 dB of the scale, so a linear map renders every stem
 * either near-black or near-full with nothing in between -- the exact
 * "uniform" impression this whole module exists to remove. A log map
 * spreads real programme material across the visible range, which is also
 * how the eye responds to intensity.
 *
 * Returns 0 for any envelope at or below ST_STEM_METER_FLOOR.
 */
uint8_t st_stem_meter_brightness(const st_stem_meter_t *m);

#endif /* STEMTAPE_PLAYER_STEM_METER_H_ */
