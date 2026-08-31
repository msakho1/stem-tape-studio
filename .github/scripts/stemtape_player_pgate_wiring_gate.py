#!/usr/bin/env python3
"""
Prove the planar CPU gate is advanced where it can actually finish.

WHY THIS EXISTS. The gate was first ticked from controls_diag(), which is
DTR-gated: it runs only while a serial monitor is attached. That is fine for
printing and wrong for driving an experiment -- unplugging the console mid-run
would freeze the state machine inside its TEST window, leaving the streamer
paying the divergent four-read cost on every sector until reboot. The bug is
invisible to a build and invisible to the host tests (the module is pure and
passes either way); it lives entirely in WHERE the pure function is called
from. So it is pinned here, structurally:

  1. st_pgate_tick() is called exactly once in main.c, and that call is inside
     stem_pgate_service().
  2. controls_diag() does not tick the gate and does not write the streamer's
     read pattern -- it reads and prints, nothing else.
  3. stem_pgate_service() is called from the main loop BEFORE the transfer-mode
     `continue`, so no early-out can skip it.
  4. g_stem_planar_sim is written in exactly one place, inside
     stem_pgate_service().

Exits non-zero with the reason on any of these, and writes a short report.
"""
import re
import sys


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def body_of(src, signature):
    """Return the brace-balanced body of the function whose definition line
    contains `signature`, plus the line number it starts on."""
    i = src.find(signature)
    if i < 0:
        return None, None
    start = src.find("{", i)
    if start < 0:
        return None, None
    depth, j = 0, start
    while j < len(src):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[start:j + 1], src.count("\n", 0, i) + 1
        j += 1
    return None, None


def main():
    path = sys.argv[1]
    report = sys.argv[2] if len(sys.argv) > 2 else None
    src = open(path, encoding="utf-8", errors="replace").read()

    lines = []

    # (1) exactly one tick, and it is in the service function.
    ticks = len(re.findall(r"st_pgate_tick\s*\(", src))
    if ticks != 1:
        fail("st_pgate_tick() is called %d times in main.c; the gate must be "
             "advanced from exactly one place" % ticks)
    svc, svc_line = body_of(src, "static void stem_pgate_service(void)")
    if svc is None:
        fail("stem_pgate_service() is missing -- the gate has no owner")
    if "st_pgate_tick(" not in svc:
        fail("stem_pgate_service() does not advance the gate")
    lines.append("- the single st_pgate_tick() call is inside "
                 "stem_pgate_service() (line %d)" % svc_line)

    # (4) and the service function is the only writer of the read pattern.
    writes = len(re.findall(r"atomic_set\(&g_stem_planar_sim", src))
    if writes != 1:
        fail("g_stem_planar_sim is written %d times; the gate must be its only "
             "writer" % writes)
    if "atomic_set(&g_stem_planar_sim" not in svc:
        fail("g_stem_planar_sim is written from outside stem_pgate_service()")
    lines.append("- g_stem_planar_sim is written once, in the same function")

    # (2) the diagnostic reports and does not drive.
    diag, diag_line = body_of(src, "static void controls_diag(")
    if diag is None:
        fail("controls_diag() not found -- this gate's assumptions are stale")
    if "st_pgate_tick(" in diag:
        fail("controls_diag() advances the gate; it is DTR-gated and would "
             "freeze the experiment when the console is unplugged")
    if "atomic_set(&g_stem_planar_sim" in diag:
        fail("controls_diag() writes the streamer's read pattern")
    if "STEMPGATE RESULT" not in diag:
        fail("controls_diag() no longer reports the verdict")
    lines.append("- controls_diag() (line %d) reports the verdict and neither "
                 "ticks nor writes" % diag_line)

    # (3) the call runs before anything that can skip it.
    call = src.find("stem_pgate_service();")
    if call < 0:
        fail("stem_pgate_service() is defined but never called")
    # Anchored on the MAIN LOOP's early-out specifically -- main.c has three
    # `if (g_xfer_mode)` guards (the audio block and the streamer have their
    # own), and matching the first one would compare the call against a branch
    # thousands of lines above it. Located independently of the call, so moving
    # the call below the early-out is reported as what it is rather than as a
    # missing early-out.
    xfer = src.find("if (g_xfer_mode) {\n\t\t\tled_service();")
    if xfer < 0:
        fail("the main loop's transfer-mode early-out was not found; check "
             "this gate still describes the main loop")
    cont = src.find("continue;", xfer)
    if cont < 0:
        fail("the transfer-mode early-out no longer continues; check this gate "
             "still describes the main loop")
    if call > cont:
        fail("the gate is advanced after the transfer-mode `continue`, so a "
             "transfer would skip it")
    lines.append("- stem_pgate_service() runs at main-loop line %d, before the "
                 "transfer-mode continue at line %d"
                 % (src.count("\n", 0, call) + 1, src.count("\n", 0, cont) + 1))

    print("PASS: the planar gate advances on every main-loop pass")
    for l in lines:
        print("  " + l)
    if report:
        with open(report, "w", encoding="utf-8") as f:
            f.write("### Planar CPU gate wiring\n\n")
            f.write("The gate is advanced from the main loop, not from the "
                    "DTR-gated diagnostic:\n\n")
            f.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
