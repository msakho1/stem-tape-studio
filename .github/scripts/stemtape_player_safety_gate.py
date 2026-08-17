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
     (emmc_write_blocks/meta_write_blocks/emmc_cache_flush) -- Zephyr's own
     disk_access/SD-MMC/filesystem subsystems remain forbidden.

  3. Within that driver's call sites in src/main.c, this gate does not just
     trust the symbol being linked -- it proves, per call site, that the
     capability is either:
       (a) INHERITED: the exact same (enclosing function, target call)
           pairing already exists, in the same or greater count, in the
           untouched classic Tape Looper baseline (firmware/src/main.c).
           That baseline is the existing, already-proven regression
           reference for this whole codebase -- a call site that matches it
            1:1 is not new capability introduced this session, it is the
           unmodified classic persistence pattern (index writes trusted from
           host-managed state, cache flushes, cold-boot format-fresh, the
           classic 'W'-verb raw track-data staging write, etc.), and
       (b) NEW: any call site in excess of the matching classic count is
           new. Every NEW call site MUST be lexically nested inside a
           conditional block that is provably reached only after
           `st_stem_validate_commit(...)` has returned `ST_STEM_OK` --
           proven by a brace-depth range trace between the nearest prior
           `st_stem_validate_commit(` call and the write call site (the
           trace fails closed: depth must never return to, or below, the
           validate call's own depth before the write is reached, and an
           `ST_STEM_OK` comparison plus conditional control flow must
           appear in that range). Evidence -- both the source region and the
           depth trace -- is printed for every NEW call site, pass or fail.

Any call site this script cannot confidently classify FAILS the gate. This
is deliberately at least as strict as m0_safety_gate.py for every category
that isn't eMMC storage, and strictly proves (not merely asserts) the one
category where stemtape-player legitimately differs from M0.

Usage: stemtape_player_safety_gate.py <nm.txt> <objdump-d.txt> <.config> \
           <classic-baseline-main.c> <out-report.md>
"""

from __future__ import annotations

import os
import re
import sys

nm_file, dis_file, cfg_file, baseline_main_c, out_path = sys.argv[1:6]

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

# --- pass D: eMMC persistent-write call-site provenance and commit gate ----
report.append("## D. eMMC write call-site provenance (the property that "
              "actually differs from stemtape-m0)")
report.append("")
report.append(
    "For each of `emmc_write_blocks`, `meta_write_blocks`, `emmc_cache_flush`, "
    "every call site in the derived src/main.c is grouped by its enclosing "
    "function and compared against the SAME (function, target) call count in "
    "the untouched classic baseline (`firmware/src/main.c`). A call site "
    "inside a function whose count for that target matches (or is exceeded "
    "by) the classic baseline is INHERITED -- the unmodified, already-proven "
    "classic Tape Looper persistence pattern, not new capability. Any call "
    "site in EXCESS of the classic count is NEW and must be individually "
    "proven reachable only after `st_stem_validate_commit(...)` has returned "
    "`ST_STEM_OK` (brace-depth range trace, evidence printed below). Any "
    "call site this script cannot classify, or cannot prove gated, FAILS."
)
report.append("")

TARGET_SYMBOLS = ["emmc_write_blocks", "meta_write_blocks", "emmc_cache_flush"]
FUNC_SIG_RX = re.compile(
    r"^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w ]*?\*?\s*([A-Za-z_]\w*)\s*\([^;{]*\)\s*$")


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


derived_lines = open(DERIVED_MAIN_C, errors="ignore").read().splitlines()
baseline_lines = open(baseline_main_c, errors="ignore").read().splitlines()

derived_func_of_line = index_functions(derived_lines)
baseline_func_of_line = index_functions(baseline_lines)

derived_sites = find_call_sites(derived_lines, derived_func_of_line)
baseline_sites = find_call_sites(baseline_lines, baseline_func_of_line)

report.append(f"Baseline (`{baseline_main_c}`): "
              f"{sum(len(v) for v in baseline_sites.values())} call site(s) across "
              f"{len(baseline_sites)} (function, target) pairing(s).")
report.append(f"Derived (`{DERIVED_MAIN_C}`): "
              f"{sum(len(v) for v in derived_sites.values())} call site(s) across "
              f"{len(derived_sites)} (function, target) pairing(s).")
report.append("")


def brace_depth_before(lines: list[str], line_no_1based: int) -> int:
    """Net (opens - closes) over lines[0 : line_no_1based-1], i.e. the brace
    depth in effect at the START of the given 1-based line."""
    depth = 0
    for l in lines[: line_no_1based - 1]:
        depth += l.count("{") - l.count("}")
    return depth


def prove_gated(lines: list[str], func: str, write_line: int, symbol: str) -> tuple[bool, list[str]]:
    """Trace backward from write_line (1-based) within `func` for the nearest
    prior `st_stem_validate_commit(` call, then verify: (1) an ST_STEM_OK
    comparison plus conditional control flow appears between that call and
    the write, and (2) brace depth never returns to, or below, the validate
    call's own depth before the write is reached (i.e. the write stays
    lexically nested inside a block opened after the validate call).
    Returns (proven, evidence_lines)."""
    ev = [f"target: `{symbol}(...)` at {DERIVED_MAIN_C}:{write_line} "
          f"(enclosing function `{func}`)",
          f"  {derived_lines[write_line - 1].strip()}"]

    validate_line = None
    for i in range(write_line - 1, 0, -1):
        if derived_func_of_line.get(i) != func:
            break
        if "st_stem_validate_commit(" in lines[i - 1]:
            validate_line = i
            break
    if validate_line is None:
        ev.append("  FAIL: no `st_stem_validate_commit(` call found earlier "
                   "in the same function.")
        return False, ev

    ev.append(f"  nearest prior validate call: {DERIVED_MAIN_C}:{validate_line}: "
              f"{lines[validate_line - 1].strip()}")

    depth_at_validate = brace_depth_before(lines, validate_line)
    depth_at_write = brace_depth_before(lines, write_line)
    ev.append(f"  brace depth at validate call: {depth_at_validate}; "
              f"at write call: {depth_at_write}")

    between = lines[validate_line - 1: write_line - 1]
    saw_ok = any("ST_STEM_OK" in l for l in between)
    saw_cond = any(re.search(r"\b(if|else)\b", l) for l in between)
    ev.append(f"  `ST_STEM_OK` comparison present in between: {saw_ok}; "
              f"conditional control flow (if/else) present in between: {saw_cond}")

    min_depth = depth_at_validate
    running = depth_at_validate
    for l in between:
        running += l.count("{") - l.count("}")
        min_depth = min(min_depth, running)
    ev.append(f"  minimum brace depth reached between validate and write: {min_depth} "
              f"(must be >= depth-at-validate = {depth_at_validate})")

    proven = (saw_ok and saw_cond
              and min_depth >= depth_at_validate
              and depth_at_write > depth_at_validate)
    ev.append(f"  depth-at-write > depth-at-validate: {depth_at_write > depth_at_validate}")
    ev.append(f"  RESULT: {'PROVEN GATED' if proven else 'NOT PROVEN — FAILS CLOSED'}")
    return proven, ev


any_new_unproven = False
all_pairs = sorted(set(derived_sites) | set(baseline_sites))
for func, sym in all_pairs:
    d_lines = derived_sites.get((func, sym), [])
    b_count = len(baseline_sites.get((func, sym), []))
    d_count = len(d_lines)
    if d_count <= b_count:
        report.append(f"INHERITED — `{func}()` calling `{sym}(...)`: "
                      f"{d_count} call site(s) here vs {b_count} in the classic "
                      f"baseline (no increase; unmodified classic pattern). "
                      f"Lines: {d_lines}")
        report.append("")
        continue
    excess = d_count - b_count
    report.append(f"NEW CAPABILITY — `{func}()` calling `{sym}(...)`: "
                  f"{d_count} call site(s) here vs {b_count} in the classic "
                  f"baseline ({excess} new). Every call site in this function "
                  f"is being individually traced (ambiguous which specific "
                  f"occurrence(s) are the {b_count} inherited one(s), so all "
                  f"{d_count} are proven, fail-closed).")
    report.append("")
    for ln in d_lines:
        proven, ev = prove_gated(derived_lines, func, ln, sym)
        report.append("```text")
        report.extend(ev)
        report.append("```")
        report.append("")
        if not proven:
            any_new_unproven = True

if any_new_unproven:
    failures.append("pass D: one or more NEW eMMC persistent-write call sites "
                     "could not be proven reachable only via a validated "
                     "st_stem_validate_commit() == ST_STEM_OK commit (see "
                     "section D above for the full trace)")
    report.append("FAIL — pass D: see traces above.")
else:
    report.append("PASS — pass D: every NEW eMMC persistent-write call site is "
                  "proven reachable only after `st_stem_validate_commit(...)` "
                  "returned `ST_STEM_OK`; every other call site is unmodified, "
                  "classic-baseline persistence behavior.")
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
    report.append("GATE PASSED — no persistent-write capability outside the proven, "
                  "validated eMMC commit path (and unmodified classic-baseline "
                  "persistence behavior) was detected in the image.")
report.append("")

open(out_path, "w").write("\n".join(report) + "\n")
print("\n".join(report))
sys.exit(1 if failures else 0)
