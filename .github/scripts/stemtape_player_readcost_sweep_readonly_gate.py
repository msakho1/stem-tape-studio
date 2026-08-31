#!/usr/bin/env python3
"""
Stem Tape Player -- read-cost sweep READ-ONLY gate (fail-closed).

The 'M' command in main.c measures how a sector read's cost splits between a
fixed per-read part and a part that scales with size. That number decides
whether per-track reverse playback is affordable at all: storing each stem in
its own plane only helps if a quarter-size read costs about a quarter, which
turns entirely on whether the 1763 us start-bit hunt is paid per BLOCK or per
READ (78% duty versus 153%).

A benchmark that answers that question must not also mutate storage. The SP-1
already retired one benchmark command ('Y') partly because it carried a write
path; its replacement is read-only by construction and this gate is what keeps
it that way. It extracts the body of the `cmd == 'M'` branch and fails if any
mutating call appears inside it.

Deliberately a textual check on the branch body rather than a whole-file
grep: main.c legitimately calls every one of these functions elsewhere (the
'W' write path, the 'F' durability barrier, the metadata saver), so a
file-wide search would either pass vacuously or fail permanently. The question
is only ever "does the SWEEP write", and that is a question about one branch.

Usage: stemtape_player_readcost_sweep_readonly_gate.py <main.c> [out-report.md]
"""
import sys

# Anything that writes, erases, flushes or schedules a persist. If the sweep
# ever needs one of these, that is a design change to argue for explicitly,
# not a line to slip past a gate.
BANNED = (
    "emmc_write_blocks",
    "emmc_trim",
    "emmc_cache_flush",
    "emmc_cache_flush_try",
    "emmc_erase",
    "xfer_v11_write",
    "st_ab_session_check_write",
    "g_meta_save_req",
    "emmc_pon_power_off_short",
)

# The sweep must still actually READ and still REPORT, or a gate that only
# forbids writes would happily pass a branch gutted to a printk.
REQUIRED = ("emmc_read_blocks", "STEMRC blocks=")

# Timestamps needed to measure an interval. Counted, not merely found: an
# earlier version of this gate required DWT->CYCCNT to be "present", and a
# mutation that replaced the closing timestamp with a constant sailed through
# because the opening one still matched. One clock reading measures nothing.
MIN_CYCCNT_READS = 2


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    try:
        src = open(path, encoding="utf-8", errors="ignore").read()
    except OSError as exc:
        print(f"GATE FAILED: cannot read {path}: {exc}")
        return 1

    start = src.find("cmd == 'M'")
    if start < 0:
        print("GATE FAILED: the read-cost sweep ('M') is not present in main.c")
        return 1
    # The branch runs to the next dispatch arm. 'F' follows it today; fall back
    # to the end of the dispatcher if that ever moves, so the gate errs toward
    # scanning MORE text rather than silently scanning none.
    end = src.find("cmd == 'F'", start)
    if end < 0:
        end = min(len(src), start + 8000)
    body = src[start:end]

    bad = [name for name in BANNED if name in body]
    missing = [name for name in REQUIRED if name not in body]
    if body.count("DWT->CYCCNT") < MIN_CYCCNT_READS:
        missing.append(
            f"at least {MIN_CYCCNT_READS} DWT->CYCCNT reads "
            f"(found {body.count('DWT->CYCCNT')}; an interval needs two)")

    lines = [
        "# Stem Tape read-cost sweep -- read-only gate",
        "",
        f"Sweep body: {len(body)} bytes of `main.c`, "
        f"between `cmd == 'M'` and the next dispatch arm.",
        "",
    ]
    if bad:
        lines.append("## Result\n\nGATE FAILED -- the sweep mutates storage:\n")
        lines += [f"  - `{name}`" for name in bad]
        print("\n".join(lines))
        print(f"GATE FAILED: forbidden in the read-cost sweep: {', '.join(bad)}")
        _write(lines)
        return 1
    if missing:
        lines.append(
            "## Result\n\nGATE FAILED -- the sweep no longer measures a real "
            "read:\n"
        )
        lines += [f"  - missing `{name}`" for name in missing]
        print("\n".join(lines))
        print(f"GATE FAILED: the sweep is missing: {', '.join(missing)}")
        _write(lines)
        return 1

    lines.append(
        "## Result\n\nGATE PASSED -- the sweep performs real timed reads and "
        f"none of the {len(BANNED)} mutating calls appear in its body."
    )
    print("\n".join(lines))
    _write(lines)
    return 0


def _write(lines):
    if len(sys.argv) >= 3:
        try:
            with open(sys.argv[2], "w", encoding="utf-8") as fh:
                fh.write("\n".join(lines) + "\n")
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
