cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

project(pebbel_k LANGUAGES C ASM)

set(ELF_TARGET ${PROJECT_NAME})
set(SKPRX_FILE ${ELF_TARGET}.skprx)
set(STUB_TARGET_NAME ${PROJECT_NAME}_stub)
set(STUB_LIB_FILE "lib${STUB_TARGET_NAME}.a")
set(STUB_GENERATION_TARGET stubs)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -nostdlib -Wall -Wextra -Wpedantic -O3 -std=gnu99")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -nostdlib")

include_directories(
  include
)

add_executable(${ELF_TARGET}
  src/main.c
  src/exceptions.S
  src/exceptions.c
)

target_link_libraries(${ELF_TARGET}
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

vita_create_self(${SKPRX_FILE} ${ELF_TARGET}
  CONFIG exports.yml
  UNSAFE
)

vita_create_stubs(${STUB_GENERATION_TARGET}
  "${ELF_TARGET}"
  exports.yml
  KERNEL
)

add_library(${STUB_TARGET_NAME} STATIC IMPORTED GLOBAL)

add_dependencies(${STUB_TARGET_NAME} ${STUB_GENERATION_TARGET})

set_target_properties(${STUB_TARGET_NAME} PROPERTIES
  IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/${STUB_LIB_FILE}"
)