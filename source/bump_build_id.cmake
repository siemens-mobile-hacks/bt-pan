# Auto-increment the build id (shown on the elf's screen as Dxx). Invoked before each build:
#   cmake -DNUMFILE=<path> -DOUT=<path> -P bump_build_id.cmake
if(EXISTS "${NUMFILE}")
  file(READ "${NUMFILE}" _n)
  string(STRIP "${_n}" _n)
else()
  set(_n 19)
endif()
math(EXPR _n "${_n} + 1")
file(WRITE "${NUMFILE}" "${_n}\n")
file(WRITE "${OUT}" "#define BT_PAN_BUILD_TAG \"D${_n}\"\n")
message(STATUS "BUILD bt-pan D${_n}")
