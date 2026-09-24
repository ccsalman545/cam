# Toolchain for cross compiling camstream-libpeer for a Pi 4 (aarch64) using Zig.
# Usage:
#   pip install ziglang
#   make camstream-libpeer CC="zig cc -target aarch64-linux-gnu" CMAKE=cmake \
#        LIBPEER_CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=cmake/zig-aarch64.cmake"
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER "zig;cc;-target;aarch64-linux-gnu" CACHE STRING "" FORCE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
