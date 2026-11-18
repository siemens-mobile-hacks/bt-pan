cmake_minimum_required(VERSION 3.28.2)

include(../sdk/config.cmake)
project(bt-pan C)

# Our SDK splits the runtime into crt + crt_helper (crt_helper carries memcpy etc.); it has no
# memset, so freestanding.c supplies one.
link_libraries(crt crt_helper gcc)
add_definitions(-DSWILIB_MODERN -D__NO_LIBC)

set(SOURCES
	src/main.c
	src/bt_panu.c
	src/bnep.c
	src/arp.c
	src/dhcp.c
	src/icmp_probe.c
	src/bt_pan_netif.c
	src/pan_lman.c
	src/peers.c
	src/lib.c
	src/speedtest.c
	src/ping.c
	src/freestanding.c
)

# Builds from one source: ELKA (EL71/E71 sw45) and SG (S65 sw58, #ifdef SGOLD; S65 BT in
# src/bt_sg.h). ELKA BTPAN priority 0x60 (as on S65; was 0x80, starved).
# NSG: NewSGOLD without ELKA (S75 sw52, 132x176): the ELKA code with NSG menu flags,
# keys and YDISP (swilib ui.h/gui.h), linked against lib/NSG.
foreach(NAME ELKA NSG SG)
	set(PLATFORM ${NAME})
	add_executable(bt-pan_${NAME} ${SOURCES})
	target_include_directories(bt-pan_${NAME} PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}")
	target_sdk_setup(bt-pan_${NAME} ${PLATFORM})
	target_compile_options(bt-pan_${NAME} BEFORE PUBLIC -std=c11 -Wall -Werror -Wno-error=main)
	set_target_properties(bt-pan_${NAME} PROPERTIES OUTPUT_NAME "bt-pan_${NAME}")
	add_dependencies(bt-pan_${NAME} bump_build_id)
endforeach()

# Auto-increment the build tag (Dxx) on every build: regenerate build_id.h from .buildnum.
set(BUILD_ID_HEADER "${CMAKE_CURRENT_BINARY_DIR}/build_id.h")
add_custom_target(bump_build_id ALL
	BYPRODUCTS "${BUILD_ID_HEADER}"
	COMMAND "${CMAKE_COMMAND}"
		"-DNUMFILE=${CMAKE_CURRENT_LIST_DIR}/.buildnum"
		"-DOUT=${BUILD_ID_HEADER}"
		-P "${CMAKE_CURRENT_LIST_DIR}/bump_build_id.cmake"
	VERBATIM
)
