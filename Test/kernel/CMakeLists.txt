cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

project(pebble LANGUAGES C ASM)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -nostdlib -Wall -Wextra -Wpedantic -O3 -std=gnu99")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -nostdlib")

include_directories(
  include
)

add_executable(pebble
  src/gui.c
  src/main.c
  src/reVita.c
  src/exceptions.S
  src/exceptions.c
)

target_link_libraries(pebble
  taihenModuleUtils_stub
  SceLibc_stub_weak
  SceCtrlForDriver_stub
  SceDebugForDriver_stub
  SceSysmemForDriver_stub
  SceDisplayForDriver_stub
  SceSysclibForDriver_stub
  SceThreadmgrForDriver_stub
  SceIofilemgrForDriver_stub
  SceKernelSuspendForDriver_stub
  SceSysmemForKernel_stub_weak
  SceExcpmgrForKernel_stub_weak
  SceThreadmgrForKernel_stub_weak
  SceModulemgrForKernel_stub_weak
  SceProcessmgrForKernel_stub_weak
  SceSysrootForKernel_stub
  SceProcEventForDriver_stub
)

vita_create_self(pebble.skprx pebble
  CONFIG exports.yml
  UNSAFE
)

add_custom_target(copy_skprx ALL
  COMMAND ${CMAKE_COMMAND} -E copy
  "${CMAKE_CURRENT_BINARY_DIR}/pebble.skprx"
  "${CMAKE_BINARY_DIR}/../../bin/pebble.skprx"
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/pebble.skprx"
)