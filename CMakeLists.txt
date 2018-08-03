cmake_minimum_required(VERSION 2.8)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()


project(vitacompanion)
include("${VITASDK}/share/vita.cmake" REQUIRED)


set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -fno-pic -fno-PIC -std=gnu99")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fno-rtti -fno-exceptions")

include_directories(
)

link_directories(
  ${CMAKE_CURRENT_BINARY_DIR}
)

add_executable(vitacompanion
        src/main.c
        src/main.h
        src/log.c
        src/log.h
        src/cmd.c
        src/cmd.h
        src/net.c
        src/net.h
)


target_link_libraries(vitacompanion
  taipool
  taihen_stub
  ftpvita
  k
  gcc
  SceLibKernel_stub_weak
  SceNet_stub_weak
  SceNetCtl_stub_weak
  SceSysmodule_stub_weak
  SceKernelThreadMgr_stub_weak
  SceIofilemgr_stub_weak
  SceRtc_stub_weak
  SceAppMgr_stub
)

set_target_properties(vitacompanion
  PROPERTIES LINK_FLAGS "-nostdlib"
)

vita_create_self(vitacompanion.suprx vitacompanion
  UNSAFE
  CONFIG ${CMAKE_SOURCE_DIR}/vitacompanion.yml
)
