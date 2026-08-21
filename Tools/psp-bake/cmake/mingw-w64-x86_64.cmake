# CMake toolchain for cross-compiling psp-bake to 64-bit Windows with MinGW-w64.
#
# It expects the compilers and the statically built zlib/libxmp that
# Dockerfile.mingw installs, but it works with any host that provides them.
# Override the dependency prefix with -DPSP_BAKE_MINGW_PREFIX=/path when the
# libraries live somewhere else.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(PSP_BAKE_MINGW_TRIPLE "x86_64-w64-mingw32" CACHE STRING "MinGW-w64 target triple")
set(PSP_BAKE_MINGW_PREFIX "/opt/mingw64" CACHE PATH "Prefix containing the cross-built zlib and libxmp")

find_program(CMAKE_C_COMPILER NAMES ${PSP_BAKE_MINGW_TRIPLE}-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${PSP_BAKE_MINGW_TRIPLE}-g++ REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${PSP_BAKE_MINGW_TRIPLE}-windres)

# Look for headers and libraries only in the cross prefix and the toolchain's
# own sysroot, but keep using host programs (cmake, make, ...).
set(CMAKE_FIND_ROOT_PATH "${PSP_BAKE_MINGW_PREFIX}" "/usr/${PSP_BAKE_MINGW_TRIPLE}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# A fully static executable: no libgcc/libstdc++/winpthread DLLs to ship, which
# is what the packaging step in CMakeLists.txt assumes when cross-compiling.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
