cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(stemtape-player)

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

# src/main.c and src/sp1_emmc.c are real derivatives of the proven Tape
# Looper (firmware/src/main.c, firmware/src/sp1_emmc.c) -- see main.c's
# provenance banner.
#
# GATE 1: src/st_midi_queue.c is the pure, host-testable USB-MIDI receive
# decode + bounded event queue (see st_midi_queue.h). main.c's real
# rx_packet_cb is a thin adapter that calls st_midi_decode_ump32() on the
# incoming UMP word -- the SAME function tests/test_stemtape_player.c
# exercises on the host -- so this is genuinely linked, not a parallel
# unwired module.
#
# STEM TAPE v1.1 (docs/stem-tape-transfer-v1.1.md is the current,
# authoritative contract): src/st_crc32.c, src/st_checksum32.c,
# src/st_sector_v11.c, src/st_stix.c, src/st_stcp.c, src/st_ab_session.c --
# the real STIX v2 index parse/validate/select, STSC sector codec, A/B
# region layout + STCP capability reply, and the stateful write-safety
# session gate (see each header's own doc) -- are linked: main.c's real
# 'Q' handler and boot-time A/B selection call these SAME pure functions
# tests/test_stem_v11.c exercises against the frozen Lovable handoff
# fixtures. st_crc32.c is linked for st_stix.c's own record-integrity CRC
# (st_stix_block_crc()), not for any v1.0 purpose.
#
# The OLD, now-retired v1.0 Gate 2 contract's own engine -- src/
# st_storage_layout.c, src/st_sector_codec.c, src/st_transfer.c, src/
# st_library_io.c, src/st_xfer_wire.c, and src/st_stem_validate.c (an
# even earlier, already-superseded design within that same v1.0 line) --
# is DELIBERATELY NOT linked here: main.c no longer calls any command
# handler or adapter that reaches these files' functions at all (the
# v1.1 migration deleted that dispatch code, not merely disabled it), so
# there is no path from this target's command dispatch to their own
# emmc_write_blocks() call sites, and no v1.0 write path is ever present
# in this ELF -- v1.0 and v1.1 can never both hold a live write path in
# the same build, by construction. These files stay in the tree and keep
# their own host tests (tests/test_stemtape_player.c) for regression
# evidence only; a follow-up commit extends the strict persistence safety
# gate to check their symbols are absent from the built ELF directly, as
# positive proof rather than relying on this list alone.
#
# NOT included here (compiled only into tests/test_stemtape_player.c's host
# test binary, not into this firmware target): led_duty.c/led_render.c/
# led_render_policy.c, st_gesture.c, st_scrub.c, st_led_pattern.c --
# st_gesture.c/st_scrub.c/st_led_pattern.c implemented an earlier,
# superseded standalone protocol/runtime design (see git log); main.c
# still drives LEDs directly (its own GPIO+soft-PWM driver, inherited
# from the Tape Looper). Preserved and still host-tested for reference;
# deliberately NOT linked into this ELF so as not to present unwired
# modules as part of the real runtime.
target_sources(app PRIVATE
	src/main.c
	src/sp1_emmc.c
	src/st_midi_queue.c
	src/st_crc32.c
	src/st_checksum32.c
	src/st_sector_v11.c
	src/st_stix.c
	src/st_stcp.c
	src/st_ab_session.c
)

# See firmware/CMakeLists.txt's identical note: the batchbuf/meta-block code
# (inherited unchanged in this derivative) type-puns static uint8_t[]
# storage through other lvalue types; -fno-strict-aliasing removes the TBAA
# assumption that would make that idiom undefined.
target_compile_options(app PRIVATE -fno-strict-aliasing)

target_link_libraries(app PRIVATE m)
