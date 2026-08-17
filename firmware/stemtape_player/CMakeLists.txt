cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(stemtape-player)

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

target_sources(app PRIVATE
	src/main.c
	src/led_duty.c
	src/led_render.c
	src/led_render_policy.c
	src/st_crc32.c
	src/st_storage_layout.c
	src/st_sector_codec.c
	src/st_transfer.c
	src/st_gesture.c
	src/st_scrub.c
	src/st_led_pattern.c
)

target_link_libraries(app PRIVATE m)
