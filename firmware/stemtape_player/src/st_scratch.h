/*
 * st_scratch.h -- THE signed-head transport primitive, and the eMMC velocity
 * clamp that bounds it.
 *
 * PURE: no Zephyr, no ADC, no clock, no audio, no allocation. The control
 * thread hands this module a gesture (a rocker edge, or a fader movement) and
 * an elapsed time; the audio thread reads back a signed rate. This module owns
 * the physics and nothing else.
 *
 * ======================================================================
 * ONE TRANSPORT, THREE USES -- AND THAT IS THE WHOLE DESIGN
 * ======================================================================
 * Reverse, scrubbing and scratching are not three features. They are three
 * durations of one gesture on one signed-velocity head:
 *
 *   reverse    sustained negative velocity
 *   scrub      sustained user-controlled signed velocity
 *   scratch    rapid signed-velocity manipulation
 *
 * There is deliberately NO timeout that decides "this was a scratch, that was
 * a scrub". The state machine does not know the difference and must not: the
 * duration of the player's input is what produces the musical result, and a
 * threshold in here would be an arbitrary seam the hand can feel. Short
 * alternating rocker presses integrate into a back-and-forth head motion; a
 * sustained hold integrates into a shuttle. Same integrator, same clamp, same
 * zero crossing.
 *
 * The TARGET is the only thing that varies:
 *
 *   ST_SCRATCH_MASTER   all four heads, phase-locked, moved as one four-track
 *                       tape. There is no hidden forward transport running
 *                       underneath -- where the gesture leaves the head IS the
 *                       position, and releasing FUNCTION resumes from there.
 *   ST_SCRATCH_STEM_n   one head moves; the other three are untouched, keep
 *                       their own positions, and are never resynced.
 *
 * ======================================================================
 * WHY THERE IS A CLAMP, AND WHY IT IS DERIVED RATHER THAN CHOSEN
 * ======================================================================
 * Playback rate is a data rate. At |r| the transport consumes stored audio |r|
 * times faster, so the eMMC must supply it |r| times faster, and past some |r|
 * it simply cannot -- that is starvation by arithmetic, not by bad luck, and
 * no amount of buffering fixes a sustained overdraft.
 *
 * So the clamp is not a taste decision. It falls out of the measured read
 * cost, the refill batch size and how many heads are moving, and it is
 * computed here from those inputs rather than written down as a number
 * somebody liked. tests/test_scratch.c re-derives every figure below from the
 * same measurement and fails if a hand-edited constant drifts from it.
 *
 * THE MODEL, stated so it can be argued with:
 *
 *   One refill batch for ONE stem is ST_LAT_REFILL_GROUPS groups, each
 *   ST_PL_GROUP_BLOCKS blocks, in a single emmc_read_blocks(). Its cost is the
 *   measured fit us = 649 + 158.4*blocks (tools/sp1-readcost-sweep.py, st32
 *   sweep, 24 reads per size). It covers ST_LAT_REFILL_GROUPS sectors of
 *   audio, which at unity is ST_LAT_REFILL_GROUPS * ST_LAT_SECTOR_US.
 *
 *   A head moving at rate r consumes that cover r times faster. So N heads at
 *   rate r occupy the eMMC for
 *
 *       duty(N, r) = N * BATCH_US * r / BATCH_COVER_US
 *
 *   and the clamp is the largest r with duty <= the budget.
 *
 * THE BUDGET IS 85%, NOT 100%. The same 15% the rest of the streaming design
 * reserves (ST_LAT_MARGIN_PCT), for the same reason: 100% duty is the cliff,
 * not a place to operate. At the cliff every read must land perfectly and the
 * first jittery one starves the transport.
 *
 * WHAT THE CLAMP DOES *NOT* HAVE TO COVER, and this is the part that makes
 * scratching affordable at all: a gesture that oscillates INSIDE the resident
 * ring costs nothing. The ring holds ST_LAT_RING_SLOTS groups per stem, so a
 * head may move freely across ST_SCRATCH_FREE_WINDOW_US of audio -- forward,
 * backward, repeatedly -- without a single new read. A real scratch is mostly
 * this. The clamp exists for the part of a gesture that TRAVELS, which is the
 * shuttle, and for a scratch wide enough to leave the ring.
 *
 * tests/test_scratch.c does not take that on faith: it drives a synthetic
 * oscillating gesture through the REAL st_stem_stream state machine and counts
 * the sectors actually demanded, so the free-window claim is measured against
 * the production ring rather than asserted here.
 */

#ifndef STEMTAPE_PLAYER_SCRATCH_H_
#define STEMTAPE_PLAYER_SCRATCH_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_latency.h"
#include "st_planar.h"
#include "st_resample.h"
#include "st_v11_format.h"

/* ======================================================================
 * THE MEASURED READ COST
 * ======================================================================
 * us = FIXED + PER_BLOCK * blocks, fitted on real hardware. PER_BLOCK is kept
 * in Q8 because 158.4 is not an integer and rounding it to 158 understates
 * every cost below -- understating a cost is the unsafe direction for a
 * starvation clamp, so the fraction is carried rather than dropped.
 */
#define ST_SCRATCH_RC_FIXED_US      649u
#define ST_SCRATCH_RC_PER_BLOCK_Q8  40550u   /* 158.4 * 256, rounded up */

/* One stem's refill batch: R groups, each ST_PL_GROUP_BLOCKS blocks. */
#define ST_SCRATCH_BATCH_BLOCKS \
	(ST_LAT_REFILL_GROUPS * ST_PL_GROUP_BLOCKS)

#define ST_SCRATCH_BATCH_US \
	(ST_SCRATCH_RC_FIXED_US + \
	 ((ST_SCRATCH_BATCH_BLOCKS * ST_SCRATCH_RC_PER_BLOCK_Q8) + 255u) / 256u)

/* Audio that batch covers, at unity. */
#define ST_SCRATCH_BATCH_COVER_US \
	(ST_LAT_REFILL_GROUPS * ST_LAT_SECTOR_US)

/* The duty budget, as a percentage. Deliberately the SAME margin the read-ahead
 * depth uses, so there is one notion of "how close to the cliff we operate". */
#define ST_SCRATCH_BUDGET_PCT (100u - ST_LAT_MARGIN_PCT)

/*
 * THE CLAMP, in Q16, for a gesture in which `moving` heads travel at the
 * clamped rate while the remaining heads run forward at unity.
 *
 *   moving * BATCH_US * r + (STEMS - moving) * BATCH_US * 1
 *       <= BUDGET_PCT/100 * BATCH_COVER_US
 *
 * solved for r. The still heads are charged their unity cost because they are
 * genuinely still reading -- an isolated stem scratch does not pause the other
 * three, which is the entire point of it.
 */
#define ST_SCRATCH_STILL_US(moving) \
	((uint32_t)(ST_PL_STEMS - (moving)) * ST_SCRATCH_BATCH_US)

#define ST_SCRATCH_BUDGET_US \
	((ST_SCRATCH_BUDGET_PCT * ST_SCRATCH_BATCH_COVER_US) / 100u)

#define ST_SCRATCH_MAX_RATE_Q16(moving) \
	((uint32_t)(((uint64_t)(ST_SCRATCH_BUDGET_US - ST_SCRATCH_STILL_US(moving)) << 16) / \
		     ((uint64_t)(moving) * ST_SCRATCH_BATCH_US)))

/*
 * ======================================================================
 * THE SECOND CEILING, AND WHY THE FIRST ONE IS NOT THE ANSWER
 * ======================================================================
 * Everything above answers "how fast can the eMMC feed a head". There is a
 * separate and lower limit on how fast the RENDERER can read one, and
 * st_resample.h states it directly: ST_RS_RATE_MAX is 2x because two source
 * frames per output frame is the most the interpolator's carried
 * previous-frame can span in one step, and the run arithmetic is written
 * against it. That header already names the principle -- "two separate
 * ceilings for two separate reasons: this one says what the reader can do,
 * that one says what the storage can feed" -- and the storage one is the only
 * one the derivation above knew about.
 *
 * So the rate a gesture may actually ask for is the MINIMUM of the two. This
 * matters most exactly where the eMMC is most generous: an isolated stem
 * scratch is affordable at 7.62x and renderable at 2x, so 2x is what it gets.
 * Shipping the storage figure would have driven the interpolator four times
 * past its stated span -- not memory-unsafe, since stem_render_run() clamps
 * its own reads to the run it was given, but well outside what the module
 * claims to support and audibly so.
 *
 * Both numbers are kept rather than collapsed into one, because they answer
 * different questions and will move for different reasons: a faster card
 * raises the first, a better interpolator raises the second.
 */
#define ST_SCRATCH_RENDER_MAX_Q16 ((uint32_t)ST_RS_RATE_MAX)

#define ST_SCRATCH_EFFECTIVE_MAX_Q16(moving) \
	((ST_SCRATCH_MAX_RATE_Q16(moving) < ST_SCRATCH_RENDER_MAX_Q16) \
	  ? ST_SCRATCH_MAX_RATE_Q16(moving) : ST_SCRATCH_RENDER_MAX_Q16)

/* The two the firmware actually uses. MASTER moves all four; a stem scratch
 * moves one and leaves three at unity. Both are the effective figure -- the
 * one a gesture may really reach -- not the storage figure alone. */
#define ST_SCRATCH_MAX_RATE_MASTER_Q16 ST_SCRATCH_EFFECTIVE_MAX_Q16(ST_PL_STEMS)
#define ST_SCRATCH_MAX_RATE_STEM_Q16   ST_SCRATCH_EFFECTIVE_MAX_Q16(1u)

/* The storage figures on their own, for the report and the tests. */
#define ST_SCRATCH_EMMC_MAX_MASTER_Q16 ST_SCRATCH_MAX_RATE_Q16(ST_PL_STEMS)
#define ST_SCRATCH_EMMC_MAX_STEM_Q16   ST_SCRATCH_MAX_RATE_Q16(1u)

/*
 * THE FREE WINDOW: audio a head may range over without a new read, because the
 * ring already holds it. One slot is the one the consumer sits in, so the
 * travel available either side of it is (G - 1) sectors.
 */
#define ST_SCRATCH_FREE_WINDOW_US \
	((ST_LAT_RING_SLOTS - 1u) * ST_LAT_SECTOR_US)

#if !defined(__cplusplus)
_Static_assert(ST_SCRATCH_BUDGET_US > ST_SCRATCH_STILL_US(1u),
	       "the three still heads must not already exhaust the duty budget");
_Static_assert(ST_SCRATCH_MAX_RATE_MASTER_Q16 > 65536u,
	       "a master gesture must at least reach unity, or it cannot shuttle at all");
_Static_assert(ST_SCRATCH_EMMC_MAX_STEM_Q16 > ST_SCRATCH_EMMC_MAX_MASTER_Q16,
	       "moving one head must be cheaper than moving four");
_Static_assert(ST_SCRATCH_MAX_RATE_MASTER_Q16 <= ST_SCRATCH_RENDER_MAX_Q16 &&
	       ST_SCRATCH_MAX_RATE_STEM_Q16 <= ST_SCRATCH_RENDER_MAX_Q16,
	       "no gesture may ask the renderer for more than it can span");
#endif

/* ======================================================================
 * THE TRANSPORT PRIMITIVE
 * ======================================================================
 *
 * ONE INTEGRATOR. The control hands in a signed DRIVE -- "how hard is the hand
 * pushing, and which way" -- and this walks the head's signed RATE toward it.
 * That single fact is what makes scratch and scrub the same state machine:
 *
 *   a short press      drive goes +1 then 0; the rate rises, then falls back
 *                      through zero as the next press pushes the other way.
 *                      Alternate rapidly and the head oscillates -- scratching.
 *   a sustained hold   drive stays +1; the rate rises to the clamp and stays
 *                      there. The tape keeps travelling -- shuttling.
 *
 * Nothing in here measures how long a press lasted, and nothing decides which
 * of the two is happening. There is no timeout, no mode flag and no threshold,
 * because any of those would be a seam the hand can feel at the moment it is
 * crossed. The duration of the input produces the musical result by itself.
 *
 * THE ZERO CROSSING IS NOT A CASE either. Drive flipping from + to - walks the
 * rate down through zero and up the other side by the same arithmetic that
 * moves it anywhere else. Forward -> slow -> stopped -> reverse is continuous
 * because it is one number changing sign, not two code paths meeting.
 *
 * ASYMMETRIC BY DESIGN. Pushing accelerates over ST_SCRATCH_ACCEL_MS; letting
 * go decelerates over ST_SCRATCH_DECEL_MS, which is shorter. That is the hand
 * on the record: a push takes effort and builds, but the moment the hand stops
 * pushing while still resting on the platter, the tape stops quickly. Both are
 * far shorter than the tape-inertia ramp st_scrub.h uses for PLAY/STOP, which
 * is a motor spinning up and is deliberately slow. Scratching needs the hand,
 * not the motor.
 *
 * RELEASING THE GESTURE IS THE THIRD THING, and it is the one case that hands
 * back to st_scrub: FUNCTION up means the hand comes OFF the record, so the
 * rate returns to +1.0 along the existing tape-inertia release ramp from
 * whatever signed rate it had. Where the head ended up is where playback
 * resumes -- there is no hidden forward transport running underneath and
 * nothing snaps back.
 */

/* Milliseconds from a standing start to the clamp under sustained full drive.
 * Short enough that a ~100 ms press produces a real excursion rather than a
 * wobble -- which is the whole difference between scratching and nudging --
 * and long enough that a press is a push rather than a step discontinuity. */
#define ST_SCRATCH_ACCEL_MS 80u

/* And from the clamp back to a standstill once the hand stops pushing. Shorter
 * than the accel: the tape under a resting hand stops sooner than it starts. */
#define ST_SCRATCH_DECEL_MS 50u

/* Drive is signed Q16: +65536 is "pushing forward as hard as the control can
 * ask", -65536 the same backwards, 0 the hand resting without pushing. */
#define ST_SCRATCH_DRIVE_FULL 65536

typedef struct {
	int32_t  rate_q16;     /* signed; the head's velocity right now */
	int32_t  drive_q16;    /* signed; what the control is currently asking for */
	int32_t  max_rate_q16; /* the eMMC clamp for this target -- never exceeded */
	bool     engaged;      /* the gesture is live (FUNCTION held) */
	bool     coasting;     /* the hand has left; walking back to unity */
} st_scratch_t;

/*
 * Begin a gesture. `from_rate_q16` is the signed rate the transport already
 * had, so grabbing a moving tape starts from its motion rather than from a
 * standstill -- taking hold of a spinning record does not stop it dead.
 * `max_rate_q16` is the clamp for the target being grabbed:
 * ST_SCRATCH_MAX_RATE_MASTER_Q16 or ST_SCRATCH_MAX_RATE_STEM_Q16.
 */
void st_scratch_begin(st_scratch_t *s, int32_t from_rate_q16, uint32_t max_rate_q16);

/*
 * What the hand is asking for, signed and clamped to +/-ST_SCRATCH_DRIVE_FULL.
 * Called every control pass while the gesture is live -- including with 0,
 * which is not "no news" but a positive statement that the hand has stopped
 * pushing and the head should slow.
 */
void st_scratch_set_drive(st_scratch_t *s, int32_t drive_q16);

/*
 * Advance the integrator by `dt_us`. Pure: no clock of its own, so the audio
 * thread and a host test drive it identically.
 */
void st_scratch_tick(st_scratch_t *s, uint32_t dt_us);

/* The head's signed rate. Zero is a real, reachable value: the tape stopped
 * under the hand, which is a thing a player asks for and must be able to get. */
static inline int32_t st_scratch_rate_q16(const st_scratch_t *s)
{
	return s->rate_q16;
}

/*
 * End the gesture -- the hand leaves the record -- and begin COASTING back to
 * unity. Returns the signed rate at the moment of release.
 *
 * THE COAST IS NOT OPTIONAL, and this is where the spec's "smoothed through
 * zero rather than a hard sign flip" is actually earned. At release the head
 * may be running at -2.6x; ordinary playback is +1.0. Simply handing the
 * transport back its own rate would step across 3.6x of tape speed between one
 * block and the next, which is the discontinuity the whole design forbids --
 * and it would be worst exactly where a player uses it most, coming out of a
 * reverse scratch.
 *
 * So the integrator keeps running with nobody pushing it, walking to +1.0 on
 * the same ramp arithmetic as everything else. No second engine, no separate
 * release path: the hand leaving is just another target.
 */
int32_t st_scratch_release(st_scratch_t *s);

/*
 * Advance a coast toward `target_q16`, the SIGNED rate the transport will
 * resume at. Returns true while still short of it, false once it has arrived
 * -- at which point the caller stops overriding and the transport takes over
 * seamlessly, because the two numbers are equal.
 *
 * THE TARGET IS PASSED IN, NOT ASSUMED TO BE UNITY, and that is not a
 * generalisation for its own sake. The first version walked to a hard-coded
 * 1.0x. With the pitch rocker set the transport runs at up to 1.19x, so the
 * coast finished at 1.0x, the override dropped, and the rate jumped 0.19x in
 * one block -- a 19% speed step, audible as a pitch glitch, on every release
 * while pitched. The handover is only seamless if the coast ends on the number
 * the transport is actually about to use.
 *
 * SIGNED, because the head may have been reverse-toggled before the gesture
 * began. Coasting to a positive rate would silently cancel that latch: the
 * player reversed a stem, scratched it, let go, and found it playing forward
 * with no gesture to explain it. Direction returns to whatever the head was
 * in; only the POSITION is left where the scratch put it.
 */
bool st_scratch_coast(st_scratch_t *s, uint32_t dt_us, int32_t target_q16);

/* Unity, in the same Q16 the transport uses. */
#define ST_SCRATCH_UNITY_Q16 65536

/*
 * ---- the two controls, mapped to drive ------------------------------------
 *
 * Both produce the SAME quantity, which is the point: below this line the
 * transport cannot tell a rocker from a fader.
 */

/*
 * THE ROCKER. A momentary two-way switch: it reports which way, never how far.
 * So it drives at full deflection while held and not at all when released, and
 * the integrator's accel/decel is what turns press DURATION into velocity.
 * That is why press timing alone spans scratching and shuttling.
 */
static inline int32_t st_scratch_drive_from_rocker(int dir)
{
	return (dir > 0) ? ST_SCRATCH_DRIVE_FULL :
	       (dir < 0) ? -ST_SCRATCH_DRIVE_FULL : 0;
}

/*
 * THE FADER. A continuous position, but position is NOT what it means here.
 * Mapping travel to song position (bottom = start, top = end) would make fine
 * scratching impossible: the whole song across 3700 counts is about 1.4 ms of
 * audio per count, so a single count of ADC jitter would jump the head further
 * than a deliberate small movement.
 *
 * So MOVEMENT is the push, exactly like a hand on a platter: the delta since
 * the last sample, per unit time, is the drive. Stop moving and the drive is
 * zero and the head slows -- which is also what a resting hand does.
 *
 * ST_SCRATCH_FADER_FULL_CPS is the movement speed, in ADC counts per second,
 * that asks for full drive: a brisk sweep of roughly a third of the fader's
 * travel in a third of a second. Faster than that simply saturates, the way a
 * hand can only push a record so hard before it is already at full speed.
 */
#define ST_SCRATCH_FADER_TRAVEL_COUNTS 3700
#define ST_SCRATCH_FADER_FULL_CPS      3700

/*
 * Movement slower than this is ADC noise, not a hand.
 *
 * SET AGAINST THE CONTROL CADENCE, not guessed. At the ~125 Hz rate the active
 * fader is sampled during a gesture, ONE count of jitter per pass is 125
 * counts per second. 200 rejects that with margin and still admits two counts
 * per pass (250 cps), which is a real if very slow movement. Without a gate at
 * all, a resting finger would scratch by itself.
 *
 * UNMEASURED, and the one number in this header that is. The ladder bands have
 * docs/ladder-measured.json behind them; the fader channels have no equivalent
 * capture yet, so this is derived from the cadence rather than from observed
 * jitter. If a resting hand drifts on hardware, this is the constant to raise,
 * and the honest fix is to measure the fader rails the way AIN0 was measured.
 */
#define ST_SCRATCH_FADER_DEADBAND_CPS 200

int32_t st_scratch_drive_from_fader(int32_t delta_counts, uint32_t dt_us);

#endif /* STEMTAPE_PLAYER_SCRATCH_H_ */
