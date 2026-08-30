/*
 * st_stem_meter.h -- per-stem audio activity, and its mapping to LED
 * brightness. Four of these, one per stem, each following only its own audio.
 *
 * ======================================================================
 * WHAT THIS IS, IN ONE LINE
 * ======================================================================
 *     stem audio -> energy -> noise gate -> attack/release envelope
 *                -> brightness curve -> LED level
 *
 * and explicitly NOT
 *
 *     tempo clock -> blink LED
 *
 * The four Track lights are not beat indicators. They are four very small VU
 * meters, each showing the life of one stem, so that watching the device tells
 * you which parts of the arrangement are currently playing. A drum stem
 * naturally punches and decays; a sustained pad naturally glows; a vocal
 * naturally clusters into syllables and goes dark between phrases. None of
 * that is special-cased per instrument -- it all falls out of following the
 * actual envelope of each stem, which is why it will keep being true for
 * material nobody has tried yet.
 *
 * ======================================================================
 * THE RULE THAT MATTERS MOST
 * ======================================================================
 * NOTHING HERE KNOWS THE TEMPO. There is no bpm, no beat index, no bar
 * position, and no clock of any kind -- the caller passes elapsed
 * milliseconds and this module decays by that much. A stem that is silent for
 * a whole bar reads dark for that whole bar however fast the song is; a stem
 * that sustains across four beats stays lit across all four. If a light ever
 * pulses in time, it is because the audio did.
 *
 * ======================================================================
 * PEAK, NOT RMS, AND WHY
 * ======================================================================
 * The energy measure is the largest absolute sample magnitude seen since the
 * last update -- a peak-programme reading rather than an RMS one. RMS is the
 * better measure of loudness; peak is the better measure of ACTIVITY, which
 * is what is being displayed. A snare hit has a large peak and an
 * unimpressive RMS, and the light should show the hit. The release envelope
 * below is what turns a stream of peaks into something that reads as level
 * rather than as flicker, which is the job RMS would otherwise be doing.
 *
 * ======================================================================
 * ATTACK IS FAST, RELEASE IS SLOW
 * ======================================================================
 * That asymmetry is the whole feel. A transient must appear at once or the
 * eye reads the light as lagging the music; the fall must be slow enough that
 * the eye sees a decay rather than a flicker. Both are milliseconds and both
 * are tunable below.
 *
 * ======================================================================
 * DOMAIN AND THREADING
 * ======================================================================
 * Peaks are absolute sample magnitudes in the stored 24-bit PCM domain -- the
 * same domain st11_sector_decode_frame() produces, taken BEFORE the 24->16
 * output shift, so the resolution of the display does not depend on the
 * output stage.
 *
 * An st_stem_meter_t is owned entirely by the single thread that calls
 * st_stem_meter_update() on it (in the firmware: the control thread, inside
 * led_service()). The audio thread's only contribution is the raw per-block
 * peak it publishes through its own atomic. This module never touches an
 * atomic, never blocks, and has no opinion about who is calling it.
 */

#ifndef STEMTAPE_PLAYER_STEM_METER_H_
#define STEMTAPE_PLAYER_STEM_METER_H_

#include <stdint.h>

/* Full-scale magnitude in the 24-bit stored domain. */
#define ST_STEM_METER_FULL_SCALE 8388607u

/* ======================================================================
 * THE TUNING SET. Every one of these is meant to be changed by eye on real
 * hardware -- that is why they are named constants in a header rather than
 * literals buried in the arithmetic. Nothing else in the firmware has to
 * know when one of them moves.
 * ====================================================================== */

/*
 * ATTACK -- how fast the light is allowed to RISE, in milliseconds.
 *
 * 0 means instant: a new peak takes effect on the frame it arrives. That is
 * the default because the transient IS the thing being shown, and because the
 * control thread services the LEDs roughly every 8-15 ms, so any attack
 * shorter than that is instant in practice anyway.
 *
 * Raise it if the lights feel twitchy and you want a softer swell -- 60-150 ms
 * turns a drum hit from a snap into a bloom. Values below about 15 ms will not
 * be distinguishable from 0 at the current service rate.
 */
#define ST_STEM_METER_ATTACK_MS 0u

/*
 * RELEASE -- how fast the light FALLS, in milliseconds.
 *
 * The fall is proportional (a constant fraction of the current value per
 * millisecond), so the decay is even in decibels and looks the same coming
 * down from a loud hit as from a quiet one. A linear fall would plummet from
 * loud and then crawl through the last few percent, which reads as a light
 * that sticks.
 *
 * 250 ms sits deliberately between two limits: slower than a kick transient,
 * so a hit is a visible punch rather than a blink the eye misses; and faster
 * than a bar at any usable tempo, so consecutive hits read as separate pulses
 * instead of smearing into one glow. Shorten it for a twitchier, more
 * percussive look; lengthen it for a smoother, more sustained one.
 */
#define ST_STEM_METER_RELEASE_MS 250u

/*
 * NOISE FLOOR -- magnitudes at or below this read as fully dark. ~-72 dBFS.
 *
 * Two jobs. It keeps dither, room tone and the last inaudible tail of a decay
 * from leaving a light faintly on forever; and it guarantees that a genuinely
 * silent stem is visibly OFF rather than merely dim, which is what makes the
 * display readable at a glance. Raise it if quiet stems look "never quite
 * off" on hardware.
 */
#define ST_STEM_METER_FLOOR 2048u

/*
 * SENSITIVITY -- the magnitude that reads as FULL brightness.
 *
 * This is the gain control, expressed as a reference level rather than a
 * multiplier because the curve is logarithmic and a reference is the thing
 * that actually moves it. LOWERING this makes everything brighter.
 *
 * The default is -6 dBFS rather than full scale: mastered stems rarely touch
 * 0 dBFS and almost never sit there, so referencing the top of the number
 * range would spend the brightest part of the display on levels the music
 * never reaches. Halving it again (-12 dBFS) is the first thing to try if the
 * lights look dim with real material.
 */
#define ST_STEM_METER_REF 4194304u   /* -6 dBFS in the 24-bit domain */

/*
 * DISPLAY RANGE -- how many octaves BELOW the reference the visible range
 * covers. This is the curve-shape control, and it matters more than any other
 * number here.
 *
 * The noise floor above is ~66 dB under the reference, and mapping all 66 dB
 * across 255 steps was the first thing tried: it gives 0.26 dB per step, so a
 * drum hit decaying by a very audible 18 dB moves the light by a quarter of
 * its range and the row reads as almost static. Musically large changes have
 * to be visually large.
 *
 * 5 octaves is ~30 dB, which is about what a hardware meter bridge shows and
 * enough that a hit falls most of the way to dark between beats. The floor is
 * still what decides OFF -- material between the floor and the bottom of this
 * range reads at MIN_ON rather than vanishing, so a quiet stem is dim, not
 * absent. Widen this for a gentler, more compressed look; narrow it for a
 * punchier, more contrasty one.
 */
#define ST_STEM_METER_SPAN_OCTAVES 5u

/*
 * MINIMUM VISIBLE and MAXIMUM brightness.
 *
 * An LED at duty 1/255 may not light at all, so any stem that is above the
 * noise floor -- i.e. genuinely making sound -- is lifted to at least
 * ST_STEM_METER_MIN_ON. The distinction the eye needs is between OFF and
 * QUIET, and that distinction is worth more than the bottom of the numeric
 * range. Zero stays exactly zero: the floor decides off, not this.
 *
 * MAX caps the top, for pulling the whole row down if the lights are
 * uncomfortably bright in a dark room.
 */
#define ST_STEM_METER_MIN_ON 20u
#define ST_STEM_METER_MAX    255u

typedef struct {
	uint32_t env;   /* current envelope, 24-bit magnitude domain */
} st_stem_meter_t;

/* Zeroes the envelope (fully dark). */
void st_stem_meter_reset(st_stem_meter_t *m);

/*
 * Advances the envelope by `dt_ms` and applies `peak` -- the largest absolute
 * sample magnitude this stem produced since the last call, already 0 for a
 * stem that is muted or silenced by another stem's solo.
 *
 * `dt_ms` is passed in rather than assumed, so the behaviour is identical
 * whether the caller runs every 8 ms or every 25 ms, and so the tests can
 * assert exact envelope values over an exact elapsed time. dt_ms == 0 applies
 * the peak with no decay.
 */
void st_stem_meter_update(st_stem_meter_t *m, uint32_t peak, uint32_t dt_ms);

/*
 * Maps the current envelope to an LED brightness on a LOGARITHMIC curve.
 *
 * Linear would be useless. Musical material spends nearly all its time in the
 * top 20 dB of the scale, so a linear map renders every stem either near-black
 * or near-full with almost nothing in between -- which is the "everything
 * looks the same" impression this module exists to remove. It is also not how
 * the eye responds to intensity. The log map spends the visible range on the
 * ~66 dB between the noise floor and the sensitivity reference, which is where
 * real programme material actually lives.
 *
 * Returns exactly 0 for any envelope at or below ST_STEM_METER_FLOOR, and
 * never returns anything between 0 and ST_STEM_METER_MIN_ON.
 */
uint8_t st_stem_meter_brightness(const st_stem_meter_t *m);

#endif /* STEMTAPE_PLAYER_STEM_METER_H_ */
