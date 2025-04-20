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
  SceLibc_stub
  SceCtrlForDriver_stub
  SceDebugForDriver_stub
  SceSysmemForDriver_stub
  SceSysmemForKernel_stub
  SceDisplayForDriver_stub
  SceSysclibForDriver_stub
  SceExcpmgrForKernel_stub
  SceThreadmgrForKernel_stub
  SceThreadmgrForDriver_stub
  SceModulemgrForKernel_stub
  SceIofilemgrForDriver_stub
  SceProcessmgrForKernel_stub
  #SceSysrootForKernel_stub
  #SceProcEventForDriver_stub
)

vita_create_self(pebble.skprx pebble
  CONFIG exports.yml
  UNSAFE
)

add_custom_target(copy_skprx ALL
  COMMAND ${CMAKE_COMMAND} -E copy
  "${CMAKE_CURRENT_BINARY_DIR}/pebble.skprx"
  "${CMAKE_BINARY_DIR}/../bin/pebble.skprx"
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/pebble.skprx"
)