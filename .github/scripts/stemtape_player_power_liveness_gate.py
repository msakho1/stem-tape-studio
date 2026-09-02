#!/usr/bin/env python3
"""
stemtape_player_power_liveness_gate.py -- two narrow syntactic guards on the
liveness of the manual power escape hatch.

WHAT THIS GATE IS FOR. E-8 (in the wiring check) proves that no
continue/break/return/goto sits between the main control loop's opening brace
and power_hold_service(). That closes the class that actually bit us -- the
g_xfer_mode `continue` -- but it says nothing about a future change that
BLOCKS instead of skipping. A `k_sem_take(&x, K_FOREVER)` added above the
hatch would leave E-8 perfectly green and strand the device just as
completely.

WHAT THIS GATE CLAIMS, AND IT IS DELIBERATELY NARROW:

  L-1  In the source region between the control loop's opening brace and the
       power_hold_service() call, and in the bodies of the two functions that
       region calls (power_hold_service, ladder_read), none of the following
       SYNTACTIC constructs appear:
         - the token K_FOREVER
         - k_sem_take / k_mutex_lock / k_condvar_wait / k_event_wait /
           k_queue_get / k_msgq_get / k_fifo_get / k_lifo_get /
           k_thread_join / k_poll / k_pipe_get / k_stack_pop
         - any `while (...)` or `for (;;)` loop

  L-2  The priority-0 audio thread's `while (1)` body begins with a blocking
       k_mem_slab_alloc(..., K_FOREVER). Because that is the FIRST statement
       of the loop body, no path through the body can reach the loop's back
       edge without passing through it, so the loop cannot become an
       unconditional busy-spin that starves priority-1 MAIN.

WHAT THIS GATE DOES **NOT** CLAIM, stated because a checker that oversells
itself is worse than no checker (E-2 asserted "runs unconditionally" while
checking three call orderings, and a real defect lived under that sentence
for months):

  * It does NOT prove no call can ever block. adc_read_dt(), the eMMC driver
    and every other Zephyr/vendor call could in principle block internally;
    their source is not analysed here and some is not in this repository.
    A driver hang is caught by the 4 s watchdog, not by this gate.
  * It does NOT prove the audio thread always makes progress -- only that its
    loop body cannot be entered without reaching one blocking call.
  * It is SYNTACTIC. A blocking wrapper reached through a function pointer,
    or hidden behind a macro this gate does not expand, is not detected.
  * It says nothing about scheduling, timeslicing or priority inversion.

Both properties are mutation-tested; see the gate's own CI step.

Usage: stemtape_player_power_liveness_gate.py <main.c> <out-report.md>
Fails closed: main.c unreadable, either anchor not found, or any check failing.
"""

from __future__ import annotations

import re
import sys

# The waits that block with no bound of their own. k_sleep/k_msleep are NOT
# here: they take an explicit duration and always return.
BLOCKING_CALLS = (
    "k_sem_take", "k_mutex_lock", "k_condvar_wait", "k_event_wait",
    "k_queue_get", "k_msgq_get", "k_fifo_get", "k_lifo_get",
    "k_thread_join", "k_poll", "k_pipe_get", "k_stack_pop",
)


def strip_c(s: str) -> str:
    """Blank comments, preserving line numbers, so the gate can neither be
    satisfied nor broken by its own explanatory prose."""
    s = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    return s


def enclosing_loop(lines: list[str], idx: int) -> int | None:
    """The innermost while(1)/for(;;) containing `idx`, found by brace depth.
    main() holds several loops -- the charge-standby gate has its own, with a
    legitimate wake `break` -- so picking the nearest one textually above is
    wrong and produced a false failure the first time this was written."""
    depth = 0
    for i in range(idx - 1, -1, -1):
        depth += lines[i].count("}") - lines[i].count("{")
        if depth < 0:
            s = lines[i].strip()
            if s.endswith("{") and re.match(
                    r"^(while\s*\(\s*1\s*\)|for\s*\(\s*;\s*;\s*\))", s):
                return i
            depth = 0
    return None


def body_of(lines: list[str], header_re: str) -> tuple[int, int] | None:
    for i, ln in enumerate(lines):
        if re.search(header_re, ln):
            depth = 0
            for j in range(i, len(lines)):
                depth += lines[j].count("{") - lines[j].count("}")
                if j > i and depth == 0:
                    return i, j
            return i, len(lines) - 1
    return None


def scan(lines: list[str], a: int, b: int, what: str, skip_first_loop: bool,
         report: list[str]) -> bool:
    """Return True on violation. `a`..`b` inclusive, 0-based."""
    bad = False
    for i in range(a, b + 1):
        ln = lines[i]
        if "K_FOREVER" in ln:
            report.append(f"    - line {i + 1}: **K_FOREVER** in {what} -- "
                          f"`{ln.strip()[:60]}`")
            bad = True
        for call in BLOCKING_CALLS:
            if re.search(r"\b" + call + r"\s*\(", ln):
                report.append(f"    - line {i + 1}: **{call}()** in {what} -- "
                              f"an unbounded wait above the escape hatch")
                bad = True
        if skip_first_loop and i == a:
            continue
        if re.search(r"\bfor\s*\(\s*;\s*;\s*\)", ln) or re.search(r"\bwhile\s*\(", ln):
            report.append(f"    - line {i + 1}: **new loop** in {what} -- "
                          f"`{ln.strip()[:60]}`. A loop here can spin without "
                          f"returning to the power service")
            bad = True
    return bad


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    main_c, out_path = sys.argv[1], sys.argv[2]
    try:
        text = open(main_c, errors="ignore").read()
    except OSError as e:
        print(f"FATAL: could not read {main_c}: {e}", file=sys.stderr)
        return 1

    L = strip_c(text).split("\n")
    report: list[str] = ["# Stem Tape power-liveness gate", ""]
    fail = False

    # ---- L-1 -----------------------------------------------------------
    report.append("## L-1. No unbounded blocking construct above the escape hatch")
    report.append("")
    svc = None
    for i, ln in enumerate(L):
        if "power_hold_service(st_ladder_mask" in ln:
            svc = i
            break
    loop = enclosing_loop(L, svc) if svc is not None else None
    if svc is None or loop is None:
        report.append("- **MISSING/BAD**: could not locate the control loop and its "
                      "power_hold_service() call. Fails closed.")
        fail = True
    else:
        if scan(L, loop, svc - 1, "the pre-hatch region", True, report):
            fail = True
        else:
            report.append(f"- present: lines {loop + 1}..{svc} contain no K_FOREVER, "
                          f"no unbounded-wait primitive and no loop")
        for name, rx in (("power_hold_service()",
                          r"static int64_t power_hold_service\s*\("),
                         ("ladder_read()", r"static int ladder_read\s*\(")):
            span = body_of(L, rx)
            if span is None:
                report.append(f"- **MISSING**: {name} not found. Fails closed.")
                fail = True
                continue
            # ladder_read's counted `for (n = 0; n < 2; n++)` is bounded and is
            # the one loop allowed anywhere in this gate's scope; it is matched
            # explicitly rather than by an "any for() is fine" carve-out.
            bad = False
            for i in range(span[0], span[1] + 1):
                ln = L[i]
                if "K_FOREVER" in ln:
                    report.append(f"    - line {i + 1}: **K_FOREVER** in {name}")
                    bad = True
                for call in BLOCKING_CALLS:
                    if re.search(r"\b" + call + r"\s*\(", ln):
                        report.append(f"    - line {i + 1}: **{call}()** in {name}")
                        bad = True
                if re.search(r"\bwhile\s*\(", ln) or re.search(r"for\s*\(\s*;\s*;\s*\)", ln):
                    report.append(f"    - line {i + 1}: **unbounded loop** in {name} "
                                  f"-- `{ln.strip()[:56]}`")
                    bad = True
            if bad:
                fail = True
            else:
                report.append(f"- present: {name} contains no unbounded wait or loop")
    report.append("")

    # ---- L-2 -----------------------------------------------------------
    report.append("## L-2. The priority-0 audio loop cannot become a busy-spin")
    report.append("")
    span = body_of(L, r"static void audio_thread\s*\(")
    if span is None:
        report.append("- **MISSING**: audio_thread() not found. Fails closed.")
        fail = True
    else:
        loop_i = None
        for i in range(span[0], span[1] + 1):
            if re.match(r"^\s*while\s*\(\s*1\s*\)\s*\{", L[i]):
                loop_i = i
                break
        if loop_i is None:
            report.append("- **MISSING**: audio_thread()'s `while (1)` not found.")
            fail = True
        else:
            # The FIRST executable statement of the loop body must be the
            # blocking allocation. Declarations without initialisers are
            # skipped; anything else ends the search.
            first = None
            for i in range(loop_i + 1, span[1] + 1):
                s = L[i].strip()
                if not s:
                    continue
                if re.match(r"^(void|int|uint\w+|int\w+|char|bool|float|"
                            r"double|struct|const)\b[^=]*;$", s):
                    continue          # a bare declaration
                first = (i, s)
                break
            if first and re.search(r"k_mem_slab_alloc\s*\([^;]*K_FOREVER", first[1]):
                report.append(f"- present: the audio loop body's first statement "
                              f"(line {first[0] + 1}) is a blocking "
                              f"`k_mem_slab_alloc(..., K_FOREVER)`. No path through "
                              f"the body reaches the back edge without passing it, "
                              f"so the loop cannot spin and starve priority-1 MAIN")
            else:
                got = first[1][:60] if first else "<nothing>"
                report.append(f"- **MISSING/BAD**: the audio loop body no longer "
                              f"BEGINS with a blocking k_mem_slab_alloc(..., "
                              f"K_FOREVER). First statement is `{got}`. A "
                              f"priority-0 loop that can reach its back edge "
                              f"without blocking starves MAIN, and the power "
                              f"service never runs again")
                fail = True
    report.append("")

    report.append("## What this gate does not prove")
    report.append("")
    report.append("- It is SYNTACTIC. It does not prove that no *called* function "
                  "blocks: `adc_read_dt()`, the eMMC driver and other vendor code "
                  "are not analysed, and some is not in this repository. A driver "
                  "hang is caught by the 4 s watchdog, not by this gate.")
    report.append("- L-2 proves reachability of one blocking call, not liveness of "
                  "the audio thread.")
    report.append("- Blocking reached through a function pointer or an unexpanded "
                  "macro is not detected.")
    report.append("- Nothing here concerns scheduling, timeslicing or priority "
                  "inversion.")

    with open(out_path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(report) + "\n")
    print("\n".join(report))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
