cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(stemtape-m0)

# Shared device_next USB bring-up helper (sample_usbd_init_device), the same
# one the SP-1 looper application uses.
include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

target_sources(app PRIVATE
	src/main.c
	src/led_battery.c
	src/led_duty.c
	src/led_frame.c
	src/led_midi.c
	src/led_render.c
)
