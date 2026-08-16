#!/usr/bin/env python3
"""Stem Tape M0 persistent-memory / power audit report.

Two passes, both read-only:
  A. source + configuration scan (firmware/stemtape/**, SP-1 board files, .config)
  B. linked-symbol scan (arm-zephyr-eabi-nm output of the built ELF)

Every category is always printed, including "none found", so findings can never
be silently dropped to make the report look clean. This script never fails the
job; it is evidence, not a gate.

Usage: m0_persistence_audit.py <nm-output-file> <out-report.md>
"""

import os
import re
import sys

nm_file, out_path = sys.argv[1], sys.argv[2]

CATEGORIES = [
    ("UICR", r"UICR|uicr",
     "UICR is the one-time-programmable user configuration registers "
     "(NRF_UICR). Any write is persistent and only clearable by ERASEUICR."),
    ("NVMC", r"NVMC|nvmc",
     "NVMC is the non-volatile memory controller. CONFIG=WEN/EEN plus a write "
     "or ERASEPAGE is the only way to alter internal flash; presence means "
     "persistent write capability exists in the image."),
    ("flash_write / flash_erase", r"flash_write|flash_erase|flash_area_write|flash_area_erase|nrf_fstorage|soc_flash",
     "Zephyr flash API writes/erases. Persistent by definition."),
    ("ERASEPAGE / ERASEALL / ERASEUICR", r"ERASEPAGE|ERASEALL|ERASEUICR|eraseall|erasepage",
     "Direct NVMC erase triggers. ERASEALL destroys the whole device including "
     "the TE bootloader."),
    ("NVS / settings / filesystem", r"\bnvs_|CONFIG_NVS|settings_|CONFIG_SETTINGS|fs_open|fs_mount|CONFIG_FILE_SYSTEM|littlefs|fatfs",
     "Key-value or filesystem subsystems; all write to a flash partition."),
    ("disk / eMMC access", r"disk_access|sdmmc|emmc|EMMC|CONFIG_DISK|mmc_",
     "Access to the SP-1 audio storage device. Any write path could damage "
     "stored Tape Looper audio."),
    ("partition management / MCUboot", r"mcuboot|MCUBOOT|boot_set|img_mgmt|slot0_partition|slot1_partition|storage_partition|BOOTLOADER_MCUBOOT",
     "Image-slot or partition manipulation. Marking/swapping slots writes "
     "flash trailers."),
    ("GPREGRET", r"GPREGRET|gpregret",
     "POWER->GPREGRET retention register. RAM-backed, survives soft reset but "
     "is NOT persistent flash; used to request bootloader/DFU mode."),
    ("RESETREAS", r"RESETREAS|resetreas|hwinfo_get_reset_cause",
     "Reset-reason register read/clear. Volatile register, not flash."),
    ("SYSTEM_OFF", r"SYSTEM_OFF|system_off|pm_state_force|POWER_SYSTEMOFF|sys_poweroff",
     "Deep sleep / power-off entry. No memory is written."),
    ("watchdog configuration", r"\bwdt_|WATCHDOG|CONFIG_WDT|wdt_setup|wdt_feed|nrf_wdt",
     "WDT peripheral. On nRF52840 the watchdog config registers are volatile; "
     "no flash is written."),
    ("power / battery-charger GPIOs", r"charger|CHG_|chg_|BATT|batt|VBUS|vbus|pwr_|PWR_|regulator|npm|bq2|POWER_HOLD|power_hold",
     "Power-path or charger control lines. Electrical effect only; no "
     "persistent memory write."),
]

SOURCE_ROOTS = [
    "firmware/stemtape",
    "firmware/boards/teenageengineering/stem_player",
]
EXTRA_FILES = [
    "build-stemtape/zephyr/.config",
    ".github/workflows/firmware.yml",
]


def source_files():
    for root in SOURCE_ROOTS:
        for dirpath, _dirs, files in os.walk(root):
            for f in files:
                yield os.path.join(dirpath, f)
    for f in EXTRA_FILES:
        if os.path.exists(f):
            yield f


files = sorted(set(source_files()))
nm_lines = open(nm_file, errors="ignore").read().splitlines()

out = ["# Stem Tape M0 — persistent-memory and power audit", "",
       "Static, read-only. No device was flashed or written.", "",
       f"Scanned source/config files: {len(files)}",
       f"Linked symbols examined: {len(nm_lines)}", ""]

for name, pattern, explanation in CATEGORIES:
    rx = re.compile(pattern)
    src_hits = []
    for path in files:
        try:
            with open(path, errors="ignore") as fh:
                for n, line in enumerate(fh, 1):
                    if rx.search(line):
                        src_hits.append((path, n, line.strip()[:200]))
        except OSError:
            continue
    sym_hits = [l for l in nm_lines if rx.search(l)]

    out.append(f"## {name}")
    out.append("")
    out.append(f"Operation: {explanation}")
    out.append("")
    if not src_hits and not sym_hits:
        out.append("Findings: none found in source, configuration or linked symbols.")
        out.append("")
        out.append("Can write persistent memory: no (absent from the image).")
        out.append("")
        continue
    out.append(f"Findings: {len(src_hits)} source/config, {len(sym_hits)} linked symbols.")
    out.append("")
    if src_hits:
        out.append("Source / configuration references:")
        out.append("")
        out.append("```text")
        for path, n, line in src_hits:
            out.append(f"{path}:{n}: {line}")
        out.append("```")
        out.append("")
    if sym_hits:
        out.append("Linked symbols (arm-zephyr-eabi-nm -n -S):")
        out.append("")
        out.append("```text")
        out.extend(sym_hits)
        out.append("```")
        out.append("")
    out.append("Can write persistent memory: review each reference above against "
               "the stated operation; categories UICR, NVMC, flash_write/erase, "
               "ERASE*, NVS/settings/filesystem, disk/eMMC and partition/MCUboot "
               "are the only ones capable of a persistent write.")
    out.append("")

open(out_path, "w").write("\n".join(out) + "\n")
print("\n".join(out))
