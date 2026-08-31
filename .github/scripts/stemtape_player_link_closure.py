#!/usr/bin/env python3
"""
Catch "calls a function whose .c file is not in the build" without a linker.

WHY THIS EXISTS, and why it is the second script of its kind. The firmware can
only be linked by the Zephyr ARM toolchain, so a call to a function nobody
compiles is invisible until CI -- and it does not fail the way a missing
function usually does. The compile succeeds (the prototype is in the header),
the FIRST link pass runs, and the build dies with no zephyr.elf. The log tail
that survives shows only "bin=NOT-PRODUCED (the build did not reach a final
link)", which describes the symptom and names nothing.

It has already happened once: main.c's v1.2 read path called st_pl_group_block(),
st_pl_validate() and st_pl_decode_frame() on every fetch and every frame, and
src/st_planar.c was never added to CMakeLists.txt. Every one of those was an
undefined reference.

WHAT IT DOES. Reads the source list out of CMakeLists.txt, then for every
src/*.c file NOT in that list, collects the non-static functions it defines and
checks whether any LINKED source calls one. A hit is a guaranteed link failure.

WHAT IT DOES NOT DO. It is a text scanner, not a linker: it does not resolve
macros, does not know about weak symbols or libraries, and cannot see a symbol
that only a header defines. It removes one specific, repeated,
expensive-to-find failure from the CI-only set. The link stays the authority.
"""
import os
import re
import sys

# `type name(` at column 0 and not `static` -- an externally visible definition.
DEF_RE = re.compile(
    r"^(?!static\b)(?:[A-Za-z_][A-Za-z0-9_]*\s+|\*)+"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\([^;]*$",
    re.M)
CALL_RE = re.compile(r"(?<![A-Za-z0-9_])(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\(")

KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "do", "else"}


def strip_comments(src):
    out, i, n = list(src), 0, len(src)
    while i < n:
        if src[i] == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                out[i] = " "
                i += 1
        elif src[i] == "/" and i + 1 < n and src[i + 1] == "*":
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                if src[i] != "\n":
                    out[i] = " "
                i += 1
            for _ in range(2):
                if i < n:
                    out[i] = " "
                    i += 1
        else:
            i += 1
    return "".join(out)


def main():
    cmake_path = sys.argv[1]
    src_dir = sys.argv[2]

    cmake = strip_comments(open(cmake_path, encoding="utf-8").read())
    linked = set(re.findall(r"src/([A-Za-z0-9_]+\.c)", cmake))
    if not linked:
        print("FAIL: no src/*.c entries found in " + cmake_path)
        return 1

    all_c = {f for f in os.listdir(src_dir) if f.endswith(".c")}
    unlinked = sorted(all_c - linked)

    # Functions each UNLINKED file defines.
    defined_by = {}
    for f in unlinked:
        body = strip_comments(open(os.path.join(src_dir, f), encoding="utf-8").read())
        for m in DEF_RE.finditer(body):
            name = m.group("name")
            if name not in KEYWORDS:
                defined_by.setdefault(name, f)

    problems = []
    for f in sorted(linked):
        path = os.path.join(src_dir, f)
        if not os.path.exists(path):
            problems.append((f, None, "listed in CMakeLists but the file does not exist"))
            continue
        body = strip_comments(open(path, encoding="utf-8").read())
        seen = set()
        for m in CALL_RE.finditer(body):
            name = m.group("name")
            if name in defined_by and name not in seen:
                seen.add(name)
                line = body.count("\n", 0, m.start()) + 1
                problems.append((f, line, "calls %s(), defined only in src/%s, "
                                          "which is NOT in the build"
                                 % (name, defined_by[name])))

    if problems:
        print("FAIL: the build would not link")
        for f, line, msg in problems:
            where = "%s:%d" % (f, line) if line else f
            print("  %s %s" % (where, msg))
        print()
        print("  The compile SUCCEEDS for these -- the prototype is in a header.")
        print("  It is the link that dies, with no zephyr.elf and a log tail")
        print("  that says only 'bin=NOT-PRODUCED'. Add the file to")
        print("  CMakeLists.txt's source list.")
        return 1

    print("PASS: every function called by a linked source is compiled into the build")
    print("  %d sources linked, %d present but deliberately unlinked"
          % (len(linked), len(unlinked)))
    if unlinked:
        print("  unlinked: " + ", ".join(unlinked))
    return 0


if __name__ == "__main__":
    sys.exit(main())
