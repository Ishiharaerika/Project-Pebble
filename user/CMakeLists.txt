project(pebble_user)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -O3 -std=gnu99")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -nostdlib")

add_executable(pebble_user
  src/main.c
)

target_link_libraries(pebble_user PUBLIC
  pebble_stub_weak
  SceSysmem_stub
  SceDisplay_stub
  SceLibKernel_stub
  SceKernelThreadMgr_stub
)

vita_create_self(pebble.suprx pebble_user
  CONFIG exports.yml
  UNSAFE
)

add_custom_target(copy_suprx ALL
  COMMAND ${CMAKE_COMMAND} -E copy
  "${CMAKE_CURRENT_BINARY_DIR}/pebble.suprx"
  "${CMAKE_BINARY_DIR}/../bin/pebble.suprx"
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/pebble.suprx"
)