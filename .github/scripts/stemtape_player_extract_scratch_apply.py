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

BEGIN = "\t * ---- THE SCRATCH, APPLIED ---"
END = "\t/* The seam's duck length converted"

def main(src_path, out_path):
    src = open(src_path).read()
    try:
        i = src.index(BEGIN)
    except ValueError:
        print(f"FAIL: '{BEGIN.strip()}' not found in {src_path}")
        return 1
    # back up to the start of the enclosing comment
    i = src.rindex("\t/*", 0, i)
    try:
        j = src.index(END, i)
    except ValueError:
        print(f"FAIL: end marker not found after the scratch block")
        return 1
    block = src[i:j]
    if block.count("\n") < 40:
        print(f"FAIL: extracted block is only {block.count(chr(10))} lines -- "
              f"too small to be the real one; the test would prove nothing")
        return 1
    # CHECK THE CODE, NOT THE PROSE. The first version of this looked for the
    # names anywhere in the block, and a deliberately gutted copy passed it --
    # every name it wanted also appears in the comments explaining why the
    # calls are there. A tripwire satisfiable by a sentence is worse than none,
    # because it reads as protection. Comments are stripped first.
    code = re.sub(r"/\*.*?\*/", "", block, flags=re.S)
    code = re.sub(r"//[^\n]*", "", code)
    for needed in ("st_scratch_begin(", "st_scratch_tick(", "st_scratch_coast(",
                   "st_stream_set_reverse(", "stem_rate_q16[sk] ="):
        if needed not in code:
            print(f"FAIL: extracted block's CODE does not contain {needed}")
            return 1
    open(out_path, "w").write(block)
    print(f"extracted {block.count(chr(10))} lines of main.c's scratch application")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
