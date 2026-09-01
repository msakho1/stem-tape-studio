#!/usr/bin/env python3
"""
sp1-readcost-sweep.py -- run the firmware's 'M' read-cost sweep and fit it.

WHAT THIS DECIDES
=================
Stem Tape's streamer reads one stem's groups at a time.  The cost of a read
is modelled (src/st_readcost.h) as

    us = F + P * blocks

and the ONLY thing that decides the read batch size R is the ratio between
those two terms.  F is paid once per read whatever its size -- the CMD18
setup, its R1 response, CMD12 and its R1b busy wait.  P is paid per 512-byte
block.  A large F means bigger reads are much cheaper per byte and R should
rise; a small F means R barely matters and the surplus has to come from
somewhere else entirely.

F has never been measured on this firmware.  One read size cannot separate
F from P -- two unknowns, one equation -- which is why the firmware sweeps
five sizes (1, 2, 4, 8, 16 blocks, 24 reps each) and this script fits them.

READ-ONLY.  The 'M' command reads blocks the 'R' verb is already allowed to
read, stepping the address every rep so the card's own read cache cannot
serve a repeat and report a cost the streamer will never see.  It writes,
erases and flushes nothing.  Transfer mode pauses playback, so the sweep
cannot steal bandwidth from a live stream and skew its own measurement.

    THAT PAUSE IS ALSO THE ONE THING TO REMEMBER WHEN READING THE RESULT.
    The 2781 us/read figure from the st49 capture was measured DURING
    playback and therefore contains the audio thread's ~35% preemption.
    These numbers do not.  Expect blocks=8 to land near 1800 us, not 2781.
    If it comes back at ~2781 the preemption accounting behind every budget
    in this project is wrong, and that is worth knowing before anything is
    migrated.

USAGE
=====
    pip install pyserial
    python3 tools/sp1-readcost-sweep.py                    # auto-detect port
    python3 tools/sp1-readcost-sweep.py --port /dev/ttyACM0
    python3 tools/sp1-readcost-sweep.py --raw sweep.log    # fit a saved log

The device must be powered, connected by USB, NOT recording, and running a
build that carries the 'M' verb (st50 or later -- the tag prints at boot and
on every LOOPER line).
"""

import argparse
import glob
import re
import sys
import time

# ---- the wire protocol, from firmware/stemtape_player/src/main.c -----------
ENTER_MAGIC = b"SP1XFER!"   # xfer_service(): 8-byte enter magic
CMD_SWEEP = b"M"            # read-size sweep; acks with 'm'
CMD_EXIT = b"X"             # leave transfer mode; commits nothing

STEMRC = re.compile(
    r"STEMRC blocks=(\d+) n=(\d+) avg_us=(\d+) worst_us=(\d+) "
    r"hunt_us=(\d+) dma_us=(\d+) crc_us=(\d+)"
)

# ---- the audio geometry the fit is applied to (v1.3, 16-bit) --------------
GROUP_BLOCKS = 4            # 2048-byte group = 4 eMMC blocks
FR_PER_GROUP_16 = 510       # (2048 - 8) / 4
FR_PER_GROUP_24 = 340       # (2048 - 8) / 6, for the comparison line
STEMS = 4
SR = 48000.0
MAX_PITCH = 1.15535         # +2.5 semitones, ST_PITCH_MAX_HALF
RAM_RING_TODAY = 6 * 2048 * STEMS   # 49,152 B: G=6 slots per stem


def find_port():
    pats = ("/dev/tty.usbmodem*", "/dev/ttyACM*", "/dev/tty.usbserial*")
    hits = [p for pat in pats for p in glob.glob(pat)]
    if not hits:
        sys.exit("no serial port found; pass --port explicitly")
    if len(hits) > 1:
        sys.exit("several ports found, pass one with --port:\n  " + "\n  ".join(hits))
    return hits[0]


def run_sweep(port, timeout_s=30.0):
    try:
        import serial
    except ImportError:
        sys.exit("pyserial missing:  pip install pyserial")

    # Opening the port asserts DTR, which is what gates the device's console
    # output (controls_diag() and every printk share this stream).
    with serial.Serial(port, 115200, timeout=0.2) as ser:
        time.sleep(0.3)
        ser.reset_input_buffer()

        ser.write(ENTER_MAGIC)
        ser.flush()
        # Both the audio thread and the streamer must acknowledge quiesce
        # before any command dispatches; that takes one audio block (5.3 ms)
        # and one streamer pass (~1 ms).  A generous margin costs nothing.
        time.sleep(0.25)

        ser.write(CMD_SWEEP)
        ser.flush()

        buf, deadline = "", time.time() + timeout_s
        while time.time() < deadline:
            chunk = ser.read(4096)
            if chunk:
                buf += chunk.decode("utf-8", "replace")
                if "STEMRC sweep end" in buf:
                    break
        else:
            print("WARNING: timed out waiting for 'sweep end'", file=sys.stderr)

        ser.write(CMD_EXIT)     # never commits; the 15 s idle timeout also exits
        ser.flush()
        return buf


def fit(rows):
    """Least squares us = F + P*blocks.  Mirrors st_readcost_fit()."""
    n = len(rows)
    if n < 2:
        sys.exit("need at least two distinct block counts to separate F from P")
    sx = sum(b for b, _ in rows)
    sy = sum(u for _, u in rows)
    sxx = sum(b * b for b, _ in rows)
    sxy = sum(b * u for b, u in rows)
    den = n * sxx - sx * sx
    if den == 0:
        sys.exit("degenerate fit: all samples at one block count")
    p = (n * sxy - sx * sy) / den
    f = (sy - p * sx) / n
    # st_readcost_fit() refuses a negative slope on purpose: bigger reads
    # costing less is a broken measurement, and reporting it as a tiny fixed
    # cost would argue for exactly the change this is meant to gate.
    if p <= 0:
        sys.exit(f"FIT REJECTED: per-block slope {p:.1f} us is not positive")
    return f, p


def analyse(F, P):
    """What the fit means for the read batch size R, at 16-bit storage."""
    print(f"\n{'=' * 72}\nFIT:  us = {F:.0f} + {P:.1f} * blocks")
    print(f"      a {GROUP_BLOCKS}-block group costs {F + P*GROUP_BLOCKS:.0f} us, "
          f"of which {100*F/(F+P*GROUP_BLOCKS):.0f}% is fixed overhead")
    print("=" * 72)

    if F < 200:
        verdict = ("SMALL. Bigger reads buy little; R barely matters and the\n"
                   "         surplus has to come from audio-thread CPU instead.")
    elif F < 700:
        verdict = "MODERATE. R=3..4 is worth taking; the gain is real but not decisive."
    else:
        verdict = "LARGE. Batch size is the dominant lever; take R as high as RAM allows."
    print(f"\n  F is {verdict}\n")

    groups_s = SR / FR_PER_GROUP_16          # per stem, per second, at 1x
    print(f"{'R':>2} {'read':>6} {'reads/s':>8} {'us/read':>8} "
          f"{'unity':>7} {'+2.5st':>7} {'+rev+FX':>8} {'RAM':>7} {'runway':>8}")
    best = None
    for R in (1, 2, 3, 4, 6):
        if 6 % R:
            continue                          # R must divide the ring depth G=6
        blocks = R * GROUP_BLOCKS
        us = F + P * blocks
        rps = STEMS * groups_s / R
        duty = rps * us * 1e-6
        # Reverse is free in whole-song planar: every stem already reads
        # alone, so a divergent stem changes nothing about the read plan.
        d_unity = duty * 100
        d_pitch = duty * MAX_PITCH * 100
        # Streamer CPU is 0.689 x wall duty (calibrated from the st49 capture:
        # str=54% at 78.4% duty).  Audio at 16-bit: 30.5 base + 7.5 varispeed
        # + 3 reverse + 13.5 FX worst arm.  Main/midi/diag 5.
        total = 0.689 * duty * MAX_PITCH * 100 + 30.5 + 7.5 + 3 + 13.5 + 5
        ram = 6 * 2048 * STEMS
        runway_ms = (6 - R) * FR_PER_GROUP_16 / 48.0
        flag = ""
        if runway_ms < 15.0:
            flag = "  <- runway too thin (worst observed read 9.3 ms)"
        elif total < 100 and (best is None or total < best[1]):
            best = (R, total)
        print(f"{R:>2} {blocks:>4}bl {rps:>8.1f} {us:>8.0f} "
              f"{d_unity:>6.1f}% {d_pitch:>6.1f}% {total:>7.1f}% "
              f"{ram/1024:>6.0f}K {runway_ms:>6.1f}ms{flag}")

    print("\n  unity/+2.5st  = streamer WALL duty (share of wall clock inside reads)")
    print("  +rev+FX       = TOTAL CPU for the finished milestone's worst case:")
    print("                  one reversed stem + max pitch-up + worst FX arm + loop")
    print("  runway        = (G-R) groups of read-ahead, G=6.  Must comfortably")
    print("                  exceed the worst observed single read (9316 us).")
    if best:
        print(f"\n  => R={best[0]} minimises worst-case total CPU at {best[1]:.1f}%")
    else:
        print("\n  => no R clears 100% on this fit; the deficit is audio-thread CPU,")
        print("     not batch size, and the next lever is the FX rack.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="serial device (auto-detected if omitted)")
    ap.add_argument("--raw", help="fit a previously captured log instead of running")
    ap.add_argument("--save", default="sweep.log", help="where to write the raw log")
    args = ap.parse_args()

    if args.raw:
        text = open(args.raw).read()
    else:
        port = args.port or find_port()
        print(f"port: {port}", file=sys.stderr)
        text = run_sweep(port)
        with open(args.save, "w") as fh:
            fh.write(text)
        print(f"raw log: {args.save}", file=sys.stderr)

    rows = []
    for line in text.splitlines():
        m = STEMRC.search(line)
        if m:
            b, n, avg, worst, hunt, dma, crc = (int(g) for g in m.groups())
            rows.append((b, avg))
            print(f"  blocks={b:<3} n={n:<3} avg={avg:>6}us worst={worst:>6}us "
                  f"hunt={hunt:>5} dma={dma:>5} crc={crc:>5}")
    if not rows:
        sys.exit("no STEMRC lines found -- is the console open and the build st50+?")

    F, P = fit(rows)
    analyse(F, P)

    # THE SELF-CHECK.  See this file's own header: the sweep runs with
    # playback stopped, so its costs carry no audio-thread preemption, while
    # the 2781 us/read from the st49 capture does.  ~35% audio CPU predicts
    # roughly 1800 us here for the same 8-block read.
    eight = [u for b, u in rows if b == 8]
    if eight:
        got = eight[0]
        print(f"\n  SELF-CHECK: blocks=8 measured {got} us; live playback saw 2781 us.")
        if got > 2400:
            print("  ** The two are close, so the live figure contains little or no")
            print("     preemption -- the ~35% audio share assumed by every budget")
            print("     in this project is WRONG and they all need redoing. **")
        else:
            print(f"  Implied non-streamer share of live wall time: "
                  f"{100*(1 - got/2781):.0f}% (expected ~35%).")


if __name__ == "__main__":
    main()
