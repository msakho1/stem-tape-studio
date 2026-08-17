cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(stemtape-player)

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

# src/main.c and src/sp1_emmc.c are real derivatives of the proven Tape
# Looper (firmware/src/main.c, firmware/src/sp1_emmc.c) -- see main.c's
# provenance banner. src/st_crc32.c and src/st_stem_validate.c are real,
# newly added and WIRED INTO main.c's xfer_service() 'Z' verb (the stem-song
# commit gate), not standalone/unused modules.
#
# NOT included here (compiled only into tests/test_stemtape_player.c's host
# test binary, not into this firmware target): led_duty.c/led_render.c/
# led_render_policy.c, st_gesture.c, st_scrub.c, st_led_pattern.c,
# st_storage_layout.c, st_sector_codec.c, st_transfer.c -- these implemented
# an earlier, superseded standalone protocol/runtime design (see git log);
# main.c now drives LEDs directly (its own GPIO+soft-PWM driver, inherited
# from the Tape Looper) and storage/transfer through the classic
# xfer_service()/meta_blk path instead. Preserved and still host-tested for
# reference; deliberately NOT linked into this ELF so as not to present
# unwired modules as part of the real runtime.
target_sources(app PRIVATE
	src/main.c
	src/sp1_emmc.c
	src/st_crc32.c
	src/st_stem_validate.c
)

# See firmware/CMakeLists.txt's identical note: the batchbuf/meta-block code
# (inherited unchanged in this derivative) type-puns static uint8_t[]
# storage through other lvalue types; -fno-strict-aliasing removes the TBAA
# assumption that would make that idiom undefined.
target_compile_options(app PRIVATE -fno-strict-aliasing)

target_link_libraries(app PRIVATE m)
