#!/usr/bin/env bash
# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# build_imx8mp.sh — cross-build drm-cxx (library + every example + tests +
# benchmarks) for NXP i.MX8M Plus boards running the NXP i.MX Yocto BSP
# (quad Cortex-A53, aarch64, Vivante GC7000UL + LCDIFv3, kernel 6.6).
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
# GStreamer is different: the BSP ships the 1.24 runtime (with NXP's VPU / G2D
# plugins) but no headers. The matching release is cross-built into a separate
# *link-only* stage copied into the sysroot — never deployed — so video_player
# compiles against it and runs against the board's own GStreamer. GST_VERSION
# must match the board (`gst-inspect-1.0 --version`).
#
# Pitfalls this script handles (see docs/hardware.md § i.MX8M Plus):
#   - no -static-libgcc: a private unwinder in libdrm-cxx.so aborts every throw;
#   - pkg-config: drop the sysroot's own -I/usr/include / -L/usr/lib, which
#     pkgconf no longer treats as system dirs once sysroot-prefixed;
#   - ar = gcc-ar, so the LTO static helper archives keep their symbols;
#   - build rpaths pointing into the host sysroot are scrubbed from every ELF,
#     so nothing deployed references a path on the build machine.
#
# Usage:
#   scripts/build_imx8mp.sh <ssh-target> [--deploy] [--resync-sysroot] [--clean]
#     e.g. scripts/build_imx8mp.sh root@imx8mp.local --deploy
#
#   <ssh-target>       board to mirror the sysroot from (first run, or with
#                      --resync-sysroot) and to deploy to (--deploy).
#   --deploy           copy the deps to the board's /usr/local (+ ld.so.conf.d
#                      entry + ldconfig) and the build tree + examples/ assets
#                      to $DEST on the board.
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
#   IMX8MP_BUILD_CACHE  cache dir (default: $HOME/.cache/drm-cxx-imx8mp)
#   BUILD_DIR           drm-cxx build dir (default: <repo>/build-imx8mp)
#   DEST                deploy dir on the board (default: /root/drm-cxx)
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
while [ $# -gt 0 ]; do
  case "$1" in
    --deploy)         DEPLOY=1; shift ;;
    --resync-sysroot) RESYNC=1; shift ;;
    --clean)          CLEAN=1; shift ;;
    -h|--help)        sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//; /^set -euo/d'; exit 0 ;;
    -*)               echo "build_imx8mp: unknown option: $1" >&2; exit 1 ;;
    *)                TARGET="$1"; shift ;;
  esac
done

REPO=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
CACHE="${IMX8MP_BUILD_CACHE:-$HOME/.cache/drm-cxx-imx8mp}"
BUILD_DIR="${BUILD_DIR:-$REPO/build-imx8mp}"
DEST="${DEST:-/root/drm-cxx}"
JOBS="${JOBS:-$(nproc)}"

LDI_REF="${LDI_REF:-0.2.0}"
SEATD_REF="${SEATD_REF:-0.9.1}"
GTEST_REF="${GTEST_REF:-v1.15.2}"
THORVG_REF="${THORVG_REF:-v1.0.4}"
BLEND2D_REF="${BLEND2D_REF:-master}"
ASMJIT_REF="${ASMJIT_REF:-master}"
LIBYUV_REF="${LIBYUV_REF:-main}"
GST_VERSION="${GST_VERSION:-1.24.0}"

TC_VER="13.2.rel1"
TC_NAME="arm-gnu-toolchain-${TC_VER}-x86_64-aarch64-none-linux-gnu"
TC_URL="https://developer.arm.com/-/media/Files/downloads/gnu/${TC_VER}/binrel/${TC_NAME}.tar.xz"
TC_SHA="12fcdf13a7430655229b20438a49e8566e26551ba08759922cdaf4695b0d4e23"
TRIPLE="aarch64-none-linux-gnu"

SYSROOT="$CACHE/sysroot"
STAGE="$CACHE/deps-stage"   # DESTDIR for the cross-built deps (prefix /usr/local)
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
if [ ! -x "$TC_DIR/bin/${TRIPLE}-g++" ]; then
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
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig:$SYSROOT/usr/local/lib/pkgconfig:$SYSROOT/usr/local/lib64/pkgconfig"
out=\$(pkg-config "\$@") || exit \$?
printf '%s\n' "\$out" | sed -E 's#(^| )-I$SYSROOT/usr/include( |\$)#\1#g; s#(^| )-L$SYSROOT/usr/lib( |\$)#\1#g'
EOF
chmod +x "$PKGCONF"

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
common = ['-mcpu=cortex-a53', '--sysroot=' + sysroot, '-idirafter', sysroot + '/usr/local/include']
link_common = common + ['-L' + sysroot + '/usr/local/lib',
                        '-Wl,-rpath-link,' + sysroot + '/usr/lib',
                        '-Wl,-rpath-link,' + sysroot + '/usr/local/lib',
                        '-Wl,-rpath-link,' + sysroot + '/usr/local/lib64']
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
cmake_prefix_path = [sysroot + '/usr/local', sysroot + '/usr']
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
    --prefix=/usr/local --buildtype=release "$@" >/dev/null
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
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local "$@" >/dev/null
  cmake --build "$CACHE/build/$name" -j "$JOBS" >/dev/null
  DESTDIR="$STAGE" cmake --install "$CACHE/build/$name" >/dev/null
  touch "$STAGE/.done-$name"
}
mkdir -p "$STAGE"
# Each dep builds against the sysroot plus the deps staged before it.
sync_stage() {
  mkdir -p "$SYSROOT/usr/local"
  cp -a "$STAGE/usr/local/." "$SYSROOT/usr/local/" 2>/dev/null || true
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
cmake_dep libyuv "$SRC/libyuv" -DUNIT_TEST=OFF

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
  log "deploying deps → $TARGET:/usr/local"
  tar -C "$STAGE" -cf - usr/local | $SSH "$TARGET" \
    'tar -C / -xf - && mkdir -p /etc/ld.so.conf.d \
     && printf "/usr/local/lib\n/usr/local/lib64\n" > /etc/ld.so.conf.d/usr-local.conf && ldconfig'
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
