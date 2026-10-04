#!/usr/bin/env bash
# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# build_imx8mp.sh — cross-build drm-cxx (library + every example + tests +
# benchmarks) for NXP i.MX8M Plus boards running the NXP i.MX Yocto BSP
# (quad Cortex-A53, aarch64, Vivante GC7000UL + LCDIFv3, kernel 6.6). Validated on
# the PANZER-PLUS Edge AIoT Computer.
#
# How: the build runs on the x86_64 host against a sysroot mirrored from the
# board itself, so the binaries link exactly the BSP's glibc, libstdc++, NXP GBM
# and Vivante EGL. The compiler is the Arm GNU Toolchain 13.2.rel1, matched to the
# BSP's GCC 13.2: a newer host cross-GCC rejects GCC 13's libstdc++ headers, and
# the BSP ships no libstdc++.a to link statically instead. The dependencies the
# BSP lacks (libdisplay-info, libseat, googletest, ThorVG, Blend2D, libyuv) are
# cross-built with the same toolchain into a staging tree, then copied into the
# sysroot (to build against) and, with --deploy, onto the board's /usr/local.
#
# --clang builds with LLVM instead: clang 18 against the same board sysroot, and
# libc++ in place of libstdc++. The BSP ships the libc++ 18 runtime (libc++abi
# merged in) but no headers, so libc++ 18.1.8's headers are installed into the
# sysroot link-only and the binaries run against the board's own libc++. The C++
# dependencies are rebuilt against libc++, so this variant keeps its own cache,
# sysroot and build dir, and deploys its dependencies to $PREFIX
# (/usr/local/drm-cxx-libcxx) — found through an rpath, never ld.so.conf, so they
# cannot shadow the GCC build's /usr/local libraries.
#
# GStreamer is different: the BSP ships the 1.24 runtime (with NXP's VPU / G2D
# plugins) but no headers. The matching release is cross-built into a separate
# *link-only* stage copied into the sysroot — never deployed — so video_player
# compiles against it and runs against the board's own GStreamer. GST_VERSION
# must match the board (`gst-inspect-1.0 --version`).
#
# Pitfalls this script handles (see docs/hardware.md § PANZER-PLUS Edge AIoT Computer):
#   - no -static-libgcc: a private unwinder in libdrm-cxx.so aborts every throw;
#   - pkg-config: drop the sysroot's own -I/usr/include / -L/usr/lib, which
#     pkgconf no longer treats as system dirs once sysroot-prefixed;
#   - ar = gcc-ar, so the LTO static helper archives keep their symbols;
#   - build rpaths pointing into the host sysroot are scrubbed from every ELF,
#     so nothing deployed references a path on the build machine.
#
# Usage:
#   scripts/build_imx8mp.sh <ssh-target> [--clang] [--deploy] [--resync-sysroot] [--clean]
#     e.g. scripts/build_imx8mp.sh root@imx8mp.local --deploy
#
#   <ssh-target>       board to mirror the sysroot from (first run, or with
#                      --resync-sysroot) and to deploy to (--deploy).
#   --clang            build with clang 18 + libc++ (see above) instead of GCC.
#   --deploy           copy the deps to the board's /usr/local (+ ld.so.conf.d
#                      entry + ldconfig; with --clang, to $PREFIX, no ld.so.conf)
#                      and the build tree + examples/ assets to $DEST on the board.
#   --resync-sysroot   re-mirror the board sysroot (after a BSP update).
#   --clean            wipe the drm-cxx build dir before configuring.
#
# On the board afterwards (the BSP's compositor service holds DRM master):
#   stop the compositor service and its socket unit, then from $DEST:
#     echo performance > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
#     build-imx8mp/examples/software_present /dev/dri/card1 600 --vsync
#     DRM_CXX_TEST_CARD=/dev/dri/card1 build-imx8mp/tests/test_layer_scene_vkms
#   Always pass /dev/dri/card1: card0 is the render-only Vivante GPU node.
#
# Env overrides:
#   IMX8MP_BUILD_CACHE  cache dir (default: $HOME/.cache/drm-cxx-imx8mp, -clang
#                       suffixed with --clang)
#   BUILD_DIR           drm-cxx build dir (default: <repo>/build-imx8mp, or
#                       <repo>/build-imx8mp-clang)
#   DEST                deploy dir on the board (default: /root/drm-cxx, or
#                       /root/drm-cxx-clang)
#   LLVM_BIN            clang 18 toolchain bin dir for --clang
#                       (default: /usr/lib64/llvm18/bin)
#   JOBS                parallel jobs (default: nproc)
#   KEYBOARD=1          also build the keyboard demo. Off by default: it bakes
#                       its font's absolute build-dir path into the binary, so
#                       it only runs from an identical path on the board.
#   LDI_REF SEATD_REF GTEST_REF THORVG_REF BLEND2D_REF ASMJIT_REF LIBYUV_REF
#                       dependency git refs
#   GST_VERSION         GStreamer release to link against (default: 1.24.0)
set -euo pipefail

TARGET=""
DEPLOY=0
RESYNC=0
CLEAN=0
CLANG=0
while [ $# -gt 0 ]; do
  case "$1" in
    --deploy)         DEPLOY=1; shift ;;
    --resync-sysroot) RESYNC=1; shift ;;
    --clean)          CLEAN=1; shift ;;
    --clang)          CLANG=1; shift ;;
    -h|--help)        sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//; /^set -euo/d'; exit 0 ;;
    -*)               echo "build_imx8mp: unknown option: $1" >&2; exit 1 ;;
    *)                TARGET="$1"; shift ;;
  esac
done

REPO=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
if [ "$CLANG" = 1 ]; then
  CACHE="${IMX8MP_BUILD_CACHE:-$HOME/.cache/drm-cxx-imx8mp-clang}"
  BUILD_DIR="${BUILD_DIR:-$REPO/build-imx8mp-clang}"
  DEST="${DEST:-/root/drm-cxx-clang}"
  PREFIX=/usr/local/drm-cxx-libcxx  # deps on the board, reached by rpath
else
  CACHE="${IMX8MP_BUILD_CACHE:-$HOME/.cache/drm-cxx-imx8mp}"
  BUILD_DIR="${BUILD_DIR:-$REPO/build-imx8mp}"
  DEST="${DEST:-/root/drm-cxx}"
  PREFIX=/usr/local
fi
JOBS="${JOBS:-$(nproc)}"

LDI_REF="${LDI_REF:-0.2.0}"
SEATD_REF="${SEATD_REF:-0.9.1}"
GTEST_REF="${GTEST_REF:-v1.15.2}"
THORVG_REF="${THORVG_REF:-v1.0.4}"
BLEND2D_REF="${BLEND2D_REF:-master}"
ASMJIT_REF="${ASMJIT_REF:-master}"
LIBYUV_REF="${LIBYUV_REF:-main}"
GST_VERSION="${GST_VERSION:-1.24.0}"
LLVM_BIN="${LLVM_BIN:-/usr/lib64/llvm18/bin}"
LIBCXX_VER="18.1.8"  # the BSP's libc++ is LLVM 18

TC_VER="13.2.rel1"
TC_NAME="arm-gnu-toolchain-${TC_VER}-x86_64-aarch64-none-linux-gnu"
TC_URL="https://developer.arm.com/-/media/Files/downloads/gnu/${TC_VER}/binrel/${TC_NAME}.tar.xz"
TC_SHA="12fcdf13a7430655229b20438a49e8566e26551ba08759922cdaf4695b0d4e23"
TRIPLE="aarch64-none-linux-gnu"

SYSROOT="$CACHE/sysroot"
STAGE="$CACHE/deps-stage"   # DESTDIR for the cross-built deps (prefix $PREFIX)
GST_STAGE="$CACHE/gst-link-stage"  # GStreamer headers/libs to link against (prefix /usr); never deployed
SRC="$CACHE/src"
TC_DIR="$CACHE/toolchain"
CROSS_FILE="$CACHE/imx8mp.cross"
CMAKE_TC="$CACHE/imx8mp.toolchain.cmake"
PKGCONF="$CACHE/pkg-config-imx8mp"
SSH="ssh -o ConnectTimeout=15"

log() { printf '\033[1;34m[imx8mp]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[imx8mp] error:\033[0m %s\n' "$*" >&2; exit 1; }

for tool in curl git meson ninja cmake pkg-config chrpath zstd sha256sum ssh scp; do
  command -v "$tool" >/dev/null 2>&1 || die "'$tool' is required on the host"
done
# libdisplay-info resolves pnp.ids through a *native* hwdata lookup at build time.
pkg-config --exists hwdata || die "host hwdata (pnp.ids) is required — install your distro's hwdata package"

mkdir -p "$CACHE" "$SRC"

# ── Toolchain ──────────────────────────────────────────────────────────────────
if [ "$CLANG" = 1 ]; then
  for t in clang clang++ ld.lld llvm-ar llvm-ranlib llvm-strip; do
    [ -x "$LLVM_BIN/$t" ] || die "--clang needs $LLVM_BIN/$t (clang 18; set LLVM_BIN)"
  done
  "$LLVM_BIN/clang" --version | grep -q 'clang version 18\.' \
    || die "--clang needs clang 18 to match the BSP's libc++ 18 (found: $("$LLVM_BIN/clang" --version | head -1))"
elif [ ! -x "$TC_DIR/bin/${TRIPLE}-g++" ]; then
  log "fetching Arm GNU Toolchain ${TC_VER} (matches the BSP's GCC 13.2)"
  tarball="$CACHE/${TC_NAME}.tar.xz"
  rm -f "$tarball.part"
  curl --fail --location --retry 3 --retry-all-errors --output "$tarball.part" "$TC_URL"
  echo "$TC_SHA  $tarball.part" | sha256sum -c --status - \
    || { rm -f "$tarball.part"; die "toolchain sha256 mismatch"; }
  mv "$tarball.part" "$tarball"
  rm -rf "$TC_DIR" && mkdir -p "$TC_DIR"
  tar -xJf "$tarball" -C "$TC_DIR" --strip-components=1
fi
TC="$TC_DIR/bin/${TRIPLE}"

# ── Sysroot (mirrored from the board) ──────────────────────────────────────────
if [ ! -f "$SYSROOT/usr/lib/libc.so" ] || [ "$RESYNC" = 1 ]; then
  [ -n "$TARGET" ] || die "no sysroot yet — pass the board's <ssh-target> to mirror it"
  log "mirroring sysroot from $TARGET (headers + libs; large BSP extras skipped)"
  rm -rf "$SYSROOT" && mkdir -p "$SYSROOT"
  # shellcheck disable=SC2016
  $SSH "$TARGET" 'cd / && tar -cf - \
      --exclude="usr/lib/python3*" --exclude=usr/lib/chromium --exclude="usr/lib/libQt6*" \
      --exclude=usr/lib/qt6 --exclude=usr/lib/modules --exclude=usr/lib/firmware \
      --exclude="usr/include/Qt*" --exclude="usr/lib/libNN*" --exclude=usr/lib/libtvm.so \
      --exclude="usr/lib/libVkLayer*" \
      usr/include usr/lib usr/share/pkgconfig 2>/dev/null' | tar -xf - -C "$SYSROOT"
  [ -f "$SYSROOT/usr/lib/libc.so" ] || die "sysroot mirror from $TARGET is incomplete"
  # Absolute symlinks would resolve into the host's own /usr — make them relative.
  find "$SYSROOT" -type l -lname '/*' -print0 | while IFS= read -r -d '' l; do
    ln -sfn "$(realpath -m --relative-to="$(dirname "$l")" "$SYSROOT$(readlink "$l")")" "$l"
  done
  ln -sfn usr/lib "$SYSROOT/lib"
  [ -e "$SYSROOT/usr/lib64" ] || ln -s lib "$SYSROOT/usr/lib64"
  ln -sfn usr/lib "$SYSROOT/lib64"
fi

# ── Cross file, CMake toolchain, pkg-config wrapper (generated per machine) ────
cat > "$PKGCONF" <<EOF
#!/bin/sh
# Generated by build_imx8mp.sh. pkg-config against the i.MX8MP sysroot, dropping
# the sysroot's own system include/lib dirs (pkgconf stops filtering them once
# sysroot-prefixed, and -isystem <sysroot>/usr/include breaks #include_next).
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig:$SYSROOT$PREFIX/lib/pkgconfig:$SYSROOT$PREFIX/lib64/pkgconfig:$SYSROOT$PREFIX/share/pkgconfig"
out=\$(pkg-config "\$@") || exit \$?
printf '%s\n' "\$out" | sed -E 's#(^| )-I$SYSROOT/usr/include( |\$)#\1#g; s#(^| )-L$SYSROOT/usr/lib( |\$)#\1#g'
EOF
chmod +x "$PKGCONF"

if [ "$CLANG" = 1 ]; then
# The BSP's GCC install (crt*.o, libgcc) — clang links through it, so the
# unwinder stays the board's libgcc_s.
GCC_DIR=$(dirname "$(find "$SYSROOT/usr/lib" -mindepth 3 -maxdepth 3 -name crtbegin.o -print -quit)")
[ -f "$GCC_DIR/crtbegin.o" ] || die "no GCC install (crtbegin.o) in the sysroot"
CLANG_COMMON="--target=aarch64-linux-gnu -mcpu=cortex-a53 --gcc-install-dir=$GCC_DIR"
# libc++ headers from the sysroot only (-nostdinc++): never the host's.
# -stdlib=libc++ is a link-time choice here (it picks -lc++); passed at compile
# time next to -nostdinc++ it is unused, which meson's checks treat as an error.
CLANG_CXX="-nostdinc++ -isystem $SYSROOT/usr/include/c++/v1"
CLANG_LINK="-fuse-ld=lld -stdlib=libc++ -Wl,-rpath,$PREFIX/lib -Wl,-rpath,$PREFIX/lib64"

cat > "$CMAKE_TC" <<EOF
# Generated by build_imx8mp.sh --clang.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER "$LLVM_BIN/clang")
set(CMAKE_CXX_COMPILER "$LLVM_BIN/clang++")
set(CMAKE_AR "$LLVM_BIN/llvm-ar")
set(CMAKE_RANLIB "$LLVM_BIN/llvm-ranlib")
set(CMAKE_C_FLAGS_INIT "$CLANG_COMMON")
set(CMAKE_CXX_FLAGS_INIT "$CLANG_COMMON $CLANG_CXX")
set(CMAKE_EXE_LINKER_FLAGS_INIT "$CLANG_LINK -L$SYSROOT$PREFIX/lib")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "$CLANG_LINK -L$SYSROOT$PREFIX/lib")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "$CLANG_LINK -L$SYSROOT$PREFIX/lib")
set(CMAKE_SYSROOT "$SYSROOT")
set(CMAKE_FIND_ROOT_PATH "$SYSROOT")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

cat > "$CROSS_FILE" <<EOF
# Generated by build_imx8mp.sh --clang.
[constants]
sysroot = '$SYSROOT'
common = ['--target=aarch64-linux-gnu', '-mcpu=cortex-a53', '--sysroot=' + sysroot,
          '--gcc-install-dir=$GCC_DIR', '-idirafter', sysroot + '$PREFIX/include']
cxx = ['-nostdinc++', '-isystem', sysroot + '/usr/include/c++/v1']
link_common = common + ['-fuse-ld=lld', '-L' + sysroot + '$PREFIX/lib',
                        '-Wl,-rpath,$PREFIX/lib', '-Wl,-rpath,$PREFIX/lib64',
                        '-Wl,-rpath-link,' + sysroot + '/usr/lib',
                        '-Wl,-rpath-link,' + sysroot + '$PREFIX/lib',
                        '-Wl,-rpath-link,' + sysroot + '$PREFIX/lib64']
[binaries]
c = '$LLVM_BIN/clang'
cpp = '$LLVM_BIN/clang++'
ar = '$LLVM_BIN/llvm-ar'
strip = '$LLVM_BIN/llvm-strip'
pkg-config = '$PKGCONF'
cmake = 'cmake'
[built-in options]
c_args = common
cpp_args = common + cxx
c_link_args = link_common
cpp_link_args = link_common + ['-stdlib=libc++']
cmake_prefix_path = [sysroot + '$PREFIX', sysroot + '/usr']
[properties]
sys_root = sysroot
cmake_toolchain_file = '$CMAKE_TC'
[cmake]
CMAKE_FIND_ROOT_PATH = '$SYSROOT'
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'cortex-a53'
endian = 'little'
EOF

# libc++ headers matching the BSP's runtime, installed into the sysroot only
# (never deployed): __config_site is generated, so they come from libc++'s own
# CMake. The board's libc++.so.1 has libc++abi merged in; -lc++ needs only a
# link-time libc++.so symlink.
if [ ! -f "$SYSROOT/usr/include/c++/v1/__config_site" ] || [ ! -f "$SYSROOT/usr/include/c++/v1/cxxabi.h" ]; then
  tarball="$SRC/llvm-project-$LIBCXX_VER.src.tar.xz"
  if [ ! -s "$tarball" ]; then
    log "fetching llvm-project $LIBCXX_VER (libc++ headers)"
    curl --fail --location --retry 3 --retry-all-errors --output "$tarball.part" \
      "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LIBCXX_VER/llvm-project-$LIBCXX_VER.src.tar.xz"
    mv "$tarball.part" "$tarball"
  fi
  if [ ! -d "$SRC/llvm-project-$LIBCXX_VER.src/runtimes" ]; then
    tar -xJf "$tarball" -C "$SRC" "llvm-project-$LIBCXX_VER.src/runtimes" \
      "llvm-project-$LIBCXX_VER.src/libcxx" "llvm-project-$LIBCXX_VER.src/libcxxabi" \
      "llvm-project-$LIBCXX_VER.src/cmake" "llvm-project-$LIBCXX_VER.src/llvm/cmake" \
      "llvm-project-$LIBCXX_VER.src/llvm/utils/llvm-lit" "llvm-project-$LIBCXX_VER.src/libc"
  fi
  log "installing libc++ $LIBCXX_VER headers into the sysroot"
  rm -rf "$CACHE/build/libcxx-headers"
  cmake -G Ninja -S "$SRC/llvm-project-$LIBCXX_VER.src/runtimes" -B "$CACHE/build/libcxx-headers" \
    -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TC" -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" -DLIBCXX_CXX_ABI=libcxxabi \
    -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
    -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
    -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_ENABLE_STATIC=OFF >/dev/null
  DESTDIR="$SYSROOT" ninja -C "$CACHE/build/libcxx-headers" install-cxx-headers \
    install-cxxabi-headers >/dev/null
  [ -f "$SYSROOT/usr/include/c++/v1/__config_site" ] && [ -f "$SYSROOT/usr/include/c++/v1/cxxabi.h" ] \
    || die "libc++ header install failed"
fi
ln -sfn libc++.so.1 "$SYSROOT/usr/lib/libc++.so"
else
cat > "$CMAKE_TC" <<EOF
# Generated by build_imx8mp.sh.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER "$TC-gcc")
set(CMAKE_CXX_COMPILER "$TC-g++")
set(CMAKE_AR "$TC-gcc-ar")
set(CMAKE_RANLIB "$TC-gcc-ranlib")
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a53")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a53")
set(CMAKE_SYSROOT "$SYSROOT")
set(CMAKE_FIND_ROOT_PATH "$SYSROOT")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

cat > "$CROSS_FILE" <<EOF
# Generated by build_imx8mp.sh.
[constants]
sysroot = '$SYSROOT'
# -idirafter / -L: the cross GCC does not search <sysroot>/usr/local, where the
# staged deps live; -idirafter keeps it behind libc so #include_next still works.
common = ['-mcpu=cortex-a53', '--sysroot=' + sysroot, '-idirafter', sysroot + '$PREFIX/include']
link_common = common + ['-L' + sysroot + '$PREFIX/lib',
                        '-Wl,-rpath-link,' + sysroot + '/usr/lib',
                        '-Wl,-rpath-link,' + sysroot + '$PREFIX/lib',
                        '-Wl,-rpath-link,' + sysroot + '$PREFIX/lib64']
[binaries]
c = '$TC-gcc'
cpp = '$TC-g++'
ar = '$TC-gcc-ar'
strip = '$TC-strip'
pkg-config = '$PKGCONF'
cmake = 'cmake'
[built-in options]
c_args = common
cpp_args = common
c_link_args = link_common
cpp_link_args = link_common
cmake_prefix_path = [sysroot + '$PREFIX', sysroot + '/usr']
[properties]
sys_root = sysroot
cmake_toolchain_file = '$CMAKE_TC'
[cmake]
CMAKE_FIND_ROOT_PATH = '$SYSROOT'
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'cortex-a53'
endian = 'little'
EOF
fi

# ── Dependencies the BSP lacks (cross-built into $STAGE, prefix /usr/local) ────
fetch() {  # fetch <url> <dir> <ref>
  [ -d "$2/.git" ] && return 0
  log "cloning $(basename "$2") @ $3"
  git -c advice.detachedHead=false clone -q --depth 1 --branch "$3" "$1" "$2"
}
meson_dep() {  # meson_dep <name> <srcdir> [meson options...]
  local name=$1 src=$2; shift 2
  [ -f "$STAGE/.done-$name" ] && return 0
  log "cross-building $name"
  rm -rf "$CACHE/build/$name"
  meson setup "$CACHE/build/$name" "$src" --cross-file "$CROSS_FILE" \
    --prefix="$PREFIX" --buildtype=release "$@" >/dev/null
  ninja -C "$CACHE/build/$name" -j "$JOBS" >/dev/null
  DESTDIR="$STAGE" meson install -C "$CACHE/build/$name" --no-rebuild >/dev/null
  touch "$STAGE/.done-$name"
}
cmake_dep() {  # cmake_dep <name> <srcdir> [cmake -D options...]
  local name=$1 src=$2; shift 2
  [ -f "$STAGE/.done-$name" ] && return 0
  log "cross-building $name"
  rm -rf "$CACHE/build/$name"
  cmake -S "$src" -B "$CACHE/build/$name" -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TC" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" "$@" >/dev/null
  cmake --build "$CACHE/build/$name" -j "$JOBS" >/dev/null
  DESTDIR="$STAGE" cmake --install "$CACHE/build/$name" >/dev/null
  touch "$STAGE/.done-$name"
}
mkdir -p "$STAGE"
# Each dep builds against the sysroot plus the deps staged before it.
sync_stage() {
  mkdir -p "$SYSROOT$PREFIX"
  cp -a "$STAGE$PREFIX/." "$SYSROOT$PREFIX/" 2>/dev/null || true
  [ -d "$GST_STAGE/usr" ] && cp -a "$GST_STAGE/usr/." "$SYSROOT/usr/"
  return 0
}

fetch https://gitlab.freedesktop.org/emersion/libdisplay-info.git "$SRC/libdisplay-info" "$LDI_REF"
meson_dep libdisplay-info "$SRC/libdisplay-info"
fetch https://git.sr.ht/~kennylevinsen/seatd "$SRC/seatd" "$SEATD_REF"
meson_dep libseat "$SRC/seatd" -Dlibseat-logind=systemd -Dlibseat-seatd=enabled \
  -Dlibseat-builtin=enabled -Dserver=disabled -Dexamples=disabled -Dman-pages=disabled
fetch https://github.com/google/googletest.git "$SRC/googletest" "$GTEST_REF"
cmake_dep googletest "$SRC/googletest" -DBUILD_SHARED_LIBS=ON
fetch https://github.com/thorvg/thorvg.git "$SRC/thorvg" "$THORVG_REF"
meson_dep thorvg "$SRC/thorvg" -Dengines=cpu -Dloaders=svg,lottie,ttf -Dsavers= \
  -Dbindings=capi -Dtools= -Dextra=lottie_exp -Dtests=false -Dstatic=false
fetch https://github.com/blend2d/blend2d.git "$SRC/blend2d" "$BLEND2D_REF"
fetch https://github.com/asmjit/asmjit.git "$SRC/blend2d/3rdparty/asmjit" "$ASMJIT_REF"
cmake_dep blend2d "$SRC/blend2d" -DBLEND2D_STATIC=FALSE -DBLEND2D_TEST=FALSE
# libyuv: cluster_sim's UVC rear-view (YUYV -> XRGB on the CPU) — the route a
# YUYV/NV12 camera takes onto the RGB-only LCDIFv3 planes.
fetch https://chromium.googlesource.com/libyuv/libyuv "$SRC/libyuv" "$LIBYUV_REF"
# No SME kernels: they need __arm_tpidr2_save, which the BSP's GCC 13 libgcc
# lacks (clang can compile them; GCC 13 cannot), and the A53 has no SME anyway.
cmake_dep libyuv "$SRC/libyuv" -DUNIT_TEST=OFF -DCAN_COMPILE_SME=OFF

# GStreamer core + plugins-base (libs only) for video_player / GstAppsinkSource.
gst_dep() {  # gst_dep <module> [meson options...]
  local mod=$1; shift
  [ -f "$GST_STAGE/.done-$mod" ] && return 0
  local tarball="$SRC/$mod-$GST_VERSION.tar.xz"
  local url="https://gstreamer.freedesktop.org/src/$mod/$mod-$GST_VERSION.tar.xz"
  if [ ! -s "$tarball" ]; then
    log "fetching $mod $GST_VERSION"
    curl --fail --location --retry 3 --retry-all-errors --output "$tarball.part" "$url"
    sum=$(curl --fail --location --retry 3 "$url.sha256sum" | cut -d' ' -f1)
    echo "$sum  $tarball.part" | sha256sum -c --status - \
      || { rm -f "$tarball.part"; die "$mod tarball sha256 mismatch"; }
    mv "$tarball.part" "$tarball"
  fi
  rm -rf "$SRC/$mod-$GST_VERSION" && tar -xJf "$tarball" -C "$SRC"
  log "cross-building $mod $GST_VERSION (link-only)"
  rm -rf "$CACHE/build/$mod"
  meson setup "$CACHE/build/$mod" "$SRC/$mod-$GST_VERSION" --cross-file "$CROSS_FILE" \
    --prefix=/usr --buildtype=release -Dexamples=disabled -Dtests=disabled \
    -Dtools=disabled -Ddoc=disabled -Dnls=disabled -Dintrospection=disabled "$@" >/dev/null
  ninja -C "$CACHE/build/$mod" -j "$JOBS" >/dev/null
  DESTDIR="$GST_STAGE" meson install -C "$CACHE/build/$mod" --no-rebuild >/dev/null
  touch "$GST_STAGE/.done-$mod"
  sync_stage
}
mkdir -p "$GST_STAGE"
gst_dep gstreamer -Dbenchmarks=disabled -Dlibunwind=disabled -Dlibdw=disabled \
  -Dbash-completion=disabled -Dptp-helper=disabled -Dcheck=disabled
gst_dep gst-plugins-base -Dauto_features=disabled
sync_stage

# ── drm-cxx ────────────────────────────────────────────────────────────────────
[ "$CLEAN" = 1 ] && rm -rf "$BUILD_DIR"
KEYBOARD_OPT=-Dkeyboard=disabled
if [ "${KEYBOARD:-0}" = 1 ]; then
  KEYBOARD_OPT=-Dkeyboard=enabled
  log "WARNING: KEYBOARD=1 bakes the font's build-dir path into the keyboard demo"
fi
deps_stamp=$(cd "$STAGE" && ls .done-* 2>/dev/null | tr '\n' ' ')$(cd "$GST_STAGE" && ls .done-* 2>/dev/null | tr '\n' ' ')
if [ -f "$BUILD_DIR/build.ninja" ] && [ "$(cat "$BUILD_DIR/.imx8mp-deps" 2>/dev/null)" != "$deps_stamp" ]; then
  log "dependency set changed — reconfiguring from scratch"
  rm -rf "$BUILD_DIR"
fi
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  log "configuring drm-cxx → $BUILD_DIR"
  meson setup "$BUILD_DIR" "$REPO" --cross-file "$CROSS_FILE" \
    -Dcpp_std=c++23 -Dbuildtype=debugoptimized \
    -Dexamples=true -Dtests=true -Dbenchmarks=true "$KEYBOARD_OPT" \
    -Degl=enabled -Dvulkan=true -Dblend2d=enabled -Dsession=enabled -Dcursor=enabled \
    -Dthorvg_janitor=enabled -Dcamera=auto -Dgstreamer=enabled
  printf '%s' "$deps_stamp" > "$BUILD_DIR/.imx8mp-deps"
fi
log "building drm-cxx"
ninja -C "$BUILD_DIR" -j "$JOBS"

# Meson gives build-tree ELFs an rpath to every non-system dependency dir — here
# the host sysroot's /usr/local. Keep the $ORIGIN entries, drop the host ones.
log "scrubbing host-sysroot rpaths"
while IFS= read -r -d '' elf; do
  head -c 4 "$elf" 2>/dev/null | grep -q $'\x7fELF' || continue
  cur=$(chrpath -l "$elf" 2>/dev/null | sed -n 's/.*R\(UN\)\{0,1\}PATH=//p') || true
  [ -n "$cur" ] || continue
  case "$cur" in *"$SYSROOT"*|*"$CACHE"*) ;; *) continue ;; esac
  keep=$(printf '%s' "$cur" | tr ':' '\n' | { grep -v -e "$SYSROOT" -e "$CACHE" || true; } | paste -sd: -)
  if [ -n "$keep" ]; then chrpath -r "$keep" "$elf" >/dev/null; else chrpath -d "$elf" >/dev/null; fi
done < <(find "$BUILD_DIR" -type f \( -perm -u+x -o -name '*.so*' \) -print0)
leaks=$(find "$BUILD_DIR" -type f \( -perm -u+x -o -name '*.so*' \) -exec chrpath -l {} \; 2>/dev/null \
          | grep -e "$SYSROOT" -e "$CACHE" || true)
[ -z "$leaks" ] || die "rpaths still reference the build machine:"$'\n'"$leaks"

# ── Deploy ─────────────────────────────────────────────────────────────────────
if [ "$DEPLOY" = 1 ]; then
  [ -n "$TARGET" ] || die "--deploy needs the board's <ssh-target>"
  log "deploying deps → $TARGET:$PREFIX"
  if [ "$CLANG" = 1 ]; then
    # Reached by the binaries' rpath; no ld.so.conf entry, so these libc++
    # builds of the deps never shadow the GCC build's /usr/local ones.
    tar -C "$STAGE" -cf - "${PREFIX#/}" | $SSH "$TARGET" 'tar -C / -xf -'
  else
    tar -C "$STAGE" -cf - usr/local | $SSH "$TARGET" \
      'tar -C / -xf - && mkdir -p /etc/ld.so.conf.d \
       && printf "/usr/local/lib\n/usr/local/lib64\n" > /etc/ld.so.conf.d/usr-local.conf && ldconfig'
  fi
  log "deploying build tree + examples/ assets → $TARGET:$DEST"
  $SSH "$TARGET" "mkdir -p '$DEST'"
  tar -C "$(dirname "$BUILD_DIR")" -cf - \
      --exclude='*.o' --exclude='*.p' --exclude='meson-*' --exclude='*.ninja*' --exclude='*.a' \
      "$(basename "$BUILD_DIR")" \
    | zstd -T0 -q | $SSH "$TARGET" "cd '$DEST' && zstd -dc | tar -xf -"
  tar -C "$REPO" -cf - examples | zstd -T0 -q | $SSH "$TARGET" "cd '$DEST' && zstd -dc | tar -xf -"
fi

log "DONE — $(find "$BUILD_DIR/examples" "$BUILD_DIR/benchmarks" -maxdepth 3 -type f -perm -u+x ! -name '*.so*' | wc -l) example/benchmark binaries in $BUILD_DIR"
[ "$DEPLOY" = 1 ] && log "on the board: stop the compositor service, then run from $DEST with /dev/dri/card1"
exit 0
