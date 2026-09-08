# ============================================================================
#  CMake toolchain: cross-compile VoxCast for Windows x64 from Linux/macOS
#  using MinGW-w64. Verified with GCC 12 (posix threads) on Debian 12.
#
#    cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
#    cmake --build build-win -j
#    wine build-win/voxcast_tests.exe
# ============================================================================
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

# The -posix variants are REQUIRED: the default win32 thread model has no
# std::thread / std::mutex / std::condition_variable, which the audio and
# network layers depend on.
find_program(CMAKE_C_COMPILER   NAMES ${TOOLCHAIN_PREFIX}-gcc-posix ${TOOLCHAIN_PREFIX}-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${TOOLCHAIN_PREFIX}-g++-posix ${TOOLCHAIN_PREFIX}-g++ REQUIRED)
find_program(CMAKE_RC_COMPILER  NAMES ${TOOLCHAIN_PREFIX}-windres)
set(CMAKE_AR      ${TOOLCHAIN_PREFIX}-ar)
set(CMAKE_RANLIB  ${TOOLCHAIN_PREFIX}-ranlib)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Ship a single self-contained .exe — no MinGW runtime DLLs to redistribute.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")

# Run the produced .exe through Wine so ctest works when cross-compiling.
find_program(WINE_EXECUTABLE NAMES wine64 wine)
if(WINE_EXECUTABLE)
  set(CMAKE_CROSSCOMPILING_EMULATOR ${WINE_EXECUTABLE})
endif()
