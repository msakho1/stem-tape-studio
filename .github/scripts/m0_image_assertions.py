#!/usr/bin/env python3
"""Stem Tape M0 image assertions.

Read-only static checks on the built ELF/BIN. Never touches hardware.

SP-1 flash map (firmware/boards/teenageengineering/stem_player/stem_player.dts):
  boot_partition  0x00000000 .. 0x00020000  (TE bootloader)
  slot0_partition 0x00020000 .. 0x000FF000  (application, 0xDF000 bytes)
  storage         0x000FF000 .. 0x00100000

nRF52840 RAM: 0x20000000 .. 0x20040000 (256 KiB).
"""

import struct
import subprocess
import sys

APP_ORIGIN = 0x00020000
APP_SIZE = 0x000DF000
APP_LIMIT = APP_ORIGIN + APP_SIZE          # 0x000FF000
RAM_START = 0x20000000
RAM_END = 0x20040000

elf, binary = sys.argv[1], sys.argv[2]

failures: list[str] = []
notes: list[str] = []


def check(ok: bool, label: str, detail: str) -> None:
    print(f"[{'PASS' if ok else 'FAIL'}] {label}: {detail}")
    if not ok:
        failures.append(f"{label}: {detail}")


readelf = subprocess.run(
    ["arm-zephyr-eabi-readelf", "-W", "-S", "-l", elf],
    check=True, capture_output=True, text=True,
).stdout
print(readelf)

# ---- parse allocated sections -------------------------------------------
sections = []          # (name, addr, size, flags)
for line in readelf.splitlines():
    line = line.strip()
    if not line.startswith("["):
        continue
    try:
        body = line.split("]", 1)[1].split()
        name, _typ, addr, _off, size = body[0], body[1], body[2], body[3], body[4]
        flags = body[5] if len(body) > 5 else ""
        addr_i, size_i = int(addr, 16), int(size, 16)
    except (IndexError, ValueError):
        continue
    if "A" in flags and size_i > 0:
        sections.append((name, addr_i, size_i, flags))

# LMA (load address) per segment, for RAM sections copied from flash.
segments = []          # (vaddr, paddr, filesz, memsz)
for line in readelf.splitlines():
    s = line.strip()
    if s.startswith("LOAD"):
        f = s.split()
        segments.append((int(f[2], 16), int(f[3], 16), int(f[4], 16), int(f[5], 16)))

flash_spans = [(p, p + fsz) for (_v, p, fsz, _m) in segments if fsz and p < RAM_START]

# 1. application origin
origins = [lo for lo, _hi in flash_spans]
check(bool(origins) and min(origins) == APP_ORIGIN,
      "1 application origin is exactly 0x20000",
      f"lowest flash LMA = {hex(min(origins)) if origins else 'none'}")

# 2/3. allocated section bounds
below = [(n, hex(a)) for n, a, _s, _f in sections
         if a < APP_ORIGIN and not (RAM_START <= a < RAM_END)]
check(not below, "2 no allocated section below 0x20000", f"violations = {below or 'none'}")

over = [(n, hex(a + s)) for n, a, s, _f in sections
        if a < RAM_START and a + s > APP_LIMIT]
over += [(f"LOAD@{hex(lo)}", hex(hi)) for lo, hi in flash_spans if hi > APP_LIMIT]
check(not over, "3 no allocated section beyond the SP-1 application limit",
      f"limit = {hex(APP_LIMIT)}, violations = {over or 'none'}")

# 4. no UICR
uicr = [n for n, a, _s, _f in sections
        if "uicr" in n.lower() or 0x10001000 <= a < 0x10002000]
uicr += [f"LOAD@{hex(lo)}" for lo, _hi in flash_spans if 0x10001000 <= lo < 0x10002000]
check(not uicr, "4 no UICR section present", f"matches = {uicr or 'none'}")

# ---- vector table --------------------------------------------------------
with open(binary, "rb") as fh:
    head = fh.read(4 * 64)
image_size = __import__("os").path.getsize(binary)
words = struct.unpack("<%dI" % (len(head) // 4), head)

# 5. initial stack pointer
sp = words[0]
check(RAM_START < sp <= RAM_END, "5 vector-table stack pointer within nRF52840 RAM",
      f"SP = {hex(sp)} (RAM {hex(RAM_START)}..{hex(RAM_END)})")

# 6. nonzero handlers inside the application
bad = []
for i, w in enumerate(words[1:], start=1):
    if w == 0 or w == 0xFFFFFFFF:
        continue
    addr = w & ~1
    if not (APP_ORIGIN <= addr < APP_ORIGIN + image_size):
        bad.append((i, hex(w)))
check(not bad, "6 every nonzero vector handler points inside the application",
      f"out-of-range entries = {bad or 'none'}")

# 7. size within bootloader allowance
check(image_size <= APP_SIZE, "7 binary size within SP-1 bootloader allowance",
      f"{image_size} bytes / allowance {APP_SIZE} bytes "
      f"({image_size * 100.0 / APP_SIZE:.2f}%)")

print()
if failures:
    print("ASSERTIONS FAILED:")
    for f in failures:
        print(" - " + f)
    sys.exit(1)
print("ALL IMAGE ASSERTIONS PASSED")
for n in notes:
    print("note: " + n)
