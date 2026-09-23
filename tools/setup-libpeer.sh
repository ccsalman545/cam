#!/usr/bin/env bash
# Build libpeer without vendoring its source into camstream.
#
# Usage:
#   tools/setup-libpeer.sh
#   LIBPEER_REF=<tag-or-commit> tools/setup-libpeer.sh
#   TARGET=aarch64-linux-gnu tools/setup-libpeer.sh
#
# The source tree and build products live below build/ and are ignored by
# git. libpeer builds its own mbedTLS, libsrtp, usrsctp and cJSON External
# Projects, so this is intentionally separate from camstream's OpenSSL /
# libsrtp2 stack.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC=${LIBPEER_SRC:-"$ROOT/build/libpeer-src"}
OUT=${LIBPEER_BUILD:-"$ROOT/build/libpeer"}
REF=${LIBPEER_REF:-5b849de378545c31d34759a145413846953e1366}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')}

if ! command -v git >/dev/null 2>&1; then
  printf '%s\n' 'error: git is required' >&2
  exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
  printf '%s\n' 'error: cmake 3.16 or newer is required' >&2
  exit 1
fi

mkdir -p "$(dirname "$SRC")"
if [ ! -d "$SRC/.git" ]; then
  git clone --recursive https://github.com/sepfy/libpeer.git "$SRC"
else
  git -C "$SRC" fetch --tags --force origin
  git -C "$SRC" submodule update --init --recursive
fi

git -C "$SRC" checkout --detach "$REF"
git -C "$SRC" submodule update --init --recursive

# Apply WebRTC browser interoperability and platform patches
python3 "$ROOT/tools/patch_libpeer.py" "$SRC"

# Pass a cross compiler through TARGET or use the caller's CC/CXX. A full
# toolchain file is preferred for sysrooted builds.
CMAKE_ARGS=(-S "$SRC" -B "$OUT" -DCMAKE_BUILD_TYPE=Release
  -DBUILD_SHARED_LIBS=OFF -DENABLE_TESTS=OFF -DCMAKE_INSTALL_PREFIX="$OUT/dist")
if [ -n "${TARGET:-}" ]; then
  CMAKE_ARGS+=("-DCMAKE_C_COMPILER=${CC:-${TARGET}-gcc}"
               "-DCMAKE_CXX_COMPILER=${CXX:-${TARGET}-g++}")
fi
if [ -n "${CMAKE_TOOLCHAIN_FILE:-}" ]; then
  CMAKE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$CMAKE_TOOLCHAIN_FILE")
fi

cmake "${CMAKE_ARGS[@]}"
cmake --build "$OUT" --target peer --parallel "$JOBS"
cmake --install "$OUT"
printf '\nlibpeer built successfully\n  source: %s\n  build:  %s\n  ref:    %s\n' "$SRC" "$OUT" "$REF"
printf '%s\n' 'Note: this builds libpeer independently; camstream still uses its audited native RTP/DTLS stack.'
