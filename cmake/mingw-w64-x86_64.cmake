# SPDX-License-Identifier: MIT
#
# Cross-compile Selsearch for 64-bit Windows from Linux with MinGW-w64.
# Used to check that the Windows port builds; release builds use MSVC (CI).
#
#   cmake -B build-win -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
#       -DQT_HOST_PATH=/opt/qt/6.8.3/gcc_64 \
#       -DCMAKE_PREFIX_PATH="/opt/qtwin/6.8.3/mingw_64;/opt/onnxruntime-win/onnxruntime-win-x64-1.23.2"
#
# Needs the posix-threads flavour of the toolchain (std::thread, std::mutex):
#   apt install g++-mingw-w64-x86-64-posix mingw-w64-tools
# See docs/BUILDING-windows.md.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TRIPLE x86_64-w64-mingw32)
find_program(CMAKE_C_COMPILER NAMES ${MINGW_TRIPLE}-gcc-posix ${MINGW_TRIPLE}-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${MINGW_TRIPLE}-g++-posix ${MINGW_TRIPLE}-g++ REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${MINGW_TRIPLE}-windres REQUIRED)

# Headers and libraries come from the MinGW sysroot and CMAKE_PREFIX_PATH
# (the Windows Qt and onnxruntime, outside the sysroot, hence BOTH). Programs
# (moc, qmlcachegen, ...) are the host's, found through QT_HOST_PATH.
set(CMAKE_FIND_ROOT_PATH /usr/${MINGW_TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# The onnxruntime zip has an MSVC import library (onnxruntime.lib), which GNU
# ld links fine; let find_library() consider the .lib suffix.
set(CMAKE_FIND_LIBRARY_SUFFIXES .dll.a .a .lib)
