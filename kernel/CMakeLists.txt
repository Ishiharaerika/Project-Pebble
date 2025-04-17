cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

project(pebble_k LANGUAGES C ASM)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -nostdlib -Wall -Wextra -Wpedantic -O3 -std=gnu99")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -nostdlib")

include_directories(
  include
)

add_executable(pebble_k
  src/main.c
  src/exceptions.S
  src/exceptions.c
)

target_link_libraries(pebble_k
  taihenModuleUtils_stub
  SceThreadmgrForKernel_stub
  SceThreadmgrForDriver_stub
  SceSysmemForDriver_stub
  SceSysmemForKernel_stub
  SceExcpmgrForKernel_stub
  SceSysclibForDriver_stub 
  SceModulemgrForKernel_stub
  SceProcessmgrForKernel_stub
  SceDebugForDriver_stub
)

vita_create_self(pebble_k.skprx pebble_k
  CONFIG exports.yml
  UNSAFE
)

vita_create_stubs(stubs
  "pebble_k"
  exports.yml
  KERNEL
)

add_library(pebble_k_stub STATIC IMPORTED GLOBAL)

set_target_properties(pebble_k_stub PROPERTIES
  IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/libpebble_k_stub.a"
)
