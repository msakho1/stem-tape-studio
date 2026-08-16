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
    hits += [f"symbol: {l}" for l in nm_lines if rx.search(l)]
    if hits:
        failures.append(f"{label}: {len(hits)} reference(s)")
        report.append(f"FAIL — {label}: {len(hits)} reference(s)")
        report.append("")
        report.append("```text")
        report.extend(hits[:200])
        report.append("```")
    else:
        report.append(f"PASS — {label}: none found.")
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
current_sym = "?"
symrx = re.compile(r"^[0-9a-f]+ <([^>]+)>:")
found = {label: [] for label, _lo, _hi in ADDRESS_WINDOWS}
for line in dis:
    m = symrx.match(line)
    if m:
        current_sym = m.group(1)
        continue
    for h in hexrx.findall(line):
        v = int(h, 16)
        for label, lo, hi in ADDRESS_WINDOWS:
            if lo <= v <= hi:
                found[label].append(f"{current_sym}: {line.strip()[:180]}")

for label, lo, hi in ADDRESS_WINDOWS:
    hits = found[label]
    if not hits:
        report.append(f"PASS — {label}: no literal reference.")
        report.append("")
        continue
    if label.startswith("UICR"):
        unexpected = [h for h in hits
                      if not any(a in h for a in ALLOWED_UICR_READONLY)]
        report.append(f"{'FAIL' if unexpected else 'PASS'} — {label}: "
                      f"{len(hits)} reference(s), "
                      f"{len(unexpected)} outside the verified read-only allow-list "
                      f"({', '.join(ALLOWED_UICR_READONLY)}).")
        report.append("")
        report.append("```text")
        report.extend(hits[:200])
        report.append("```")
        report.append("")
        if unexpected:
            failures.append(f"{label}: {len(unexpected)} non-allow-listed reference(s)")
        continue
    failures.append(f"{label}: {len(hits)} literal reference(s)")
    report.append(f"FAIL — {label}: {len(hits)} literal reference(s)")
    report.append("")
    report.append("```text")
    report.extend(hits[:200])
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
