#!/usr/bin/env python3
"""
stemtape_fx_budget.py — the FX rack resource calculator.

THIS SCRIPT IS THE ARGUMENT, NOT A SUMMARY OF ONE. Every byte figure in the FX
design comes from here, so a reviewer can change one assumption at the top and
watch the whole conclusion move. Nothing about the rack may be sized from a
number that is not computed in this file.

WHAT IT ANSWERS. The contract permits a per-stem rack on each of four stems
plus one global rack (stemPerformance.ts: `StemFxByStem = Record<StemIndex,
StemFxState>` alongside `globalFx: StemFxState`), each rack holding four banks
that may all be latched at once (`isBankActive = momentary || latched`). That
is the worst contract-permitted state, and it has to be priced BEFORE any
production wiring, not discovered at link time.

THE DOMINANT COST IS DELAY MEMORY, and it is set by musical time, not by any
constant a firmware author chooses: a tempo-locked echo of D beats needs
D * frames_per_beat frames, and frames_per_beat = sample_rate * 60 / bpm. So
the slowest song the firmware admits decides the size of every tempo-locked
line. There is no BPM clamp anywhere in the firmware today
(st_beat_phase.c:9-30 accepts any nonzero bpm_q8), which is why MIN_BPM below
is a decision this analysis has to surface rather than inherit.
"""

SR = 48000                  # Hz, fixed by the I2S path
BYTES_PER_SAMPLE = 2        # int16 delay storage (Q15)
FREE_RAM_NOW = 67618        # measured, commit 37a24f3
FREE_RAM_AFTER_CACHE = 110000   # projected by the unified-cache rework

# The slowest tempo a tempo-locked delay line must serve. Every echo/gate size
# below scales as 1/MIN_BPM.
MIN_BPM = 70.0

FRAMES_PER_BEAT_MAX = SR * 60.0 / MIN_BPM


def line(seconds, channels=1):
    """Bytes for a delay line of `seconds` at the given channel count."""
    return int(round(seconds * SR)) * channels * BYTES_PER_SAMPLE


def beats(n, channels=1):
    """Bytes for a delay line of `n` beats at MIN_BPM."""
    return int(round(n * FRAMES_PER_BEAT_MAX)) * channels * BYTES_PER_SAMPLE


# ---------------------------------------------------------------------------
# Per-algorithm delay memory, straight from src/audio/fx/banks.ts.
# `state` is the non-delay persistent state (filter histories, LFO phase,
# counters, macro, PRNG) — small and flat.
# ---------------------------------------------------------------------------
def algorithms(channels):
    c = channels
    return {
        # ---- TONE (bank 0) — no delay memory at all
        "filter":       dict(bank="TONE",   delay=0,                    state=32,  note="biquad, 2 histories/ch"),
        "exciter":      dict(bank="TONE",   delay=0,                    state=96,  note="HP split + shaper + LP + shelf"),
        "dirt":         dict(bank="TONE",   delay=0,                    state=48,  note="waveshaper + tame LP"),

        # ---- MOD (bank 1)
        # banks.ts:250 createDelay(0.05); sweep reaches 0.0006+0.0038+0.0004+0.0036
        "reelFlange":   dict(bank="MOD",    delay=line(0.010, c),       state=64,  note="0.4-8 ms modulated, 10 ms line"),
        "formantShift": dict(bank="MOD",    delay=0,                    state=128, note="4 peaking biquads"),
        "gate":         dict(bank="MOD",    delay=0,                    state=48,  note="phase counter + VCA"),

        # ---- MOTION (bank 2)
        # banks.ts:460 ratios = [0.75, 0.5, 0.375, 0.25] beats; worst 0.75
        "echo":         dict(bank="MOTION", delay=beats(0.75, c),       state=64,  note="0.75 beat @ MIN_BPM + damp LP"),
        # banks.ts:464 second tap at ratio * 1.5 -> 1.125 beats
        "pitchEcho":    dict(bank="MOTION", delay=beats(1.125, c),      state=96,  note="1.125 beat @ MIN_BPM, 2 taps"),
        # banks.ts:494 createDelay(0.5), base 0.02+3*0.017=0.071, depth <=0.021
        "scatter":      dict(bank="MOTION", delay=line(0.10, c) * 4,    state=160, note="4 lines x 100 ms + PRNG"),

        # ---- SPACE (bank 3)
        # banks.ts:552 taps 0.0297/0.0371/0.0411/0.0437
        "reverb":       dict(bank="SPACE",  delay=line(0.0437, c) * 4,  state=192, note="4-line FDN + damping"),
        # + banks.ts:574 sparkle delay 0.13 s
        "shimmer":      dict(bank="SPACE",  delay=line(0.0437, c) * 4 + line(0.13, c),
                                                                        state=256, note="FDN + 130 ms bright tail"),
        # banks.ts:634 createDelay(1), delayTime 0.08 + 0.35 * macro -> 0.43 s
        "freeze":       dict(bank="SPACE",  delay=line(0.43, c),        state=160, note="430 ms capture loop"),
    }


BANK_ORDER = ["TONE", "MOD", "MOTION", "SPACE"]


def worst_per_bank(algs):
    """Algorithms in a bank are mutually exclusive -> arena = the largest."""
    out = {}
    for b in BANK_ORDER:
        members = {k: v for k, v in algs.items() if v["bank"] == b}
        worst = max(members.items(), key=lambda kv: kv[1]["delay"] + kv[1]["state"])
        out[b] = (worst[0], worst[1]["delay"] + worst[1]["state"], members)
    return out


def report(channels, label):
    algs = algorithms(channels)
    print(f"\n{'=' * 78}\n{label}  (delay storage {channels}ch x {BYTES_PER_SAMPLE} B, MIN_BPM {MIN_BPM:g})\n{'=' * 78}")
    print(f"{'algorithm':<14}{'bank':<8}{'delay B':>10}{'state B':>9}{'total B':>10}   note")
    for name, a in algs.items():
        print(f"{name:<14}{a['bank']:<8}{a['delay']:>10,}{a['state']:>9,}"
              f"{a['delay'] + a['state']:>10,}   {a['note']}")

    wb = worst_per_bank(algs)
    print(f"\n-- per-bank arena (largest algorithm; the three are mutually exclusive)")
    rack = 0
    for b in BANK_ORDER:
        name, size, _ = wb[b]
        rack += size
        print(f"   {b:<8} arena {size:>9,} B   (worst = {name})")
    print(f"   {'RACK':<8} total {rack:>9,} B   (four banks, all latched at once)")

    print(f"\n-- worst contract-permitted state")
    for racks, what in ((1, "1 rack  (global only, or one stem)"),
                        (2, "2 racks (one stem + global)"),
                        (5, "5 racks (4 stems + global) = FULL CONTRACT")):
        tot = rack * racks
        print(f"   {what:<38} {tot:>9,} B"
              f"   vs {FREE_RAM_NOW:,} free now -> {'FITS' if tot < FREE_RAM_NOW else 'DOES NOT FIT'}"
              f" | vs {FREE_RAM_AFTER_CACHE:,} projected -> {'FITS' if tot < FREE_RAM_AFTER_CACHE else 'DOES NOT FIT'}")
    return rack


def constrained_rack(min_bpm, echo_max_beats, freeze_s, scatter_line_s, channels=1):
    """
    One rack under explicit restrictions, so the decision is a number rather
    than a shrug. Only the four knobs that actually move the total are exposed:
    everything else is fixed by the reference.
    """
    fpb = SR * 60.0 / min_bpm
    c = channels
    b = lambda n: int(round(n * fpb)) * c * BYTES_PER_SAMPLE
    s = lambda sec: int(round(sec * SR)) * c * BYTES_PER_SAMPLE

    tone = 96
    mod = s(0.010) + 64
    # pitchEcho is the worst MOTION member: second tap at 1.5x the base division
    motion = max(b(echo_max_beats) + 64,
                 b(echo_max_beats * 1.5) + 96,
                 s(scatter_line_s) * 4 + 160)
    space = max(s(0.0437) * 4 + 192,
                s(0.0437) * 4 + s(0.13) + 256,
                s(freeze_s) + 160)
    return tone + mod + motion + space, dict(TONE=tone, MOD=mod, MOTION=motion, SPACE=space)


def sensitivity():
    print(f"\n{'=' * 78}\nWHAT ACTUALLY FITS — sensitivity of ONE rack to the four knobs\n{'=' * 78}")
    print("Reference values: MIN_BPM 70, echo 1.125 beat, freeze 0.43 s, scatter 0.10 s\n")
    print(f"{'MIN_BPM':>8}{'echo beat':>11}{'freeze s':>10}{'scatter s':>11}"
          f"{'1 rack B':>11}{'x5 B':>11}   verdict (1 rack vs 110k projected)")
    rows = [
        (70,  0.75,  0.43, 0.10),
        (70,  0.375, 0.43, 0.10),
        (70,  0.25,  0.20, 0.06),
        (100, 0.25,  0.20, 0.06),
        (100, 0.25,  0.15, 0.05),
        (120, 0.25,  0.15, 0.05),
        (120, 0.125, 0.12, 0.04),
    ]
    for bpm, eb, fz, sc in rows:
        tot, _ = constrained_rack(bpm, eb, fz, sc)
        v = "fits" if tot < FREE_RAM_AFTER_CACHE else "NO"
        v5 = "fits" if tot * 5 < FREE_RAM_AFTER_CACHE else "NO"
        print(f"{bpm:>8}{eb:>11}{fz:>10}{sc:>11}{tot:>11,}{tot * 5:>11,}   "
              f"1x {v}, 5x {v5}")
    print("\n  Even the most aggressive row leaves the FULL five-rack contract far")
    print("  out of reach. The binding constraint is not any one algorithm: it is")
    print("  that the contract permits FIVE independent racks to be latched at")
    print("  once, and delay memory does not share between racks that can sound")
    print("  simultaneously.")


def main():
    print(__doc__)
    mono = report(1, "MONO delay storage (wet path summed to mono, dry stays stereo)")
    stereo = report(2, "STEREO delay storage (a literal port of the Web Audio graph)")
    sensitivity()

    print(f"\n{'=' * 78}\nCONCLUSION\n{'=' * 78}")
    print(f"  one rack, mono delay storage   : {mono:,} B")
    print(f"  one rack, stereo delay storage : {stereo:,} B")
    print(f"  full contract (5 racks), mono  : {mono * 5:,} B")
    print(f"  full contract (5 racks), stereo: {stereo * 5:,} B")
    print(f"  free RAM today                 : {FREE_RAM_NOW:,} B")
    print(f"  free RAM after unified cache   : ~{FREE_RAM_AFTER_CACHE:,} B (projected, not yet built)")
    print()
    print("  The MOTION bank alone dominates: a tempo-locked echo is sized by")
    print(f"  musical time, and 1.125 beats at {MIN_BPM:g} BPM is "
          f"{beats(1.125, 1):,} B in mono.")
    print("  Raising MIN_BPM shrinks it linearly; nothing else moves it.")


if __name__ == "__main__":
    main()
