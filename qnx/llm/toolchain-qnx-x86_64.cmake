# CMake toolchain for QNX SDP 8.0, x86_64 target. Source qnxsdp-env.sh first so QNX_HOST and QNX_TARGET are set.
set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_VERSION 8.0.0)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(QNX_HOST "$ENV{QNX_HOST}")
set(QNX_TARGET "$ENV{QNX_TARGET}")
if(NOT QNX_HOST OR NOT QNX_TARGET)
    message(FATAL_ERROR "QNX_HOST and QNX_TARGET are not set: source qnxsdp-env.sh")
endif()

set(CMAKE_C_COMPILER "${QNX_HOST}/usr/bin/x86_64-pc-nto-qnx8.0.0-gcc")
set(CMAKE_CXX_COMPILER "${QNX_HOST}/usr/bin/x86_64-pc-nto-qnx8.0.0-g++")
set(CMAKE_AR "${QNX_HOST}/usr/bin/x86_64-pc-nto-qnx8.0.0-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${QNX_HOST}/usr/bin/x86_64-pc-nto-qnx8.0.0-ranlib" CACHE FILEPATH "")

set(CMAKE_SYSROOT "${QNX_TARGET}")
set(CMAKE_FIND_ROOT_PATH "${QNX_TARGET}" "${QNX_TARGET}/x86_64")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# QNX hides some POSIX and BSD declarations unless an extension level is requested.
add_compile_definitions(_QNX_SOURCE)
