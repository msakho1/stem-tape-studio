#!/usr/bin/env python3
"""
Catch a RAM-ownership entry that names a symbol the source no longer has.

WHY THIS EXISTS. stemtape_player_ram_inventory.py classifies every writable
symbol in the linked ELF by looking it up in stemtape_player_ram_ownership
.json: first the exact `symbols` map, then the `prefixes` map, then the
`default`. That fallback chain is what makes the inventory able to describe an
ELF full of framework-generated symbols nobody wrote an entry for -- and it is
also what makes a stale entry SILENT.

It has already happened once. v1.2 song-planar renamed the read-ahead ring from
g_stem_sector_bufs to g_stem_group_bufs. The JSON still said
g_stem_sector_bufs, so the exact-symbol lookup missed, the "g_stem_" prefix
rule caught the 49,152-byte array instead, and it was filed under category
'stemtape' -- whose cap is 4,096 bytes, because that category is meant for
scalar runtime state. The budget verdict failed with

    category 'stemtape' uses 49805 B, above its 4096 B cap

which is a true statement about a wrong classification: not one byte of RAM had
moved. The array is the same allocation it always was, and its real category
(sector_pool, cap 172,032) had room for it twice over. The failure named the
consequence and not the cause, and it could only be seen after a full ARM
toolchain setup and link.

WHAT IT DOES. Every key in the `symbols` map declares where it lives, in
`declared_in`. This checks the identifier actually appears in that file. A
stale entry is reported by name, with the reminder that the inventory will not
fail on it -- it will quietly reclassify.

WHAT IT DOES NOT DO. It cannot tell you a symbol is classified WRONGLY, only
that a named one no longer exists. The prefix rules are deliberately not
checked: they exist to catch symbols nobody enumerated. Text scan, no compiler,
runs in milliseconds, and needs no ELF -- which is the whole point, since the
inventory itself cannot run without one.
"""
import json
import os
import re
import sys


def main():
    ownership_path = sys.argv[1]
    firmware_dir = sys.argv[2]

    with open(ownership_path, encoding="utf-8") as f:
        ownership = json.load(f)

    symbols = ownership.get("symbols", {})
    if not symbols:
        print("FAIL: no `symbols` map in " + ownership_path)
        return 1

    problems = []
    checked = 0

    for name, meta in sorted(symbols.items()):
        declared_in = meta.get("declared_in", "")
        # "src/main.c (function-local static)" -- take the path, drop the note.
        rel = declared_in.split()[0] if declared_in else ""
        if not rel.startswith("src/"):
            problems.append("%s: `declared_in` is %r, which names no source file "
                            "under src/ -- an entry that cannot be checked is an "
                            "entry that can go stale unnoticed"
                            % (name, declared_in))
            continue

        path = os.path.join(firmware_dir, rel)
        if not os.path.exists(path):
            problems.append("%s: `declared_in` names %s, which does not exist"
                            % (name, rel))
            continue

        with open(path, encoding="utf-8") as f:
            body = f.read()

        checked += 1
        if not re.search(r"(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])" % re.escape(name), body):
            problems.append("%s: no such identifier in %s any more (renamed? "
                            "deleted?). The inventory will NOT fail on this -- it "
                            "will fall through to the prefix rules and file the "
                            "symbol under whatever they say, which is how a "
                            "49,152-byte ring ended up counted against a 4,096-byte "
                            "cap." % (name, rel))

    if problems:
        print("FAIL: the RAM ownership map has gone stale")
        for p in problems:
            print("  " + p)
        print()
        print("  Fix the entry in " + ownership_path + " -- rename the key to")
        print("  match the source, or move it to the `_reclaimed` section if the")
        print("  allocation is genuinely gone.")
        return 1

    print("PASS: every named RAM-ownership entry still exists in its source")
    print("  %d symbols checked against their declared_in file" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
