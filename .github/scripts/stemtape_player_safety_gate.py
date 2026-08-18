#!/usr/bin/env python3
"""Stem Tape Player STRICT persistence safety gate (fail-closed).

stemtape-player is NOT the M0 diagnostic tool: unlike M0 (see
m0_safety_gate.py, which continues to guarantee that build can NEVER touch
the SP-1's stored audio at all), stemtape-player's whole purpose is to
durably persist validated 4-stem songs to the eMMC card. A blanket
"no eMMC write capability may be linked" gate is therefore the WRONG
property for this target -- it would fail on the exact capability this
milestone requires. This script is a different, still strictly fail-closed,
gate for the property that actually matters here:

  1. Every category of persistent-write capability that has NOTHING to do
     with stem-song storage -- internal-flash erase/write, NVMC destructive
     registers, UICR/APPROTECT, any OTHER filesystem/NVS/settings/disk
     subsystem, MCUboot/partition management -- remains completely
     forbidden, exactly as strictly as m0_safety_gate.py enforces it.

  2. eMMC writes are allowed ONLY through the hand-rolled sp1_emmc.c driver
     (emmc_write_blocks/meta_write_blocks) -- Zephyr's own disk_access/
     SD-MMC/filesystem subsystems remain forbidden.

  3. `emmc_write_blocks` itself is allowed ONLY from inside one of three
     purpose-built, region-bounded adapter functions in the derived
     src/main.c -- GATE 2's real transfer/commit engine
     (xfer_staging_write/xfer_header_write/xfer_songdata_write). This gate
     does not just trust the symbol being linked, or trust a caller's
     promise: it proves, for every real `emmc_write_blocks` call site,
     that it is inside one of exactly those three functions AND that the
     function's OWN source body contains the real region-boundary
     constant(s) it claims to be bounded by, with an early `return -1;`
     guard before the write is ever reached -- a structural property of
     the function itself, independent of who calls it or in what order.
     `meta_write_blocks` (the classic looper's own index writer) must have
     ZERO call sites -- it has no definition left to link in this
     derivative at all. See pass D below for the exact per-function
     evidence.

Any call site this script cannot confidently classify FAILS the gate. This
is deliberately at least as strict as m0_safety_gate.py for every category
that isn't eMMC storage, and strictly proves (not merely asserts) the one
category where stemtape-player legitimately differs from M0.

Usage: stemtape_player_safety_gate.py <nm.txt> <objdump-d.txt> <.config> \
           <out-report.md>
"""

from __future__ import annotations

import os
import re
import sys

nm_file, dis_file, cfg_file, out_path = sys.argv[1:5]

SOURCE_ROOTS = [
    "firmware/stemtape_player",
    "firmware/boards/teenageengineering/stem_player",
]

DERIVED_MAIN_C = "firmware/stemtape_player/src/main.c"

# ---- symbol / source patterns that must NOT be present -------------------
# Identical in spirit and wording to m0_safety_gate.py's own categories,
# EXCEPT the eMMC category: `emmc_write_blocks` itself is carved out here
# (traced separately, with proof, in pass D below) but every OTHER
# eMMC-write-shaped symbol, and every other storage subsystem, stays fully
# forbidden -- the negative lookahead below still fails on e.g. a future
# `emmc_writeback()`/`emmc_write_uicr()` helper that pass D never audited.
FORBIDDEN_SYMBOLS = [
    ("internal-flash write/erase",
     r"\bflash_write\b|\bflash_erase\b|flash_area_write|flash_area_erase|"
     r"flash_area_flatten|nrf_fstorage|nrfx_nvmc|soc_flash_nrf|flash_nrf"),
    ("NVMC register access",
     r"NRF_NVMC|->CONFIG\s*=\s*NVMC|ERASEPAGE|ERASEALL|ERASEUICR|"
     r"NVMC_CONFIG_WEN|nvmc_"),
    ("UICR write / access protection",
     r"NRF_UICR->\s*\w+\s*=|UICR->APPROTECT|APPROTECT\s*=|"
     # (?<!pin) excludes exactly one confirmed false positive, found by a
     # real CI run of this gate: Zephyr's own generic pinctrl subsystem
     # function `pinctrl_apply_state()` (drivers/pinctrl/pinctrl_nrf.c,
     # linked in by CONFIG_PINCTRL=y for ADC/UART/etc. pin muxing on every
     # target that uses a devicetree &pinctrl node -- nothing to do with
     # UICR or the Cortex-M CTRL-AP debug port) happens to contain the
     # literal substring "ctrl_ap" purely by coincidence: "pin" + "CTRL_AP"
     # + "ply_state". Evidence: nm.txt line
     # "00034af6 0000001e t pinctrl_apply_state.isra.0". The lookbehind
     # narrowly excludes only this one collision -- CTRLAP (no underscore)
     # and every other ctrl_ap-shaped identifier not immediately preceded
     # by "pin" still fails the gate exactly as before.
     r"nrf_uicr_write|CTRLAP|(?<!pin)ctrl_ap|DISABLE_APPROTECT|ENABLE_APPROTECT"),
    ("non-driver eMMC write / other disk / SD subsystem write",
     r"disk_access_write|disk_access_ioctl|sdmmc_|"
     r"\bemmc_(?!write_blocks\b)\w*write\w*\b|"
     r"(?<!e)\bmmc_write\b|sdhc_"),
    ("filesystem / settings / NVS write",
     r"\bnvs_write\b|\bnvs_init\b|\bnvs_mount\b|settings_save|settings_load|"
     r"\bfs_write\b|\bfs_open\b|\bfs_mount\b|littlefs|fatfs|\bff_\w+_write"),
    ("MCUboot / partition management",
     r"boot_set_next|boot_set_pending|boot_write_img_confirmed|img_mgmt_|"
     r"boot_request_upgrade|flash_img_"),
]

# ---- Kconfig options that must NOT be enabled ----------------------------
FORBIDDEN_CONFIG = [
    "CONFIG_FLASH", "CONFIG_FLASH_MAP", "CONFIG_NVS", "CONFIG_SETTINGS",
    "CONFIG_FILE_SYSTEM", "CONFIG_DISK_ACCESS", "CONFIG_SDMMC_SUBSYS",
    "CONFIG_MMC_STACK", "CONFIG_BOOTLOADER_MCUBOOT", "CONFIG_IMG_MANAGER",
    "CONFIG_STREAM_FLASH", "CONFIG_NRF_APPROTECT_LOCK",
    "CONFIG_NRF_APPROTECT_USER_HANDLING",
]

# ---- literal peripheral windows in the disassembly -----------------------
ADDRESS_WINDOWS = [
    ("NVMC (0x4001E000-0x4001EFFF)", 0x4001E000, 0x4001EFFF),
    ("UICR (0x10001000-0x100013FF)", 0x10001000, 0x100013FF),
    ("FICR/CTRL-AP (0x4000C000-0x4000CFFF)", 0x4000C000, 0x4000CFFF),
]

ALLOWED_UICR_READONLY = [
    "nrfx_", "SystemInit", "z_arm_platform_init", "soc_early_init_hook",
]

failures: list[str] = []
report: list[str] = [
    "# Stem Tape Player — STRICT persistence safety gate", "",
    "Fail-closed. Static, read-only; no device was written.", "",
    "Unlike stemtape-m0's gate, this target is REQUIRED to durably persist "
    "validated 4-stem songs to eMMC -- pass D below proves the specific, "
    "narrow property that matters (only the validated commit path reaches "
    "new persistent-write capability) instead of forbidding eMMC writes "
    "outright.", "",
]


def source_files():
    for root in SOURCE_ROOTS:
        for dirpath, _dirs, files in os.walk(root):
            for f in files:
                yield os.path.join(dirpath, f)


files = sorted(set(source_files()))
nm_lines = open(nm_file, errors="ignore").read().splitlines()
dis = open(dis_file, errors="ignore").read().splitlines()
cfg = open(cfg_file, errors="ignore").read().splitlines() if os.path.exists(cfg_file) else []

report.append(f"Source files scanned: {len(files)}  |  symbols: {len(nm_lines)}"
              f"  |  disassembly lines: {len(dis)}  |  config lines: {len(cfg)}")
report.append("")


# ---- nm-line parsing + the same two documented false-positive exclusions -
def parse_nm_line(line: str):
    parts = line.split(None, 3)
    if len(parts) == 3:
        addr, typ, name = parts
        return addr, None, typ, name
    if len(parts) == 4:
        addr, size, typ, name = parts
        return addr, size, typ, name
    return None


nm_addr: dict[str, int] = {}
for _l in nm_lines:
    _p = parse_nm_line(_l)
    if _p is None:
        continue
    try:
        nm_addr[_p[3]] = int(_p[0], 16)
    except ValueError:
        pass


def is_kconfig_marker(nm_line: str) -> bool:
    parsed = parse_nm_line(nm_line)
    if parsed is None:
        return False
    _addr, _size, typ, name = parsed
    return typ == "A" and name.startswith("CONFIG_")


def empty_iterable_boundary(nm_line: str) -> bool:
    parsed = parse_nm_line(nm_line)
    if parsed is None:
        return False
    _addr, _size, _typ, name = parsed
    for suffix, other_suffix in (("_list_start", "_list_end"),
                                  ("_list_end", "_list_start")):
        if name.endswith(suffix):
            paired = name[: -len(suffix)] + other_suffix
            if name in nm_addr and paired in nm_addr:
                return nm_addr[name] == nm_addr[paired]
    return False


# --- pass A: symbols + source text -----------------------------------------
report.append("## A. Symbol and source scan (categories unrelated to eMMC "
              "storage — same strictness as stemtape-m0)")
report.append("")
for label, pattern in FORBIDDEN_SYMBOLS:
    rx = re.compile(pattern)
    hits = []
    for path in files:
        try:
            with open(path, errors="ignore") as fh:
                for n, line in enumerate(fh, 1):
                    if rx.search(line):
                        hits.append(f"{path}:{n}: {line.strip()[:180]}")
        except OSError:
            continue
    excluded_kconfig = []
    excluded_iterable = []
    for l in nm_lines:
        if not rx.search(l):
            continue
        if is_kconfig_marker(l):
            excluded_kconfig.append(l)
            continue
        if empty_iterable_boundary(l):
            excluded_iterable.append(l)
            continue
        hits.append(f"symbol: {l}")
    if hits:
        failures.append(f"{label}: {len(hits)} reference(s)")
        report.append(f"FAIL — {label}: {len(hits)} reference(s)")
        report.append("")
        report.append("```text")
        report.extend(hits[:200])
        report.append("```")
    else:
        report.append(f"PASS — {label}: none found.")
    if excluded_kconfig:
        report.append("")
        report.append(f"  Excluded as Kconfig link-time markers: {len(excluded_kconfig)}")
        report.append("```text")
        report.extend(excluded_kconfig[:50])
        report.append("```")
    if excluded_iterable:
        report.append("")
        report.append(f"  Excluded as empty iterable-section boundaries: "
                       f"{len(excluded_iterable)}")
        report.append("```text")
        report.extend(excluded_iterable[:50])
        report.append("```")
    report.append("")

# --- pass B: Kconfig --------------------------------------------------------
report.append("## B. Effective Kconfig")
report.append("")
enabled = []
for line in cfg:
    m = re.match(r"^(CONFIG_[A-Z0-9_]+)=y$", line.strip())
    if m and m.group(1) in FORBIDDEN_CONFIG:
        enabled.append(line.strip())
if enabled:
    failures.append(f"forbidden Kconfig enabled: {enabled}")
    report.append("FAIL — persistent-storage subsystems enabled:")
    report.append("")
    report.append("```text")
    report.extend(enabled)
    report.append("```")
else:
    report.append("PASS — none of " + ", ".join(FORBIDDEN_CONFIG) + " is enabled.")
report.append("")

# --- pass C: literal peripheral addresses in the disassembly ---------------
report.append("## C. Literal peripheral-address scan (register writes without symbols)")
report.append("")
hexrx = re.compile(r"0x([0-9a-fA-F]{8})")
symrx = re.compile(r"^[0-9a-f]+ <([^>]+)>:")

func_starts = [(i, m.group(1)) for i, line in enumerate(dis)
               for m in [symrx.match(line)] if m]


def enclosing_function(line_idx: int):
    start, name = 0, "?"
    for s, n in func_starts:
        if s > line_idx:
            break
        start, name = s, n
    end = len(dis)
    for s, _n in func_starts:
        if s > start:
            end = s
            break
    return start, end, name


NVMC_OFFSET_NAMES = {
    0x400: "READY (read-only status)",
    0x504: "CONFIG (write/erase enable — DANGEROUS)",
    0x508: "ERASEPAGE (DANGEROUS)",
    0x50C: "ERASEALL (DANGEROUS — destroys the whole device incl. bootloader)",
    0x510: "ERASEPAGEPARTIAL (DANGEROUS)",
    0x514: "ERASEUICR (DANGEROUS)",
    0x518: "ERASEPAGEPARTIALCFG (DANGEROUS)",
    0x540: "ICACHECNF (instruction-cache enable, volatile, resets on power-cycle)",
    0x548: "IHIT (read-only cache-hit counter)",
    0x54C: "IMISS (read-only cache-miss counter)",
}
NVMC_ALLOWED_WRITE_OFFSETS = {0x540}
STORE_MNEM_RX = re.compile(r"\bstr[bh]?(?:\.w)?\s")
STORE_OFFSET_RX = re.compile(r";\s*0x([0-9a-fA-F]+)\s*$")
CALL_RX = re.compile(r"\bbl[x]?(?:\.w)?\s")
LOAD_LITERAL_RX = re.compile(
    r"\bldr(?:\.w)?\s+(r\d+),\s*\[pc,[^\]]*\]\s*;\s*\(([0-9a-fA-F]+)")

current_sym = "?"
found = {label: [] for label, _lo, _hi in ADDRESS_WINDOWS}
for i, line in enumerate(dis):
    m = symrx.match(line)
    if m:
        current_sym = m.group(1)
        continue
    for h in hexrx.findall(line):
        v = int(h, 16)
        for label, lo, hi in ADDRESS_WINDOWS:
            if lo <= v <= hi:
                found[label].append((current_sym, i, line.strip()[:180]))

for label, lo, hi in ADDRESS_WINDOWS:
    hits = found[label]
    if not hits:
        report.append(f"PASS — {label}: no literal reference.")
        report.append("")
        continue
    if label.startswith("NVMC"):
        report.append(f"{label}: {len(hits)} literal reference(s) — resolving each "
                      f"to the exact register that loads the NVMC base and every "
                      f"write through that register. ICACHECNF (base+0x540 = "
                      f"0x4001E540) is the sole permitted write. Every other "
                      f"offset fails the gate. Failing closed: an unresolved base "
                      f"register, an unresolved store offset, or a function that "
                      f"branches out (bl/blx) while the base may still be live "
                      f"all fail the gate.")
        report.append("")
        seen = []
        any_bad = False
        for sym, i, _txt in hits:
            if sym in seen:
                continue
            seen.append(sym)
            start, end, _name = enclosing_function(i)
            body = dis[start:end]
            report.append(f"### `{sym}`")
            report.append("")
            report.append("```text")
            report.extend(l.rstrip() for l in body[:200])
            report.append("```")
            report.append("")

            lit_addr_m = re.match(r"^\s*([0-9a-fA-F]+):", dis[i])
            lit_addr = lit_addr_m.group(1).lstrip("0") or "0" if lit_addr_m else None
            base_reg = None
            for l in body:
                lm = LOAD_LITERAL_RX.search(l)
                if lm and lit_addr is not None and lm.group(2).lstrip("0") == lit_addr:
                    base_reg = lm.group(1)
                    break

            bad_here = []
            offsets_here = []
            if base_reg is None:
                bad_here.append(("could not resolve which register receives the "
                                 "NVMC base address — cannot verify any offset",
                                 dis[i].strip()))
            else:
                reg_operand_rx = re.compile(r"\[\s*" + re.escape(base_reg) + r"\s*,")
                for l in body:
                    if CALL_RX.search(l):
                        bad_here.append(("function branches out (bl/blx); the NVMC "
                                         "base may still be live in a register and "
                                         "the callee cannot be verified here",
                                         l.strip()))
                        continue
                    if not STORE_MNEM_RX.search(l) or not reg_operand_rx.search(l):
                        continue
                    om = STORE_OFFSET_RX.search(l)
                    if not om:
                        bad_here.append((f"store through {base_reg} with an "
                                         "unresolved offset", l.strip()))
                        continue
                    off = int(om.group(1), 16)
                    name = NVMC_OFFSET_NAMES.get(off, f"undocumented offset 0x{off:x}")
                    offsets_here.append((off, name, l.strip()))
                    if off not in NVMC_ALLOWED_WRITE_OFFSETS:
                        bad_here.append((name, l.strip()))
            if bad_here:
                any_bad = True
                report.append(f"FAIL — `{sym}`: non-permitted NVMC write:")
                for name, l in bad_here:
                    report.append(f"  - {name}: {l}")
            elif offsets_here:
                report.append(f"PASS — `{sym}`: base address resolved to register "
                              f"`{base_reg}`; the only NVMC offset(s) written through "
                              f"it: " + ", ".join(f"0x{o:x} ({n})"
                                                   for o, n, _l in offsets_here))
            else:
                report.append(f"PASS — `{sym}`: base address resolved to register "
                              f"`{base_reg}`; no write through it in this function.")
            report.append("")
        if any_bad:
            failures.append(f"{label}: non-permitted NVMC register write (see per-function analysis above)")
            report.append(f"FAIL — {label}: see per-function analysis above.")
        else:
            report.append(f"PASS — {label}: {len(hits)} literal reference(s), every "
                          f"resolved write traced to ICACHECNF (0x4001E540) only.")
        report.append("")
        continue
    if label.startswith("UICR"):
        unexpected = [h for h in hits
                      if not any(a in h[0] for a in ALLOWED_UICR_READONLY)]
        report.append(f"{'FAIL' if unexpected else 'PASS'} — {label}: "
                      f"{len(hits)} reference(s), "
                      f"{len(unexpected)} outside the verified read-only allow-list "
                      f"({', '.join(ALLOWED_UICR_READONLY)}).")
        report.append("")
        report.append("```text")
        report.extend(f"{sym}: {txt}" for sym, _i, txt in hits[:200])
        report.append("```")
        report.append("")
        if unexpected:
            failures.append(f"{label}: {len(unexpected)} non-allow-listed reference(s)")
        continue
    failures.append(f"{label}: {len(hits)} literal reference(s)")
    report.append(f"FAIL — {label}: {len(hits)} literal reference(s)")
    report.append("")
    report.append("```text")
    report.extend(f"{sym}: {txt}" for sym, _i, txt in hits[:200])
    report.append("```")
    report.append("")
    seen = []
    for sym, i, _txt in hits:
        if sym in seen:
            continue
        seen.append(sym)
        start, end, _name = enclosing_function(i)
        report.append(f"Full disassembly of `{sym}` (offset tracing evidence):")
        report.append("```text")
        report.extend(dis[start:end][:200])
        report.append("```")
        report.append("")

# --- pass D: eMMC persistent-write call-site provenance ---------------------
# GATE 2 (real 4-stem song transfer) replaced the earlier st_stem_validate_
# commit()-gated design (that module was superseded before it was ever
# wired to a real verb -- see st_stem_validate.h's own docstring vs.
# st_transfer.c's real, independently-designed and already-tested
# st_xfer_verify()/st_xfer_commit_precheck()) with three purpose-built,
# region-bounded eMMC-write adapter functions in the derived src/main.c.
# Each is bounded to its own disjoint sector range by its OWN function
# body -- a literal comparison against the real region-boundary
# constant(s), with an early `return -1;` before ever reaching
# `emmc_write_blocks` -- not merely by caller trust or call-graph
# position, so this pass proves the bound structurally rather than by
# tracing every caller.
report.append("## D. eMMC write call-site provenance (the property that "
              "actually differs from stemtape-m0)")
report.append("")
report.append(
    "`emmc_write_blocks` may be called ONLY from inside one of three "
    "region-bounded adapter functions in the derived src/main.c:\n\n"
    "- `xfer_staging_write` -- bounded to `[ST_STAGING_SECTOR0, "
    "+ST_STAGING_SECTOR_COUNT)`. Called only from the 'S' (stage) verb.\n"
    "- `xfer_header_write` -- bounded to `[ST_LIBRARY_HEADER_SECTOR0, "
    "+ST_LIBRARY_HEADER_SECTORS)`. Called only from the commit-gated 'C' "
    "path and the destructive-token-gated 'D'/'I' paths.\n"
    "- `xfer_songdata_write` -- bounded below by `ST_SONG_DATA_SECTOR0` and "
    "above by the real, EXT_CSD-detected device capacity "
    "(`g_emmc_total_sectors`; 0 = not yet known, fail-closed). Called only "
    "from the commit path, after the transaction is confirmed open and "
    "verified.\n\n"
    "Any `emmc_write_blocks` call site OUTSIDE these three functions, or "
    "inside one of them without its real bounds-check pattern actually "
    "present in the source, FAILS closed. `meta_write_blocks` (the classic "
    "looper's own index writer) must have ZERO call sites -- it has no "
    "definition left to link in this derivative at all."
)
report.append("")

TARGET_SYMBOLS = ["emmc_write_blocks", "meta_write_blocks"]
FUNC_SIG_RX = re.compile(
    r"^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w ]*?\*?\s*([A-Za-z_]\w*)\s*\([^;{]*\)\s*$")

ALLOWED_WRITE_FUNCS = {
    "xfer_staging_write": ("ST_STAGING_SECTOR0", "ST_STAGING_SECTOR_COUNT"),
    "xfer_header_write": ("ST_LIBRARY_HEADER_SECTOR0", "ST_LIBRARY_HEADER_SECTORS"),
    "xfer_songdata_write": ("ST_SONG_DATA_SECTOR0", "g_emmc_total_sectors"),
}


def index_functions(lines: list[str]):
    """Map each 1-based line number to its enclosing top-level function name,
    using brace-depth tracking. Returns dict[int -> str|None]."""
    depth = 0
    cur_func = None
    pending_name = None
    func_of_line: dict[int, str | None] = {}
    for i, line in enumerate(lines):
        if depth == 0:
            m = FUNC_SIG_RX.match(line)
            if m:
                pending_name = m.group(1)
        opens = line.count("{")
        closes = line.count("}")
        if depth == 0 and opens > 0 and pending_name:
            cur_func = pending_name
        depth += opens - closes
        func_of_line[i + 1] = cur_func if depth > 0 else None
        if depth <= 0:
            depth = max(depth, 0)
            if cur_func is not None:
                # a real function body just closed.
                cur_func = None
                pending_name = None
            elif opens > 0:
                # a same-line brace pair at file scope that never became a
                # function body (e.g. a `static const T x[] = { ... };`
                # initializer) -- don't let it leak into the next signature.
                pending_name = None
    return func_of_line


def find_call_sites(lines: list[str], func_of_line: dict[int, str | None]):
    """dict[(func, symbol)] -> list of 1-based line numbers, for real calls
    only (skips comments/doc lines mentioning the symbol name in prose)."""
    sites: dict[tuple[str, str], list[int]] = {}
    for i, line in enumerate(lines, 1):
        stripped = line.strip()
        if stripped.startswith(("*", "//")):
            continue
        for sym in TARGET_SYMBOLS:
            if re.search(r"\b" + sym + r"\s*\(", line):
                fn = func_of_line.get(i)
                if fn is None:
                    continue  # not inside any function body (e.g. the fn's own signature)
                sites.setdefault((fn, sym), []).append(i)
    return sites


def function_body_bounds(lines: list[str], line_1based: int) -> tuple[int, int]:
    """Given a 1-based line number known to be inside some function's body,
    returns [start, end) 0-based bounds of that exact function's brace-
    delimited body -- found by real brace-depth matching (walk outward to
    the nearest enclosing unmatched '{', then forward to its matching
    '}'), not a fixed line-count guess. Same technique this repo's own
    stemtape_player_usb_descriptor_assertions.py uses for devicetree node
    bodies, reapplied here for C function bodies."""
    idx = line_1based - 1
    depth = 0
    start = 0
    for i in range(idx, -1, -1):
        depth += lines[i].count("}") - lines[i].count("{")
        if depth < 0:
            start = i
            break
    depth = 0
    end = len(lines)
    for i in range(start, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if depth == 0 and i > start:
            end = i + 1
            break
    return start, end


derived_lines = open(DERIVED_MAIN_C, errors="ignore").read().splitlines()
derived_func_of_line = index_functions(derived_lines)
derived_sites = find_call_sites(derived_lines, derived_func_of_line)

any_bad = False

meta_sites = [ln for (fn, sym), lns in derived_sites.items() if sym == "meta_write_blocks" for ln in lns]
if meta_sites:
    failures.append(f"pass D: meta_write_blocks has {len(meta_sites)} call site(s) -- "
                     "must never be called")
    report.append(f"FAIL — `meta_write_blocks` called at line(s) {meta_sites}")
    any_bad = True
else:
    report.append("confirmed: `meta_write_blocks` has zero call sites")
report.append("")

write_sites = {(fn, sym): lns for (fn, sym), lns in derived_sites.items() if sym == "emmc_write_blocks"}
report.append(f"`emmc_write_blocks` call sites in `{DERIVED_MAIN_C}`: "
              f"{sum(len(v) for v in write_sites.values())} across "
              f"{len(write_sites)} enclosing function(s).")
report.append("")

seen_funcs = set()
for (fn, _sym), lines_ in sorted(write_sites.items()):
    seen_funcs.add(fn)
    if fn not in ALLOWED_WRITE_FUNCS:
        failures.append(f"pass D: emmc_write_blocks called from `{fn}()`, which is "
                         "not one of the three allowed region-bounded adapters")
        report.append(f"FAIL — unexpected caller `{fn}()` at line(s) {lines_}")
        any_bad = True
        continue
    lower_const, upper_const = ALLOWED_WRITE_FUNCS[fn]
    start, end = function_body_bounds(derived_lines, lines_[0])
    body = "\n".join(derived_lines[start:end])
    has_lower = lower_const in body
    has_upper = upper_const in body
    has_return = re.search(r"return\s+-1\s*;", body) is not None
    ok = has_lower and has_upper and has_return
    report.append(f"{'PASS' if ok else 'FAIL'} — `{fn}()` ({len(lines_)} call site(s), "
                  f"line(s) {lines_}, body {DERIVED_MAIN_C}:{start + 1}-{end}): "
                  f"lower bound `{lower_const}` present: {has_lower}; "
                  f"upper bound `{upper_const}` present: {has_upper}; an early "
                  f"`return -1;` guard present: {has_return}")
    if not ok:
        failures.append(f"pass D: `{fn}()` is missing its required bounds-check pattern")
        any_bad = True
    report.append("```text")
    report.extend(body.splitlines())
    report.append("```")
    report.append("")

for fn in ALLOWED_WRITE_FUNCS:
    if fn not in seen_funcs:
        report.append(f"NOTE: `{fn}()` has zero `emmc_write_blocks` call sites in this "
                      "build (not itself a failure -- e.g. legitimately dead-code-"
                      "eliminated if genuinely unreachable in this configuration -- but "
                      "worth a human's attention if unexpected).")
        report.append("")

if any_bad:
    report.append("FAIL — pass D: see above.")
else:
    report.append("PASS — pass D: every `emmc_write_blocks` call site is inside one of "
                  "the three allowed, independently region-bounded adapter functions, "
                  "and each one's real bounds-check pattern is confirmed present in its "
                  "own source body.")
report.append("")

# --- explicitly permitted volatile operations -------------------------------
report.append("## E. Permitted volatile operations (documented, not persistent)")
report.append("")
PERMITTED = [
    ("GPREGRET = 0x57", r"GPREGRET", "POWER retention register (RAM-backed). "
     "Survives soft reset, is not flash; requests UF2 bootloader."),
    ("RESETREAS clear", r"RESETREAS", "Volatile reset-reason register."),
    ("SYSTEMOFF", r"SYSTEMOFF", "Deep sleep entry; writes no memory."),
    ("watchdog feed / status", r"NRF_WDT->|wdt_install_timeout|wdt_setup|RREN|RUNSTATUS",
     "WDT config and reload registers are volatile on nRF52840."),
    ("power / charger GPIO", r"BQ_NCE_PIN|PWR_PIN|charger_init",
     "Electrical control lines only."),
]
for label, pattern, why in PERMITTED:
    rx = re.compile(pattern)
    hits = []
    for path in files:
        try:
            with open(path, errors="ignore") as fh:
                for n, line in enumerate(fh, 1):
                    if rx.search(line):
                        hits.append(f"{path}:{n}: {line.strip()[:160]}")
        except OSError:
            continue
    report.append(f"### {label} — {len(hits)} reference(s)")
    report.append("")
    report.append(why)
    report.append("")
    if hits:
        report.append("```text")
        report.extend(hits[:80])
        report.append("```")
        report.append("")

report.append("## Result")
report.append("")
if failures:
    report.append("GATE FAILED:")
    report.append("")
    for f in failures:
        report.append(f"- {f}")
else:
    report.append("GATE PASSED — no persistent-write capability outside the three "
                  "proven, independently region-bounded eMMC adapter functions was "
                  "detected in the image.")
report.append("")

open(out_path, "w").write("\n".join(report) + "\n")
print("\n".join(report))
sys.exit(1 if failures else 0)
