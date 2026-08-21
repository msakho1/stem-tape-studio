#!/usr/bin/env python3
"""
Stem Tape Player -- RAM inventory, generated from the real Zephyr ELF.

Reads the artefacts the "Collect toolchain analysis" step already produces
(nm -n -S, readelf -W -S, the build log's memory-region report) and emits:

  * a human-readable markdown breakdown
  * a machine-readable JSON inventory
  * a pass/fail verdict against committed per-category and total budgets

WHY THIS EXISTS. Every RAM figure in this repository up to now has been the
linker's single "RAM: N bytes" line plus hand-reading of the source. That is
enough to notice a regression and not enough to decide anything: it cannot
tell you which subsystem owns a pool, whether two pools are alive at the same
time, or whether a size was ever measured rather than guessed. This produces
the per-symbol picture instead, reconciled against the linker's own total so
nothing can hide in "framework overhead".

The ownership metadata (owner, lifetime, phases, threads, evidence) is NOT
inferred -- it is committed data in ram-ownership.json beside this script, so
a claim about a buffer's lifetime is reviewable in a diff rather than being
re-derived from prose each time.

Usage:
  stemtape_player_ram_inventory.py <nm.txt> <readelf.txt> <build.log> \
      <ownership.json> <budgets.json> <out.md> <out.json>

Fails closed: unparsable input, a budget breach, or a reconciliation gap
wider than the tolerance is a non-zero exit.
"""

import json
import re
import sys

# Symbols smaller than this are aggregated rather than listed individually.
MIN_LISTED = 128

# The reconciliation tolerance, in bytes. Sized symbols do not account for
# every byte of a section: linker-inserted alignment padding and a handful of
# unsized assembly symbols land between them. A gap larger than this means
# something real is unaccounted for, not rounding.
RECONCILE_TOLERANCE = 4096


def die(msg):
    print("RAM INVENTORY: FAIL -- " + msg, file=sys.stderr)
    sys.exit(1)


def parse_sections(path):
    """Writable RAM sections from `readelf -W -S`: name -> (addr, size, flags)."""
    out = {}
    pat = re.compile(
        r"^\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+"
        r"([0-9a-fA-F]+)\s+\S+\s+(\S*)"
    )
    with open(path) as f:
        for line in f:
            m = pat.match(line)
            if not m:
                continue
            name, stype, addr, off, size, flags = m.groups()
            if "W" not in flags:
                continue
            if stype not in ("PROGBITS", "NOBITS"):
                continue
            n = int(size, 16)
            if n == 0:
                continue
            out[name] = (int(addr, 16), n, stype)
    if not out:
        die("no writable sections parsed from readelf output")
    return out


def parse_nm(path):
    """`nm -n -S` -> list of (addr, size, type, name); size 0 when absent."""
    syms = []
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) == 4:
                addr, size, typ, name = parts
                try:
                    syms.append((int(addr, 16), int(size, 16), typ, name))
                except ValueError:
                    continue
            elif len(parts) == 3:
                addr, typ, name = parts
                try:
                    syms.append((int(addr, 16), 0, typ, name))
                except ValueError:
                    continue
    if not syms:
        die("no symbols parsed from nm output")
    return syms


def linker_ram(path):
    """The linker's own RAM used/total, from the build log's region report."""
    used = total = None
    pat = re.compile(r"^\s*RAM:\s+(\d+)\s*B\s+(\d+)\s*KB")
    with open(path) as f:
        for line in f:
            m = pat.match(line)
            if m:
                used = int(m.group(1))
                total = int(m.group(2)) * 1024
    if used is None:
        die("no 'RAM:' memory-region line in the build log")
    return used, total


def section_of(addr, size, sections):
    for name, (base, n, _t) in sections.items():
        if base <= addr < base + n:
            return name
    return None


def classify(name, ownership):
    """Exact match first, then longest prefix, then the catch-all."""
    if name in ownership["symbols"]:
        return ownership["symbols"][name]
    best = None
    for pref, meta in ownership["prefixes"].items():
        if name.startswith(pref):
            if best is None or len(pref) > len(best[0]):
                best = (pref, meta)
    if best:
        return best[1]
    return ownership["default"]


def main():
    if len(sys.argv) != 8:
        print(__doc__)
        sys.exit(2)
    nm_path, readelf_path, log_path, own_path, budget_path, out_md, out_json = \
        sys.argv[1:]

    sections = parse_sections(readelf_path)
    syms = parse_nm(nm_path)
    used, total = linker_ram(log_path)
    with open(own_path) as f:
        ownership = json.load(f)
    with open(budget_path) as f:
        budgets = json.load(f)

    section_total = sum(n for (_a, n, _t) in sections.values())

    entries = []
    seen = set()
    for addr, size, typ, name in syms:
        if size == 0:
            continue
        sec = section_of(addr, size, sections)
        if sec is None:
            continue                      # not in a writable RAM section
        key = (addr, name)
        if key in seen:
            continue
        seen.add(key)
        meta = classify(name, ownership)
        entries.append({
            "symbol": name,
            "size": size,
            "addr": "0x%08x" % addr,
            "section": sec,
            "category": meta["category"],
            "owner": meta["owner"],
            "declared_in": meta.get("declared_in", "?"),
            "lifetime": meta.get("lifetime", "?"),
            "writers": meta.get("writers", "?"),
            "readers": meta.get("readers", "?"),
            "phases": meta.get("phases", "?"),
            "overlaps": meta.get("overlaps", "none identified"),
            "evidence": meta.get("evidence", "NOT MEASURED"),
        })

    entries.sort(key=lambda e: -e["size"])
    listed = [e for e in entries if e["size"] >= MIN_LISTED]
    small = [e for e in entries if e["size"] < MIN_LISTED]
    listed_bytes = sum(e["size"] for e in listed)
    small_bytes = sum(e["size"] for e in small)
    sym_bytes = listed_bytes + small_bytes

    by_cat = {}
    for e in entries:
        by_cat.setdefault(e["category"], {"bytes": 0, "symbols": 0})
        by_cat[e["category"]]["bytes"] += e["size"]
        by_cat[e["category"]]["symbols"] += 1

    unattributed = section_total - sym_bytes
    free = total - used

    # ---- verdicts ------------------------------------------------------
    problems = []
    if abs(section_total - used) > RECONCILE_TOLERANCE:
        problems.append(
            "sections total %d B but the linker reports %d B used (gap %d B)"
            % (section_total, used, section_total - used))
    if unattributed > budgets["max_unattributed_bytes"]:
        problems.append(
            "%d B of writable RAM is not covered by any sized symbol, above the "
            "%d B allowance -- padding and unsized symbols should not grow"
            % (unattributed, budgets["max_unattributed_bytes"]))
    if free < budgets["min_free_bytes"]:
        problems.append("only %d B free, below the %d B floor"
                        % (free, budgets["min_free_bytes"]))
    for cat, cap in budgets["category_caps"].items():
        got = by_cat.get(cat, {"bytes": 0})["bytes"]
        if got > cap:
            problems.append("category '%s' uses %d B, above its %d B cap"
                            % (cat, got, cap))
    unmeasured = [e for e in listed
                  if e["evidence"] == "NOT MEASURED"
                  and e["size"] >= budgets["measure_required_at_bytes"]]

    # ---- output --------------------------------------------------------
    doc = {
        "linker": {"ram_used": used, "ram_total": total, "ram_free": free},
        "sections": {k: {"addr": "0x%08x" % v[0], "size": v[1], "type": v[2]}
                     for k, v in sorted(sections.items())},
        "section_total": section_total,
        "symbol_total": sym_bytes,
        "unattributed": unattributed,
        "by_category": by_cat,
        "symbols": listed,
        "small_symbol_bytes": small_bytes,
        "small_symbol_count": len(small),
        "unmeasured_large": [e["symbol"] for e in unmeasured],
        "problems": problems,
    }
    with open(out_json, "w") as f:
        json.dump(doc, f, indent=2, sort_keys=True)

    L = []
    L.append("# Stem Tape Player -- RAM inventory (from the linked ELF)\n")
    L.append("| | bytes |")
    L.append("|---|---:|")
    L.append("| RAM used (linker) | %d |" % used)
    L.append("| RAM total | %d |" % total)
    L.append("| **RAM free** | **%d** |" % free)
    L.append("| writable sections, summed | %d |" % section_total)
    L.append("| covered by sized symbols | %d |" % sym_bytes)
    L.append("| padding / unsized remainder | %d |" % unattributed)
    L.append("")
    L.append("## Sections\n")
    L.append("| section | type | addr | bytes |")
    L.append("|---|---|---|---:|")
    for k, (a, n, t) in sorted(sections.items(), key=lambda kv: -kv[1][1]):
        L.append("| `%s` | %s | 0x%08x | %d |" % (k, t, a, n))
    L.append("")
    L.append("## By category\n")
    L.append("| category | bytes | %% of used | symbols | cap |")
    L.append("|---|---:|---:|---:|---:|")
    for cat, v in sorted(by_cat.items(), key=lambda kv: -kv[1]["bytes"]):
        cap = budgets["category_caps"].get(cat)
        L.append("| %s | %d | %.1f%% | %d | %s |"
                 % (cat, v["bytes"], 100.0 * v["bytes"] / used, v["symbols"],
                    str(cap) if cap is not None else "-"))
    L.append("")
    L.append("## Every writable symbol of %d bytes or more\n" % MIN_LISTED)
    L.append("| bytes | symbol | category | owner | lifetime | live during | "
             "evidence for this size |")
    L.append("|---:|---|---|---|---|---|---|")
    for e in listed:
        L.append("| %d | `%s` | %s | %s | %s | %s | %s |"
                 % (e["size"], e["symbol"], e["category"], e["owner"],
                    e["lifetime"], e["phases"], e["evidence"]))
    L.append("")
    L.append("Below %d bytes: %d symbols, %d bytes total.\n"
             % (MIN_LISTED, len(small), small_bytes))
    if unmeasured:
        L.append("## Allocations of %d bytes or more with NO measured basis\n"
                 % budgets["measure_required_at_bytes"])
        for e in unmeasured:
            L.append("- `%s` (%d B, %s)" % (e["symbol"], e["size"], e["owner"]))
        L.append("")
    if problems:
        L.append("## Budget verdict: FAIL\n")
        for p in problems:
            L.append("- %s" % p)
    else:
        L.append("## Budget verdict: PASS\n")
        L.append("- %d B free, floor is %d B" % (free, budgets["min_free_bytes"]))
    L.append("")
    with open(out_md, "w") as f:
        f.write("\n".join(L))

    print("\n".join(L))
    if problems:
        sys.exit(1)
    print("RAM INVENTORY: PASS")


if __name__ == "__main__":
    main()
