/*
 * st_stem_meter.h -- per-stem audio activity, and its mapping to LED
 * brightness. Four of these, one per stem, each following only its own audio.
 *
 * ======================================================================
 * WHAT THIS IS, IN ONE LINE
 * ======================================================================
 *     stem audio -> peak -> noise gate -> BODY envelope + TRANSIENT accent
 *                -> brightness curve -> LED level
 *
 * and explicitly NOT
 *
 *     tempo clock -> blink LED
 *
 * The four Track lights are not beat indicators. They are an expressive
 * visualisation of four stems, so that watching the device tells you what each
 * part of the arrangement is doing. A drum stem punches; a piano breathes and
 * bounces with each chord; a bass swells note by note; a vocal follows
 * syllables. None of that is special-cased per instrument -- it falls out of
 * following each stem's own envelope, which is why it keeps being true for
 * material nobody has tried yet.
 *
 * ======================================================================
 * BODY + ACCENT, AND WHY ONE ENVELOPE WAS NOT ENOUGH
 * ======================================================================
 * The first version was a single envelope: instant attack, slow release, log
 * curve. It followed the audio correctly and it looked STATIC, and the reason
 * is worth stating because it is not obvious.
 *
 * A peak measured over ~10 ms is, for anything sustained, just the signal's
 * amplitude -- and amplitude barely moves between two consecutive reads. A
 * piano holding a chord produced 60% / 62% / 59% / 61%, which to the eye is a
 * solid light. Real mastered stems make it worse: they vary maybe 6-10 dB
 * moment to moment, so on a 30 dB display window the whole musical performance
 * used a fifth of the brightness range, parked near the top.
 *
 * So there are now TWO envelopes over the same peak stream:
 *
 *   BODY   slow attack, slow release. "How much is this stem doing lately."
 *          It is deliberately capped well below full brightness (see
 *          ST_STEM_METER_BODY_SHARE_Q8) so that sustained material sits in
 *          the MIDDLE of the range with headroom above it. A light already at
 *          90% has nowhere to go.
 *
 *   FAST   instant attack, short release. It tracks the immediate signal.
 *
 * The ACCENT is how far FAST is currently above BODY, in decibels. That is a
 * relative-change detector, and it is the thing that makes the display
 * expressive: at a note attack, FAST jumps at once while BODY is still
 * climbing, so the gap opens and the light pops. As BODY catches up the gap
 * closes and the light settles back to its glow. Between attacks FAST falls
 * to BODY, the accent is zero, and the light shows body alone.
 *
 *     brightness = MIN_ON + body + accent
 *
 * BODY'S SLOW ATTACK IS LOAD-BEARING. If BODY rose instantly it would equal
 * FAST at the very instant of every hit, the gap would never open, and there
 * would be no accent at all -- the display would collapse back to the single
 * envelope this replaced.
 *
 * AND IT IS ALL REAL. The accent is measured from the stem's own waveform.
 * Nothing here is random, nothing is periodic, and nothing consults the tempo.
 * A stem that genuinely holds a steady level genuinely reads steady.
 *
 * ======================================================================
 * PEAK, NOT RMS, AND WHY
 * ======================================================================
 * The energy measure is the largest absolute sample magnitude seen since the
 * last update -- a peak-programme reading rather than an RMS one. RMS is the
 * better measure of loudness; peak is the better measure of ACTIVITY, which is
 * what is being displayed, and RMS over any window long enough to be smooth is
 * exactly the averaging that erases the attacks this module exists to show. A
 * snare hit has a large peak and an unimpressive RMS, and the light should
 * show the hit.
 *
 * ======================================================================
 * DOMAIN AND THREADING
 * ======================================================================
 * Peaks are absolute sample magnitudes in the stored 24-bit PCM domain -- the
 * same domain st11_sector_decode_frame() produces, taken BEFORE the 24->16
 * output shift, so the resolution of the display does not depend on the output
 * stage.
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

#include "st_v11_format.h"

/*
 * FULL SCALE IN THE STORED DOMAIN, DERIVED -- not a literal, because a
 * literal is exactly how this got it wrong once.
 *
 * The peaks this module is fed come from stem_render_run(), which meters the
 * sample st_pl_decode_stem_inline() produced. That sample is signed at
 * ST11_PCM_BIT_DEPTH bits, so its full scale is 2^(depth-1) - 1: 8388607 at
 * v1.2's 24 bits, 32767 at v1.3's 16.
 *
 * When the stored width moved to 16 bits this constant stayed at the 24-bit
 * value, and so did REF and FLOOR below. Nothing failed: the meter's own tests
 * are written against these same constants, so they scale with whatever the
 * constants say and cannot see a disagreement with the PRODUCER. What actually
 * happened on hardware is that every peak arrived 256x smaller than the window
 * REF and FLOOR describe -- the entire body range sat above the largest number
 * a 16-bit sample can be, so body_lit was structurally zero and the Track row
 * could never rise above MIN_ON. tests/test_stem_meter.c now closes that with
 * a case that decodes a real full-scale stored frame and asserts it lands
 * here, which is the only kind of check that can catch a domain drift.
 */
#define ST_STEM_METER_FULL_SCALE ((1u << (ST11_PCM_BIT_DEPTH - 1u)) - 1u)

/* ======================================================================
 * THE TUNING SET. Every one of these is meant to be changed by eye on real
 * hardware -- that is why they are named constants in a header rather than
 * literals buried in the arithmetic. Nothing else in the firmware has to know
 * when one of them moves.
 *
 * IF THE ROW LOOKS TOO STATIC, in order of effect:
 *   1. RAISE  BODY_ATTACK_MS   -- a slower body opens a bigger accent gap
 *   2. LOWER  ACCENT_SPAN_OCTAVES -- less dB needed for a full-scale pop
 *   3. LOWER  BODY_SHARE_Q8    -- more of the range reserved for accents
 *   4. SHORTEN FAST_RELEASE_MS -- accents fall away faster, so the next one
 *                                 reads as a separate event
 *
 * IF IT LOOKS TWITCHY OR FLICKERY, move the same four the other way.
 * ====================================================================== */

/*
 * ATTACK -- how fast FAST is allowed to rise, in milliseconds.
 *
 * 0 means instant: a new peak lands on the frame it arrives. That is the
 * default because the transient IS the thing being shown, and because the
 * control thread services the LEDs every ~8-15 ms, so any attack shorter than
 * that is instant in practice anyway. This should almost certainly stay 0;
 * BODY_ATTACK_MS below is the knob that shapes how a hit looks.
 */
#define ST_STEM_METER_ATTACK_MS 0u

/*
 * FAST RELEASE -- how fast the transient detector falls, in milliseconds.
 *
 * Short, because its whole job is to be ahead of the body. Too long and
 * consecutive hits smear into one sustained accent; too short and a hit is a
 * single-frame blink the eye misses at a ~10 ms service rate. 60 ms is about
 * three LED frames of visible decay.
 */
#define ST_STEM_METER_FAST_RELEASE_MS 60u

/*
 * BODY ATTACK -- how fast the glow follows a rise, in milliseconds.
 *
 * THIS IS WHAT CREATES THE ACCENT. The body deliberately lags, so that at a
 * note attack the fast envelope is far above it and the gap between them is
 * large. Set this to 0 and the accent disappears entirely -- body would equal
 * fast at every hit.
 *
 * 120 ms means a hit reads as a pop that blooms into a glow over about an
 * eighth of a second. Longer makes attacks pop harder and for longer; shorter
 * makes the display calmer.
 */
#define ST_STEM_METER_BODY_ATTACK_MS 120u

/*
 * BODY RELEASE -- how fast the glow falls, in milliseconds.
 *
 * The fall is proportional (a constant fraction of the current value per
 * millisecond), so the decay is even in decibels and looks the same coming
 * down from a loud passage as from a quiet one. A linear fall would plummet
 * from loud and then crawl through the last few percent, which reads as a
 * light that sticks.
 *
 * 300 ms is short enough that the glow visibly drops between musical events --
 * which is what lets the NEXT event read as a fresh pulse rather than a bump
 * on a plateau -- and long enough that it is a decay rather than a flicker.
 */
#define ST_STEM_METER_BODY_RELEASE_MS 300u

/*
 * NOISE FLOOR -- magnitudes at or below this read as fully dark. ~-72 dBFS.
 *
 * Two jobs. It keeps dither, room tone and the last inaudible tail of a decay
 * from leaving a light faintly on forever; and it guarantees that a genuinely
 * silent stem is visibly OFF rather than merely dim, which is what makes the
 * display readable at a glance. Raise it if quiet stems look "never quite off"
 * on hardware.
 *
 * Expressed as a fraction of full scale rather than as a magnitude, so it
 * stays -72 dBFS at any stored width: 2048 at 24 bits, 8 at 16.
 */
#define ST_STEM_METER_FLOOR ((ST_STEM_METER_FULL_SCALE + 1u) / 4096u)

/*
 * SENSITIVITY -- the magnitude at which the BODY reaches its own ceiling.
 *
 * The gain control, expressed as a reference level rather than a multiplier
 * because the curve is logarithmic and a reference is the thing that actually
 * moves it. LOWERING this makes everything brighter.
 *
 * -6 dBFS rather than full scale: mastered stems rarely touch 0 dBFS and
 * almost never sit there, so referencing the top of the number range would
 * spend the brightest part of the display on levels the music never reaches.
 */
/* Half of full scale, at whatever the stored width is: 4194304 at 24 bits,
 * 16384 at 16. */
#define ST_STEM_METER_REF ((ST_STEM_METER_FULL_SCALE + 1u) / 2u)

/*
 * BODY DISPLAY RANGE -- how many octaves below the reference the body's own
 * range covers. One octave is ~6 dB.
 *
 * This started at 11 octaves (the whole way down to the noise floor) and the
 * row read as static: at 0.26 dB per brightness step, a drum hit decaying by a
 * very audible 18 dB moved its light by a quarter of its range. 4 octaves is
 * ~24 dB, matched to how much real programme material actually moves, so the
 * body uses its whole range on the dynamics that are present instead of on
 * headroom that never gets used.
 *
 * Narrower = more contrast and more travel, at the cost of quiet material
 * bottoming out sooner.
 */
#define ST_STEM_METER_BODY_SPAN_OCTAVES 4u

/*
 * ACCENT RANGE -- how far FAST must sit above BODY, in octaves, to produce a
 * full-scale accent. One octave is ~6 dB.
 *
 * This is the transient sensitivity. At 1 octave, a 6 dB jump above the
 * current glow lights the entire accent range -- and 6 dB is an ordinary
 * musical accent, not a dramatic one, which is the point: the brief asked for
 * a move from 0.30 to 0.45 to be visible, and that is 3.5 dB, so it lands at
 * better than half accent.
 *
 * Lower this for a more excitable display, raise it for a calmer one.
 */
#define ST_STEM_METER_ACCENT_SPAN_OCTAVES 1u

/*
 * HOW THE RANGE IS SPLIT between the steady glow and the accents, in 256ths.
 *
 * 120/256 is a little under half: sustained material at full body sits just
 * under halfway up the visible range and everything above that is reserved for
 * attacks. This is the anti-saturation control -- a light that normally sits
 * at 90% has nowhere left to animate, which was the single biggest reason the
 * first version looked flat.
 *
 * Lower it to reserve even more room for accents; raise it if the row looks
 * too dim between hits.
 */
#define ST_STEM_METER_BODY_SHARE_Q8 120u

/*
 * MINIMUM VISIBLE and MAXIMUM brightness.
 *
 * An LED at duty 1/255 may not light at all, so any stem above the noise floor
 * -- i.e. genuinely making sound -- is lifted to at least MIN_ON. The
 * distinction the eye needs is between OFF and QUIET, and that is worth more
 * than the bottom of the numeric range. Zero stays exactly zero: the floor
 * decides off, not this.
 *
 * MAX caps the top, for pulling the whole row down if the lights are
 * uncomfortably bright in a dark room.
 */
#define ST_STEM_METER_MIN_ON 16u
#define ST_STEM_METER_MAX    255u

typedef struct {
	uint32_t fast;   /* immediate level: instant attack, short release */
	uint32_t body;   /* the glow: lagging attack, slower release */
} st_stem_meter_t;

/* Zeroes both envelopes (fully dark). */
void st_stem_meter_reset(st_stem_meter_t *m);

/*
 * Advances both envelopes by `dt_ms` and applies `peak` -- the largest
 * absolute sample magnitude this stem produced since the last call, already 0
 * for a stem that is muted or silenced by another stem's solo.
 *
 * `dt_ms` is passed in rather than assumed, so the behaviour is identical
 * whether the caller runs every 8 ms or every 25 ms, and so the tests can
 * assert exact envelope values over an exact elapsed time. dt_ms == 0 applies
 * the peak with no decay.
 */
void st_stem_meter_update(st_stem_meter_t *m, uint32_t peak, uint32_t dt_ms);

/*
 * The LED level: body glow plus transient accent, on a LOGARITHMIC curve.
 *
 * Linear would be useless. Musical material spends nearly all its time in the
 * top 20-30 dB of the scale, so a linear map renders every stem either
 * near-black or near-full with almost nothing in between -- and it is not how
 * the eye responds to intensity either.
 *
 * Returns exactly 0 only when the stem is genuinely silent (both envelopes at
 * or below the noise floor), and otherwise never returns less than
 * ST_STEM_METER_MIN_ON.
 */
uint8_t st_stem_meter_brightness(const st_stem_meter_t *m);

#endif /* STEMTAPE_PLAYER_STEM_METER_H_ */
