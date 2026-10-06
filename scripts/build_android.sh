#!/usr/bin/env bash
# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# build_android.sh — cross-build libdrm-cxx for Android with the NDK.
#
# Library only. drm::input, drm::session and HotplugMonitor need libinput,
# libudev and xkbcommon, which Android lacks (-Dinput=disabled), and the
# examples and benchmarks need those in turn. Dependencies are cross-built
# from pinned sources into build-android-<abi>/deps:
#
#   libdrm           2.4.124
#   libdisplay-info  0.2.0
#   minigbm          Android's GBM implementation, generic dumb-buffer backend;
#                    the three libcutils headers it expects from the platform
#                    are shimmed onto NDK APIs. Installed as libgbm.so.1 (its
#                    soname).
#
# Usage:
#   scripts/build_android.sh [--abi arm64-v8a|x86_64] [--api N] [--ndk DIR]
#
# NDK: --ndk, else $ANDROID_NDK_HOME, else $ANDROID_NDK_LATEST_HOME, else the
# newest under ~/Android/Sdk/ndk. Needs meson, ninja, make, git, pkg-config.
#
# Running it needs KMS access (root, or a userdebug build with SurfaceFlinger
# stopped). Ship libdrm-cxx.so with libdrm.so, libdisplay-info.so, libgbm.so.1
# and the NDK's libc++_shared.so.

set -euo pipefail

ABI=arm64-v8a
API=29
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_LATEST_HOME:-}}"
while [ $# -gt 0 ]; do
  case "$1" in
    --abi) ABI="$2"; shift 2 ;;
    --api) API="$2"; shift 2 ;;
    --ndk) NDK="$2"; shift 2 ;;
    *) echo "build_android: unknown argument $1" >&2; exit 2 ;;
  esac
done
if [ -z "$NDK" ]; then
  NDK="$(ls -d "$HOME"/Android/Sdk/ndk/* 2>/dev/null | sort -V | tail -1 || true)"
fi
if [ -z "$NDK" ] || [ ! -d "$NDK/toolchains/llvm/prebuilt/linux-x86_64" ]; then
  echo "build_android: no NDK found; pass --ndk DIR or set ANDROID_NDK_HOME" >&2
  exit 1
fi
case "$ABI" in
  arm64-v8a) TRIPLE=aarch64-linux-android; CPU_FAMILY=aarch64; CPU=armv8-a ;;
  x86_64)    TRIPLE=x86_64-linux-android;  CPU_FAMILY=x86_64;  CPU=x86_64 ;;
  *) echo "build_android: unsupported ABI $ABI (arm64-v8a, x86_64)" >&2; exit 2 ;;
esac
for tool in meson ninja make git pkg-config; do
  command -v "$tool" >/dev/null 2>&1 || { echo "build_android: $tool is required" >&2; exit 1; }
done

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$REPO/build-android-$ABI"
DEPS="$OUT/deps"
PREFIX="$DEPS/prefix"
BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="$BIN/$TRIPLE$API-clang"
mkdir -p "$DEPS" "$PREFIX/lib/pkgconfig" "$PREFIX/include"
echo "build_android: NDK $(basename "$NDK"), $ABI, API $API → $OUT"

# NDK clang finds its own sysroot; no sys_root here, or pkg-config would
# prefix it onto the deps' include paths.
CROSS="$OUT/android.cross"
cat > "$CROSS" <<EOF
[binaries]
c = '$CC'
cpp = '$BIN/$TRIPLE$API-clang++'
ar = '$BIN/llvm-ar'
strip = '$BIN/llvm-strip'
ranlib = '$BIN/llvm-ranlib'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = '$PREFIX/lib/pkgconfig'

[host_machine]
system = 'android'
cpu_family = '$CPU_FAMILY'
cpu = '$CPU'
endian = 'little'
EOF

fetch_pinned() {  # url sha dir
  if [ "$(git -C "$3" rev-parse HEAD 2>/dev/null || true)" = "$2" ]; then
    return
  fi
  rm -rf "$3"
  git init -q "$3"
  git -C "$3" remote add origin "$1"
  git -C "$3" fetch -q --depth 1 origin "$2"
  git -C "$3" checkout -q FETCH_HEAD
}

meson_dep() {  # name src [options...]
  local name="$1" src="$2"
  shift 2
  if [ -f "$DEPS/.$name.done" ]; then
    return
  fi
  echo "build_android: $name"
  rm -rf "$src/build-$ABI"
  meson setup "$src/build-$ABI" "$src" --cross-file "$CROSS" --prefix "$PREFIX" --libdir lib \
    --buildtype release "$@" >/dev/null
  ninja -C "$src/build-$ABI" install >/dev/null
  touch "$DEPS/.$name.done"
}

fetch_pinned https://gitlab.freedesktop.org/mesa/drm.git \
  38ec7dbd4df3141441afafe5ac62dfc9df36a77e "$DEPS/libdrm"  # libdrm-2.4.124
meson_dep libdrm "$DEPS/libdrm" \
  -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled \
  -Dvmwgfx=disabled -Domap=disabled -Dexynos=disabled -Dfreedreno=disabled \
  -Dtegra=disabled -Dvc4=disabled -Detnaviv=disabled -Dcairo-tests=disabled \
  -Dvalgrind=disabled -Dman-pages=disabled -Dtests=false -Dinstall-test-programs=false

fetch_pinned https://gitlab.freedesktop.org/emersion/libdisplay-info.git \
  66b802d05b374cd8f388dc6ad1e7ae4f08cb3300 "$DEPS/libdisplay-info"  # 0.2.0
meson_dep libdisplay-info "$DEPS/libdisplay-info"

# minigbm: AOSP builds it against libcutils; the NDK has the same services
# under other names.
fetch_pinned https://chromium.googlesource.com/chromiumos/platform/minigbm \
  a2d42f09d696b04e6ded5ad38596d8554ffd8988 "$DEPS/minigbm"
if [ ! -f "$DEPS/.minigbm.done" ]; then
  echo "build_android: minigbm"
  SHIM="$DEPS/cutils-shim"
  mkdir -p "$SHIM/cutils"
  printf '#pragma once\n#include <android/log.h>\n' > "$SHIM/cutils/log.h"
  printf '#pragma once\n#define AID_SYSTEM 1000\n' > "$SHIM/cutils/android_filesystem_config.h"
  cat > "$SHIM/cutils/properties.h" <<'EOF'
#pragma once
#include <sys/system_properties.h>
#define PROPERTY_VALUE_MAX PROP_VALUE_MAX
static inline int property_get(const char* key, char* value, const char* default_value) {
  int len = __system_property_get(key, value);
  if (len > 0) {
    return len;
  }
  int i = 0;
  for (; default_value != 0 && default_value[i] != '\0' && i < PROPERTY_VALUE_MAX - 1; ++i) {
    value[i] = default_value[i];
  }
  value[i] = '\0';
  return i;
}
EOF
  rm -rf "$DEPS/minigbm-out"
  # CPPFLAGS / LDLIBS through the environment: the Makefile appends to them.
  PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" CPPFLAGS="-I$SHIM" LDLIBS="-llog" \
    make -C "$DEPS/minigbm" -j"$(nproc)" CC="$CC" AR="$BIN/llvm-ar" \
    OUT="$DEPS/minigbm-out/" >/dev/null
  cp "$DEPS/minigbm/gbm.h" "$PREFIX/include/"
  cp "$DEPS/minigbm-out/libminigbm.so.1.0.0" "$PREFIX/lib/libgbm.so.1"
  ln -sf libgbm.so.1 "$PREFIX/lib/libgbm.so"
  cat > "$PREFIX/lib/pkgconfig/gbm.pc" <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: gbm
Description: minigbm (GBM API on libdrm)
Version: 25.0.0
Requires.private: libdrm
Libs: -L\${libdir} -lgbm
Cflags: -I\${includedir}
EOF
  touch "$DEPS/.minigbm.done"
fi

echo "build_android: drm-cxx"
rm -rf "$OUT/drm-cxx"
meson setup "$OUT/drm-cxx" "$REPO" --cross-file "$CROSS" --buildtype release \
  -Dinput=disabled -Dsession=disabled -Dexamples=false -Dtests=false -Dbenchmarks=false \
  -Dvulkan=true -Degl=enabled -Dstreams=disabled -Dblend2d=disabled -Dgstreamer=disabled \
  -Dcamera=disabled -Dthorvg_janitor=disabled -Dnvbufsurface=disabled >/dev/null
ninja -C "$OUT/drm-cxx"
echo "build_android: DONE → $OUT/drm-cxx/src/libdrm-cxx.so (deps in $PREFIX/lib)"
