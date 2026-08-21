#!/usr/bin/env python3
"""
stemtape_fx_budget.py — the FX resource calculator for the ONE-RACK contract.

SUPERSEDES the five-rack/twelve-algorithm analysis. That study answered a
question the product decision then removed: it priced four stem racks plus a
global rack, each with four banks of three selectable algorithms, and found
676,140 B in mono against ~110,000 B of projected free RAM. Five racks are not
part of the product, so that figure is no longer a constraint on anything. It
is kept only in git history.

WHAT THE PRODUCT ACTUALLY IS. Exactly ONE DSP rack exists. It is inserted
either on one selected stem (STEM scope) or over the complete audible mix
(GLOBAL scope), and moving the target moves the same rack — the previously
targeted stem returns to dry. Four fixed effects, no algorithm selection, no
user macro, no per-algorithm macro storage.

Fixed musical settings are the committed reference defaults from
src/machine/fx12.ts, evaluated through src/audio/fx/banks.ts:

  Filter      macro 0.50  -> lowpass 1800 Hz, Q 0.9        (banks.ts:104)
  Delay/Echo  macro 0.50  -> 0.375 beat, feedback 0.43     (banks.ts:460-463)
  Distortion  macro 0.35  -> tanh k=15, trim 0.8425        (banks.ts:202,228)
  Gate        macro 0.50  -> 4 cycles/beat (1/16), 50% duty (banks.ts:391-397)

All four have an explicit committed default. Nothing was invented.

THE ONLY LARGE ALLOCATION IS THE ECHO DELAY LINE, and it is sized by musical
time: 0.375 beat at the slowest admitted tempo. MIN_BPM is therefore a real
decision and is stated here rather than buried.
"""

SR = 48000                  # Hz, fixed by the I2S path
FREE_RAM_BEFORE_FX = 67618  # measured, commit 37a24f3 (st18)

# ---------------------------------------------------------------------------
# THE TWO DECISIONS THAT SIZE THE ECHO
# ---------------------------------------------------------------------------
# The slowest song the firmware admits. No BPM clamp exists in the firmware
# today (st_beat_phase.c:9-30 accepts any nonzero bpm_q8), so this becomes the
# clamp. 60 BPM is a genuine musical floor and a round number; the reference
# song is 93.71 BPM, so this is comfortably below anything in use.
MIN_BPM = 60.0

# Delay storage is MONO Q15. The dry path stays fully stereo — only the echo's
# wet return is summed to mono, which halves the one allocation that matters
# and centres the repeats. A stereo line at this division and MIN_BPM would be
# 72,000 B, more than the whole free-RAM budget before the cache rework.
ECHO_CHANNELS = 1
ECHO_BYTES_PER_SAMPLE = 2   # Q15 int16

ECHO_DIVISION_BEATS = 0.375   # banks.ts:460 ratios[2] at macro 0.5
ECHO_FEEDBACK = 0.43          # banks.ts:463 0.18 + 0.5 * 0.5
ECHO_FEEDBACK_CLAMP = 0.72    # contract ceiling


def frames_per_beat(bpm):
    return SR * 60.0 / bpm


ECHO_MAX_FRAMES = int(round(ECHO_DIVISION_BEATS * frames_per_beat(MIN_BPM)))
ECHO_BUFFER_BYTES = ECHO_MAX_FRAMES * ECHO_CHANNELS * ECHO_BYTES_PER_SAMPLE

# ---------------------------------------------------------------------------
# Persistent state, per effect. Stereo where the effect is stereo.
# ---------------------------------------------------------------------------
STATE = {
    "filter": dict(
        bytes=2 * 4 * 4 + 5 * 4,
        note="biquad LP: 2 ch x (x1,x2,y1,y2) int32 + 5 Q14 coeffs"),
    "distortion": dict(
        bytes=2 * 4 * 4 + 5 * 4 + 8,
        note="tame LP biquad (2 ch) + coeffs + trim/drive"),
    "gate": dict(
        bytes=4 + 4 + 4 + 4,
        note="phase accumulator, step, current gain, div"),
    "echo": dict(
        bytes=4 + 4 + 2 * 4 + 4 + 4,
        note="write index, length, damp LP state (mono), feedback, delay frames"),
}

RACK_STATE = dict(
    bytes=4 * 4 + 4 + 4 + 4 + 4,
    note="4 x engage ramp (phase+step), scope, target stem, latch mask, momentary mask")

# Alignment: each block is 4-byte aligned; the echo buffer is int16 so it needs
# 2-byte alignment but is placed on a 4-byte boundary with the rest.
ALIGN = 4


def align_up(n, a=ALIGN):
    return (n + a - 1) // a * a


def main():
    print(__doc__)

    print("=" * 78)
    print("ECHO DELAY LINE — the only large allocation")
    print("=" * 78)
    print(f"  fixed tempo division      : {ECHO_DIVISION_BEATS} beat "
          f"(banks.ts:460 ratios[2] at the committed default macro 0.5)")
    print(f"  minimum admitted BPM      : {MIN_BPM:g}")
    print(f"  frames per beat @ MIN_BPM : {frames_per_beat(MIN_BPM):,.0f}")
    print(f"  maximum delay frames      : {ECHO_MAX_FRAMES:,}")
    print(f"  sample format             : Q15 int16, "
          f"{'mono' if ECHO_CHANNELS == 1 else 'stereo'} wet return")
    print(f"  exact delay-buffer bytes  : {ECHO_BUFFER_BYTES:,}")
    print(f"  feedback at default       : {ECHO_FEEDBACK} "
          f"(contract clamp {ECHO_FEEDBACK_CLAMP})")
    print(f"  a STEREO line would be    : {ECHO_MAX_FRAMES * 2 * 2:,} B  -- rejected")

    print()
    print("=" * 78)
    print("ONE-RACK RAM")
    print("=" * 78)
    print(f"{'component':<16}{'bytes':>10}   note")
    total_state = 0
    for k, v in STATE.items():
        total_state += v["bytes"]
        print(f"{k:<16}{v['bytes']:>10,}   {v['note']}")
    total_state += RACK_STATE["bytes"]
    print(f"{'rack':<16}{RACK_STATE['bytes']:>10,}   {RACK_STATE['note']}")
    print(f"{'':<16}{'-' * 10}")
    print(f"{'persistent':<16}{total_state:>10,}   (all four effects + rack)")
    print(f"{'echo buffer':<16}{ECHO_BUFFER_BYTES:>10,}")

    scratch = 0
    print(f"{'scratch':<16}{scratch:>10,}   none: every effect processes the "
          f"output block in place")

    fx_total = align_up(total_state) + align_up(ECHO_BUFFER_BYTES) + scratch
    pad = fx_total - (total_state + ECHO_BUFFER_BYTES + scratch)
    print(f"{'alignment pad':<16}{pad:>10,}")
    print(f"{'':<16}{'=' * 10}")
    print(f"{'TOTAL FX RAM':<16}{fx_total:>10,}")

    print()
    print("=" * 78)
    print("AGAINST THE IMAGE")
    print("=" * 78)
    after = FREE_RAM_BEFORE_FX - fx_total
    print(f"  free RAM before FX (st18, 37a24f3) : {FREE_RAM_BEFORE_FX:>9,} B")
    print(f"  FX cost                            : {fx_total:>9,} B")
    print(f"  free RAM after FX (projected)      : {after:>9,} B")
    print(f"  verdict                            : "
          f"{'FITS' if after > 0 else 'DOES NOT FIT'}")
    print()
    print("  No new permanent 8 KiB sector buffer is added for FX.")
    print("  The unified playback-cache and stale-memory cleanup continue")
    print("  independently; the RAM they reclaim is NOT spent here.")

    print()
    print("=" * 78)
    print("REMOVED FROM SCOPE — no memory is reserved for any of these")
    print("=" * 78)
    for name in ("Reel Flange", "Formant Shift", "Pitch Echo", "Granular Scatter",
                 "Reverb", "Shimmer", "Spectral Freeze", "Exciter"):
        print(f"    {name}")
    print("  There are no per-bank arenas, no algorithm arrays and no macro")
    print("  arrays: the four effects are fixed, so nothing is sized for a")
    print("  member that cannot be selected.")


if __name__ == "__main__":
    main()
