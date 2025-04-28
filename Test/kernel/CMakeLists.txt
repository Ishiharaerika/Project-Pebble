project(pebble LANGUAGES C ASM)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -nostdlib -Wall -Wextra -Wpedantic -O3 -std=gnu99")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -nostdlib")

add_library(pebble_stub_weak STATIC IMPORTED GLOBAL)

set_target_properties(pebble_stub_weak PROPERTIES
  IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/libpebble_stub_weak.a"
)

include_directories(
  include
)

add_executable(pebble
  src/gui.c
  src/main.c
  src/renderer.c
  src/exceptions.S
  src/exceptions.c
)

target_link_libraries(pebble
  k
  taihenForKernel_stub
  taihenModuleUtils_stub
  SceCpuForDriver_stub
  SceCtrlForDriver_stub
  SceDebugForDriver_stub
  SceAppMgrForDriver_stub
  SceSysmemForDriver_stub
  SceDisplayForDriver_stub
  SceSysclibForDriver_stub
  SceSysrootForDriver_stub
  SceThreadmgrForDriver_stub
  SceIofilemgrForDriver_stub
  SceKernelSuspendForDriver_stub
  SceSysmemForKernel_363_stub_weak
  SceExcpmgrForKernel_363_stub_weak
  SceThreadmgrForKernel_363_stub_weak
  SceModulemgrForKernel_363_stub_weak
  SceProcessmgrForKernel_363_stub_weak
  SceSysrootForKernel_stub
  SceProcEventForDriver_stub
)

vita_create_self(pebble.skprx pebble
  CONFIG exports.yml
  UNSAFE
)

vita_create_stubs(stubs
  pebble
  exports.yml
  KERNEL
)

add_custom_target(copy_skprx ALL
  COMMAND ${CMAKE_COMMAND} -E copy
  "${CMAKE_CURRENT_BINARY_DIR}/pebble.skprx"
  "${CMAKE_BINARY_DIR}/../bin/pebble.skprx"
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/pebble.skprx"
)