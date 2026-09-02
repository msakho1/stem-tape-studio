#!/usr/bin/env python3
"""Lift main.c's scratch-application block into a compilable .inc.

The block decides which heads move, in which direction, at what speed. It is
the one piece of the scratch that lives only in main.c, and a hand-written
copy in a test would be a second implementation that goes stale silently. So
the test compiles THESE LINES, taken from the file at build time.

Fails loudly if the markers are missing or the block is implausibly small --
an empty extract would make the test pass by testing nothing.
"""
import re
import sys

# Each extractable region: a name, the line that opens it, and the line that
# closes it. Adding a region here is how a new block becomes testable; the
# per-region `must` list is what stops a gutted block passing.
REGIONS = {
    "apply": {
        "begin": "\t * ---- THE SCRATCH, APPLIED ---",
        "end":   "\t/* The seam's duck length converted",
        "min_lines": 40,
        "must": ("st_scratch_begin(", "st_scratch_tick(", "st_scratch_coast(",
                  "st_stream_set_reverse(", "stem_rate_q16[sk] ="),
    },
    "loopwrap": {
        "begin": "\t\tif (lp_on && lp_end > lp_lo) {",
        "end":   "\t\t/* Mirror the (audio-thread-exclusive) underrun episode",
        "min_lines": 20,
        "must": ("hd->reverse", "st_stream_seek(hd, lp_end - 1u)",
                  "st_stream_seek(hd, lp_lo)"),
        "verbatim_begin": True,
    },
}

def main(src_path, out_path, region="apply"):
    if region not in REGIONS:
        print(f"FAIL: unknown region '{region}'")
        return 1
    r = REGIONS[region]
    src = open(src_path).read()
    try:
        i = src.index(r["begin"])
    except ValueError:
        print(f"FAIL: '{r['begin'].strip()}' not found in {src_path}")
        return 1
    if not r.get("verbatim_begin"):
        # back up to the start of the enclosing comment
        i = src.rindex("\t/*", 0, i)
    try:
        j = src.index(r["end"], i)
    except ValueError:
        print(f"FAIL: end marker for region '{region}' not found")
        return 1
    block = src[i:j]
    if block.count("\n") < r["min_lines"]:
        print(f"FAIL: extracted '{region}' block is only {block.count(chr(10))} "
              f"lines -- too small to be the real one; the test would prove nothing")
        return 1
    # CHECK THE CODE, NOT THE PROSE. The first version of this looked for the
    # names anywhere in the block, and a deliberately gutted copy passed it --
    # every name it wanted also appears in the comments explaining why the
    # calls are there. A tripwire satisfiable by a sentence is worse than none,
    # because it reads as protection. Comments are stripped first.
    code = re.sub(r"/\*.*?\*/", "", block, flags=re.S)
    code = re.sub(r"//[^\n]*", "", code)
    for needed in r["must"]:
        if needed not in code:
            print(f"FAIL: extracted '{region}' block's CODE does not contain {needed}")
            return 1
    open(out_path, "w").write(block)
    print(f"extracted {block.count(chr(10))} lines of main.c's '{region}' block")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2],
                   sys.argv[3] if len(sys.argv) > 3 else "apply"))
