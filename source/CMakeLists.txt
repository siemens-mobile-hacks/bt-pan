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
	src/fw.c
	src/speedtest.c
	src/usart.c
	src/freestanding.c
)

add_executable(bt-pan_ELKA ${SOURCES})
target_include_directories(bt-pan_ELKA PRIVATE src)
target_sdk_setup(bt-pan_ELKA ELKA)
target_compile_options(bt-pan_ELKA BEFORE PUBLIC -std=c11 -Wall -Werror -Wno-error=main)
set_target_properties(bt-pan_ELKA PROPERTIES OUTPUT_NAME "bt-pan_ELKA")

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
add_dependencies(bt-pan_ELKA bump_build_id)
target_include_directories(bt-pan_ELKA PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
