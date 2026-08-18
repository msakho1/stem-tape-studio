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
# GATE 2: src/st_crc32.c, src/st_storage_layout.c, src/st_sector_codec.c,
# src/st_transfer.c, src/st_library_io.c, src/st_xfer_wire.c -- the real,
# current Stem Tape library-header + transactional song-transfer engine
# (see st_transfer.h/st_storage_layout.h/st_library_io.h's own docs) --
# are now linked: main.c's real xfer_service() 'V'/'B'/'S'/'K'/'C'/'A'/
# 'D'/'I' verb handlers call these SAME pure functions the host tests
# exercise, through the region-bounded eMMC adapters
# (xfer_staging_write()/xfer_header_write()/xfer_songdata_write() in
# main.c). This is the ONLY route to emmc_write_blocks() this target
# links -- see the strict persistence safety gate.
#
# st_stem_validate.c -- an EARLIER, superseded validated-commit design
# (its own header docstring describes a 'Z' verb and TRACK_BLOCKS-based
# present[] mask that were never implemented; st_transfer.c's real
# st_xfer_verify()/st_xfer_commit_precheck() supersede it with an
# independently-designed, already-tested equivalent) -- is NOT linked.
# Preserved and still host-tested for reference, matching how this file
# already treats every other superseded/reference-only module below.
#
# NOT included here (compiled only into tests/test_stemtape_player.c's host
# test binary, not into this firmware target): led_duty.c/led_render.c/
# led_render_policy.c, st_gesture.c, st_scrub.c, st_led_pattern.c,
# st_stem_validate.c -- st_gesture.c/st_scrub.c/st_led_pattern.c implemented
# an earlier, superseded standalone protocol/runtime design (see git log);
# main.c still drives LEDs directly (its own GPIO+soft-PWM driver,
# inherited from the Tape Looper). Preserved and still host-tested for
# reference; deliberately NOT linked into this ELF so as not to present
# unwired modules as part of the real runtime.
target_sources(app PRIVATE
	src/main.c
	src/sp1_emmc.c
	src/st_midi_queue.c
	src/st_crc32.c
	src/st_storage_layout.c
	src/st_sector_codec.c
	src/st_transfer.c
	src/st_library_io.c
	src/st_xfer_wire.c
)

# See firmware/CMakeLists.txt's identical note: the batchbuf/meta-block code
# (inherited unchanged in this derivative) type-puns static uint8_t[]
# storage through other lvalue types; -fno-strict-aliasing removes the TBAA
# assumption that would make that idiom undefined.
target_compile_options(app PRIVATE -fno-strict-aliasing)

target_link_libraries(app PRIVATE m)
