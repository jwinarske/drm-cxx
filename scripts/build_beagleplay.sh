#!/usr/bin/env bash
# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# build_beagleplay.sh — cross-build drm-cxx (examples + tests + benchmarks) for
# the BeaglePlay (TI AM625: quad Cortex-A53, arm64; tidss display + PowerVR
# AXE-1-16M on the upstream powervr driver).
#
# Built inside debian:trixie with the arm64 multiarch -dev packages and the
# aarch64-linux-gnu cross toolchain, so the binaries link the libdrm / gbm /
# libdisplay-info / input / seat versions the board's Debian 13 ships.
# Vulkan is on: Mesa's PowerVR Vulkan driver renders, tidss scans out.
# Blend2D (text in the examples, drm::capture, drm::csd) has no Debian package;
# it is cross-built once, at the SHAs CI pins, into build-bp-deps/ and shipped
# next to libdrm-cxx.
#
# Usage:
#   scripts/build_beagleplay.sh                     build → bp-build/
#   scripts/build_beagleplay.sh <ssh-target> --deploy
#                                                   build, then copy the tree to
#                                                   <ssh-target>:~/drm-cxx-bp
# Override the container image with BP_BUILD_IMAGE (default debian:trixie).

set -euo pipefail

if [ -z "${IN_BP_CONTAINER:-}" ]; then
  REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  TARGET=""
  DEPLOY=0
  for a in "$@"; do
    case "$a" in
      --deploy) DEPLOY=1 ;;
      -*) echo "build_beagleplay: unknown option $a" >&2; exit 2 ;;
      *) TARGET="$a" ;;
    esac
  done
  if [ "$DEPLOY" = 1 ] && [ -z "$TARGET" ]; then
    echo "build_beagleplay: --deploy needs an <ssh-target>" >&2
    exit 2
  fi
  command -v podman >/dev/null 2>&1 || { echo "build_beagleplay: podman is required" >&2; exit 1; }
  IMAGE="${BP_BUILD_IMAGE:-docker.io/library/debian:trixie}"
  echo "build_beagleplay: cross-building for the BeaglePlay (arm64, $IMAGE)…"
  # amd64 explicitly: a cached arm64 debian image would run the "cross" build
  # emulated, where the cross binutils are missing.
  podman run --rm --platform linux/amd64 -e IN_BP_CONTAINER=1 -v "$REPO:/work:z" "$IMAGE" \
    bash /work/scripts/build_beagleplay.sh
  if [ "$DEPLOY" = 1 ]; then
    echo "build_beagleplay: deploying bp-build → $TARGET:~/drm-cxx-bp"
    tar -C "$REPO" --exclude='*.p' --exclude='*.o' --exclude='meson-*' --exclude='*.ninja' \
      -czf - bp-build examples/scene/signage_player/example.toml docs/logo.png \
      | ssh "$TARGET" 'rm -rf ~/drm-cxx-bp && mkdir ~/drm-cxx-bp && tar -C ~/drm-cxx-bp -xzf -'
  fi
  exit 0
fi

# ── Container side ────────────────────────────────────────────────────────────
export DEBIAN_FRONTEND=noninteractive
TRIPLE=aarch64-linux-gnu

echo "[bp] enabling arm64 multiarch + apt deps"
# APT::Sandbox::User=root: the _apt sandbox user can't read the SELinux-relabeled
# bind mount.
dpkg --add-architecture arm64
apt-get -o APT::Sandbox::User=root update -qq >/dev/null
apt-get -o APT::Sandbox::User=root install -y -qq --no-install-recommends \
  crossbuild-essential-arm64 meson ninja-build cmake pkg-config git ca-certificates \
  python3 hwdata glslang-tools \
  libdrm-dev:arm64 libgbm-dev:arm64 libegl-dev:arm64 libgles-dev:arm64 \
  libinput-dev:arm64 libudev-dev:arm64 libxkbcommon-dev:arm64 \
  libdisplay-info-dev:arm64 libfmt-dev:arm64 libseat-dev:arm64 \
  libgtest-dev:arm64 >/dev/null

echo "[bp] writing meson cross-file (cortex-a53)"
CROSS=/tmp/bp.cross
cat > "$CROSS" <<EOF
[binaries]
c = '${TRIPLE}-gcc'
cpp = '${TRIPLE}-g++'
# gcc-ar loads the LTO plugin; plain ar drops LTO objects from static helpers.
ar = '${TRIPLE}-gcc-ar'
ranlib = '${TRIPLE}-gcc-ranlib'
strip = '${TRIPLE}-strip'
pkg-config = 'pkg-config'
# Blend2D ships CMake config only (no .pc); meson needs cmake to read it.
cmake = 'cmake'

[properties]
pkg_config_libdir = '/usr/lib/${TRIPLE}/pkgconfig:/usr/share/pkgconfig'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'cortex-a53'
endian = 'little'

[built-in options]
c_args = ['-mcpu=cortex-a53']
cpp_args = ['-mcpu=cortex-a53']
EOF

# Blend2D at the SHAs CI pins (see .github/workflows/ci.yml), built once.
DEPS=/work/build-bp-deps
B2D=$DEPS/blend2d-install
if [ ! -f "$B2D/lib/libblend2d.so" ]; then
  echo "[bp] cross-building blend2d"
  fetch_pinned() {  # url sha dir
    rm -rf "$3"
    git init -q "$3"
    git -C "$3" remote add origin "$1"
    git -C "$3" fetch -q --depth 1 origin "$2"
    git -C "$3" checkout -q FETCH_HEAD
  }
  fetch_pinned https://github.com/asmjit/asmjit 0bd5787b54b575ed94bf32ac452153b34385c514 "$DEPS/asmjit-src"
  fetch_pinned https://github.com/blend2d/blend2d 6dbc2cefbc996379e07104e34519a440b49b15d7 "$DEPS/blend2d-src"
  cmake -S "$DEPS/blend2d-src" -B "$DEPS/blend2d-build" -G Ninja \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=${TRIPLE}-gcc -DCMAKE_CXX_COMPILER=${TRIPLE}-g++ \
    -DCMAKE_CXX_FLAGS=-mcpu=cortex-a53 \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$B2D" -DCMAKE_INSTALL_LIBDIR=lib \
    -DASMJIT_DIR="$DEPS/asmjit-src" -DBLEND2D_STATIC=FALSE -DBLEND2D_TEST=FALSE >/dev/null
  cmake --build "$DEPS/blend2d-build" >/dev/null
  cmake --install "$DEPS/blend2d-build" >/dev/null
fi

echo "[bp] meson setup"
cd /work
rm -rf bp-build
meson setup bp-build --cross-file "$CROSS" \
  -Dcmake_prefix_path="$B2D" \
  -Dexamples=true -Dtests=true -Dbenchmarks=true \
  -Dvulkan=true -Degl=enabled -Dblend2d=enabled \
  -Dgstreamer=disabled -Dstreams=disabled \
  -Dnvbufsurface=disabled -Dcamera=disabled -Dthorvg_janitor=disabled

echo "[bp] ninja"
ninja -C bp-build
# Shipped beside libdrm-cxx so LD_LIBRARY_PATH=bp-build/src finds it.
cp -a "$B2D"/lib/libblend2d.so* bp-build/src/
echo "[bp] DONE → bp-build/"
