cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()


project(vitacompanion)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITACOMPANION_FTP_PORT 1337 CACHE STRING "FTP control port")
set(VITACOMPANION_CMD_PORT 1338 CACHE STRING "Command server port")
set(VITACOMPANION_MODULE_NAME "vitacompanion" CACHE STRING "Vita module name")

if(NOT VITACOMPANION_FTP_PORT MATCHES "^[0-9]+$" OR
   VITACOMPANION_FTP_PORT LESS 1 OR VITACOMPANION_FTP_PORT GREATER 65535)
  message(FATAL_ERROR "VITACOMPANION_FTP_PORT must be between 1 and 65535")
endif()
if(NOT VITACOMPANION_CMD_PORT MATCHES "^[0-9]+$" OR
   VITACOMPANION_CMD_PORT LESS 1 OR VITACOMPANION_CMD_PORT GREATER 65535)
  message(FATAL_ERROR "VITACOMPANION_CMD_PORT must be between 1 and 65535")
endif()
if(NOT VITACOMPANION_MODULE_NAME MATCHES "^[A-Za-z0-9_]+$")
  message(FATAL_ERROR "VITACOMPANION_MODULE_NAME must contain only letters, digits, and underscores")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -fno-pic -fno-PIC -std=gnu99")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fno-rtti -fno-exceptions")

include_directories(
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/vendor/libftpvita
)

link_directories(
  ${CMAKE_CURRENT_BINARY_DIR}
)

add_library(ftpvita STATIC
        vendor/libftpvita/ftpvita.c
        vendor/libftpvita/ftpvita.h
        vendor/libftpvita/ftpvita_io.c
        vendor/libftpvita/ftpvita_io.h
        vendor/libftpvita/ftpvita_path.c
        vendor/libftpvita/ftpvita_path.h
        vendor/libftpvita/ftpvita_protocol.c
        vendor/libftpvita/ftpvita_protocol.h)

add_executable(vitacompanion_kernel
        kernel/main.c)

target_compile_definitions(vitacompanion_kernel PRIVATE
  __VITA_KERNEL__
)

target_link_libraries(vitacompanion_kernel
  taihenForKernel_stub
  SceCtrlForDriver_stub
  SceThreadmgrForDriver_stub
  gcc
)

set_target_properties(vitacompanion_kernel
  PROPERTIES LINK_FLAGS "-nostdlib"
)

vita_create_self(vitacompanion_kernel.skprx vitacompanion_kernel
  CONFIG ${CMAKE_SOURCE_DIR}/kernel/vitacompanion_kernel.yml
)
vita_create_stubs(vitacompanion_kernel_stubs vitacompanion_kernel
  ${CMAKE_SOURCE_DIR}/kernel/vitacompanion_kernel.yml KERNEL
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
        src/nosleep.c
        src/nosleep.h
        src/parser.c
        src/parser.h
        src/input.c
        src/input.h
        src/input_parse.c
        src/version.c
        src/version.h
        src/cmd_definitions.c
        src/cmd_definitions.h)

target_compile_definitions(ftpvita PRIVATE
  FTP_PORT=${VITACOMPANION_FTP_PORT}
)
target_compile_definitions(vitacompanion PRIVATE
  CMD_PORT=${VITACOMPANION_CMD_PORT}
)

add_dependencies(vitacompanion vitacompanion_kernel_stubs)

target_link_libraries(vitacompanion
  taipool
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
  SceKernelModulemgr_stub
  SceProcessmgr_stub_weak
  ScePower_stub
  ScePromoterUtil_stub_weak
  ${CMAKE_CURRENT_BINARY_DIR}/libvitacompanion_kernel_stub.a
)

set_target_properties(vitacompanion
  PROPERTIES LINK_FLAGS "-nostdlib"
)

file(READ "${CMAKE_SOURCE_DIR}/vitacompanion.yml" VITACOMPANION_EXPORTS)
string(REGEX REPLACE "^vitacompanion:"
  "${VITACOMPANION_MODULE_NAME}:"
  VITACOMPANION_EXPORTS "${VITACOMPANION_EXPORTS}")
set(VITACOMPANION_EXPORTS_FILE
  "${CMAKE_CURRENT_BINARY_DIR}/${VITACOMPANION_MODULE_NAME}.yml")
file(WRITE "${VITACOMPANION_EXPORTS_FILE}" "${VITACOMPANION_EXPORTS}")

vita_create_self(${VITACOMPANION_MODULE_NAME}.suprx vitacompanion
  UNSAFE
  CONFIG ${VITACOMPANION_EXPORTS_FILE}
)
