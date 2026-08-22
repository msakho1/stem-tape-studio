#!/usr/bin/env python3
"""Stem Tape M0 STRICT persistence safety gate (fail-closed).

This is NOT the descriptive persistence report (m0_persistence_audit.py keeps
that role). This gate FAILS the job when the image contains any capability to
alter persistent state on the SP-1:

  * internal-flash erase or write (Zephyr flash API, fstorage, NVMC registers)
  * UICR writes, including access-port protection (APPROTECT) changes
  * eMMC / disk / SD writes (the SP-1's stored Tape Looper audio)
  * filesystem, settings or NVS writes
  * MCUboot / partition-slot management

Detection is by symbol name, by source text AND by literal peripheral base
address in the disassembly, so a raw register write with no symbol still trips
the gate.

Explicitly allowed (documented, volatile, no persistent write):
  * NRF_POWER->GPREGRET = 0x57       retention RAM, DFU request
  * NRF_POWER->RESETREAS = 0xFFFF... volatile reset-reason clear
  * NRF_POWER->SYSTEMOFF = 1         deep sleep
  * NRF_WDT->RR[n] / RREN / RUNSTATUS watchdog feeds and status reads
  * existing power / charger GPIOs   electrical only
  * READ-ONLY Zephyr UICR references (listed verbatim below)

Two categories of build/link ARTIFACT are excluded from the symbol/source
scan, both narrowly, both with evidence printed in the report rather than
silently dropped:

  * Kconfig link-time value markers: Zephyr links every referenced boolean/
    int Kconfig option in as an absolute (nm type 'A') symbol literally named
    "CONFIG_<OPTION>" for external debug tooling. These carry no code, no
    data, no section and are never executed or read by the firmware; a
    pattern that happens to match the *option's name* (e.g. "NRF_NVMC" inside
    "CONFIG_HAS_HW_NRF_NVMC_PE", a SoC "this chip has the peripheral"
    capability flag) is not evidence that code using that peripheral is
    linked in. The actual effective Kconfig is separately and exhaustively
    checked against build-stemtape/zephyr/.config in pass B below, which is
    the correct place to fail on a *dangerous option being enabled* — pass A
    only excludes the nm-symbol false-positive, it never excludes pass B.
  * Empty Zephyr iterable-section boundaries: Z_STRUCT_SECTION_LABEL-style
    driver-API registries emit an unconditional "<x>_list_start"/
    "<x>_list_end" symbol pair at link time whenever the API type is
    referenced anywhere in the build graph, independent of whether any
    Kconfig subsystem is enabled or any device object of that type exists.
    When start == end the section holds zero entries: no instance of that
    driver type is linked into the image. Only an exactly-empty pair is
    excluded; a nonzero-size registry still fails.

Usage: m0_safety_gate.py <nm.txt> <objdump-d.txt> <.config> <out-report.md>
"""

from __future__ import annotations

import os
import re
import sys

nm_file, dis_file, cfg_file, out_path = sys.argv[1:5]

SOURCE_ROOTS = [
    "firmware/stemtape",
    "firmware/boards/teenageengineering/stem_player",
]

# ---- symbol / source patterns that must NOT be present ------------------
FORBIDDEN_SYMBOLS = [
    ("internal-flash write/erase",
     r"\bflash_write\b|\bflash_erase\b|flash_area_write|flash_area_erase|"
     r"flash_area_flatten|nrf_fstorage|nrfx_nvmc|soc_flash_nrf|flash_nrf"),
    ("NVMC register access",
     r"NRF_NVMC|->CONFIG\s*=\s*NVMC|ERASEPAGE|ERASEALL|ERASEUICR|"
     r"NVMC_CONFIG_WEN|nvmc_"),
    ("UICR write / access protection",
     r"NRF_UICR->\s*\w+\s*=|UICR->APPROTECT|APPROTECT\s*=|"
     r"nrf_uicr_write|CTRLAP|ctrl_ap|DISABLE_APPROTECT|ENABLE_APPROTECT"),
    ("eMMC / disk / SD write",
     r"disk_access_write|disk_access_ioctl|sdmmc_|\bemmc_\w*write|"
     r"mmc_write|sdhc_"),
    ("filesystem / settings / NVS write",
     r"\bnvs_write\b|\bnvs_init\b|\bnvs_mount\b|settings_save|settings_load|"
     r"\bfs_write\b|\bfs_open\b|\bfs_mount\b|littlefs|fatfs|\bff_\w+_write"),
    ("MCUboot / partition management",
     r"boot_set_next|boot_set_pending|boot_write_img_confirmed|img_mgmt_|"
     r"boot_request_upgrade|flash_img_"),
]

# ---- Kconfig options that must NOT be enabled ---------------------------
FORBIDDEN_CONFIG = [
    "CONFIG_FLASH", "CONFIG_FLASH_MAP", "CONFIG_NVS", "CONFIG_SETTINGS",
    "CONFIG_FILE_SYSTEM", "CONFIG_DISK_ACCESS", "CONFIG_SDMMC_SUBSYS",
    "CONFIG_MMC_STACK", "CONFIG_BOOTLOADER_MCUBOOT", "CONFIG_IMG_MANAGER",
    "CONFIG_STREAM_FLASH", "CONFIG_NRF_APPROTECT_LOCK",
    "CONFIG_NRF_APPROTECT_USER_HANDLING",
]

# ---- literal peripheral windows in the disassembly ----------------------
# (label, low, high) — any literal pool word or immediate landing inside one
# of these windows means the code can address the peripheral directly.
ADDRESS_WINDOWS = [
    ("NVMC (0x4001E000-0x4001EFFF)", 0x4001E000, 0x4001EFFF),
    ("UICR (0x10001000-0x100013FF)", 0x10001000, 0x100013FF),
    ("FICR/CTRL-AP (0x4000C000-0x4000CFFF)", 0x4000C000, 0x4000CFFF),
]

# READ-ONLY UICR references produced by Zephyr itself that are verified safe.
# Each entry is matched against the disassembly symbol context; anything not
# listed here that touches UICR is a failure.
ALLOWED_UICR_READONLY = [
    # Zephyr reads UICR->NRFFW[..] / HFXOSRC-style fields at boot; none write.
    "nrfx_", "SystemInit", "z_arm_platform_init", "soc_early_init_hook",
]

failures: list[str] = []
report: list[str] = ["# Stem Tape M0 — STRICT persistence safety gate", "",
                     "Fail-closed. Static, read-only; no device was written.", ""]


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


# ---- nm-line parsing + the two documented false-positive exclusions -----
def parse_nm_line(line: str):
    """`arm-zephyr-eabi-nm -n -S` prints 'ADDR [SIZE] TYPE NAME'; SIZE is
    omitted for symbols nm cannot size (e.g. absolute symbols). Returns
    (addr, size_or_None, type, name) or None if the line doesn't parse."""
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
    """Absolute (type 'A') symbol literally named CONFIG_<OPTION>: a linked
    Kconfig value marker, not code or data. See module docstring."""
    parsed = parse_nm_line(nm_line)
    if parsed is None:
        return False
    _addr, _size, typ, name = parsed
    return typ == "A" and name.startswith("CONFIG_")


def empty_iterable_boundary(nm_line: str) -> bool:
    """'<x>_list_start' / '<x>_list_end' pair from Zephyr's iterable-section
    macros, present with start == end, i.e. the registry holds zero entries.
    See module docstring."""
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


# --- pass A: symbols + source text ---------------------------------------
report.append("## A. Symbol and source scan")
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
        report.append(f"  Excluded as Kconfig link-time markers (absolute symbol, "
                       f"CONFIG_-named, no code/data — see module docstring): "
                       f"{len(excluded_kconfig)}")
        report.append("```text")
        report.extend(excluded_kconfig[:50])
        report.append("```")
    if excluded_iterable:
        report.append("")
        report.append(f"  Excluded as empty iterable-section boundaries "
                       f"(start == end, zero linked instances): "
                       f"{len(excluded_iterable)}")
        report.append("```text")
        report.extend(excluded_iterable[:50])
        report.append("```")
    report.append("")

# --- pass B: Kconfig -----------------------------------------------------
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
    report.append("PASS — none of "
                  + ", ".join(FORBIDDEN_CONFIG) + " is enabled.")
report.append("")

# --- pass C: literal peripheral addresses in the disassembly -------------
report.append("## C. Literal peripheral-address scan (register writes without symbols)")
report.append("")
hexrx = re.compile(r"0x([0-9a-fA-F]{8})")
symrx = re.compile(r"^[0-9a-f]+ <([^>]+)>:")

# Index function boundaries so a hit can be traced to the complete
# enclosing disassembly — the actual str/ldr instruction and register
# offset, not just the literal-pool word — before any finding is accepted
# or excluded.
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


# nRF52840 NVMC register map (offsets from base 0x4001E000). Only ICACHECNF
# is a volatile, non-destructive write; every other write-capable offset
# alters flash state and must fail the gate. (Read-only status/count
# registers are listed for completeness but are irrelevant to a WRITE scan.)
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
# Matches any store mnemonic (str/strb/strh, with an optional .w width suffix).
STORE_MNEM_RX = re.compile(r"\bstr[bh]?(?:\.w)?\s")
# objdump prints the resolved absolute immediate as a trailing "; 0xNNN"
# comment on indexed str instructions — that comment IS the effective
# byte offset from the base register, independent of the raw encoded field.
STORE_OFFSET_RX = re.compile(r";\s*0x([0-9a-fA-F]+)\s*$")
CALL_RX = re.compile(r"\bbl[x]?(?:\.w)?\s")
# A PC-relative literal load: "ldr rX, [pc, #N] ; (ADDR <sym+off>)". Group 1 is
# the destination register, group 2 the resolved literal-pool address — used
# to identify exactly which register receives a given literal-pool value.
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
                      f"0x4001E540) is the sole permitted write: an nRF52 "
                      f"instruction-cache enable/disable bit that resets on every "
                      f"power cycle and touches no flash. Every other offset — "
                      f"CONFIG, ERASEPAGE, ERASEALL, ERASEPAGEPARTIAL, ERASEUICR, "
                      f"ERASEPAGEPARTIALCFG — is a flash write/erase control and "
                      f"fails the gate. Failing closed: an unresolved base register, "
                      f"a store whose offset cannot be resolved, or a function that "
                      f"branches out (bl/blx) while the base may still be live in a "
                      f"register all fail the gate.")
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

            # Which register does this literal end up in? Match the PC-relative
            # load whose resolved-target comment points at this literal's own
            # address (objdump prints "; (<addr> <sym+off>)" on such loads).
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
    # Full disassembly of every distinct enclosing function, so the exact
    # instruction, register and offset that uses this base address can be
    # read directly from the audit artifact/job log — required evidence
    # before this (or any future) finding may ever be allow-listed.
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

# --- explicitly permitted volatile operations ----------------------------
report.append("## D. Permitted volatile operations (documented, not persistent)")
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
    report.append("GATE PASSED — no persistent-write capability detected in the image.")
report.append("")

open(out_path, "w").write("\n".join(report) + "\n")
print("\n".join(report))
sys.exit(1 if failures else 0)
