#!/usr/bin/env bash
# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# build_beaglebone_black.sh — cross-build drm-cxx (examples + tests) for the
# BeagleBone Black (TI AM335x, 32-bit ARMv7 Cortex-A8, armhf).
#
# Why a container: the BeagleBone Black runs Debian 13 (trixie) armhf. We build
# inside debian:trixie with the armhf multiarch -dev packages + the
# arm-linux-gnueabihf cross-toolchain, so the binaries link exactly the libdrm /
# fmt / libdisplay-info / input / udev versions the board ships and run there
# unmodified.
#
# Scope: this SoC has NO GPU acceleration — the PowerVR SGX530 has no open
# 3D/Vulkan, only the tilcdc display controller (KMS dumb-buffer scanout). So
# Vulkan is off and the only examples that run usefully on-target are the
# software / dumb-buffer ones (software_present, ring_present, atomic_modeset,
# signage_player, …). The GL examples still build but need llvmpipe (slow) and
# are not the point here.
#
# Usage:
#   Build (needs podman; run from anywhere in the tree):
#     scripts/build_beaglebone_black.sh
#       Output (in the repo): bbb-build/src/libdrm-cxx.so + bbb-build/examples/* +
#       bbb-build/tests/*.
#
#   Flash the OS image to an SD card AND stage the built binaries to the board's
#   $HOME (needs sudo; ERASES the card; run the build first):
#     scripts/build_beaglebone_black.sh --image-sd /dev/sdX [--dut-user debian]
#       Flashes the pinned BeagleBone Black Debian 13.5 image, then copies the
#       lib + every example + every test binary to /home/<dut-user>/drm-cxx on
#       the card's rootfs. Then on the board, from a free VT (DRM master):
#         cd ~/drm-cxx && LD_LIBRARY_PATH=. ./software_present /dev/dri/card0 120 --vsync
#       Options: --no-verify-flash, --image-url <url>, --image-sha256 <hex>.
#
# Override the container image with BBB_BUILD_IMAGE (default debian:trixie).
set -euo pipefail

TRIPLE=arm-linux-gnueabihf

# ── Host side ─────────────────────────────────────────────────────────────────
if [ -z "${IN_BBB_CONTAINER:-}" ]; then
  REPO=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
  BUILD_DIR="$REPO/bbb-build"

  # Pinned BeagleBone Black Debian 13.5 (2026-05-19) base/console armhf image.
  IMG_BASE="https://files.beagle.cc/file/beagleboard-public-2021/images"
  IMG_URL="$IMG_BASE/am335x-debian-13.5-base-v6.18-armhf-2026-05-19-4gb.img.xz"
  IMG_SHA="e8104145aa9f8e25fa2818f8edad0e4571b990bfca701aa3041dbe6e552898b7"
  CACHE="${BBB_BUILD_CACHE:-$HOME/.cache/drm-cxx-bbb}"

  IMAGE_SD_DEV=""
  DUT_USER="debian"   # BeagleBoard Debian default login
  VERIFY_FLASH=1
  while [ $# -gt 0 ]; do
    case "$1" in
      --image-sd)        IMAGE_SD_DEV="$2"; shift 2 ;;
      --dut-user)        DUT_USER="$2"; shift 2 ;;
      --no-verify-flash) VERIFY_FLASH=0; shift ;;
      --image-url)       IMG_URL="$2"; shift 2 ;;
      --image-sha256)    IMG_SHA="$2"; shift 2 ;;
      *) echo "build_beaglebone_black: unknown option: $1" >&2; exit 1 ;;
    esac
  done

  # ── Flash the OS image to an SD card + stage the cross-built binaries ────────
  if [ -n "$IMAGE_SD_DEV" ]; then
    DEV="$IMAGE_SD_DEV"
    [ -b "$DEV" ] || { echo "error: $DEV is not a block device" >&2; exit 1; }
    [ "$(lsblk -dno RM "$DEV" 2>/dev/null)" = 1 ] \
      || { echo "error: $DEV is not removable — refusing to erase it" >&2; exit 1; }
    [ "$(lsblk -dno TYPE "$DEV" 2>/dev/null)" = disk ] \
      || { echo "error: $DEV is not a whole disk (pass the device, not a partition)" >&2; exit 1; }
    [ -f "$BUILD_DIR/src/libdrm-cxx.so" ] \
      || { echo "error: no bbb-build — run the build (no args) first" >&2; exit 1; }
    command -v sudo >/dev/null 2>&1 || { echo "error: sudo is required to flash" >&2; exit 1; }

    mkdir -p "$CACHE"
    IMG_XZ="$CACHE/$(basename "$IMG_URL")"; IMG="${IMG_XZ%.xz}"
    if [ ! -s "$IMG_XZ" ]; then
      echo "[bbb] fetching $(basename "$IMG_XZ")"
      curl --fail --location --retry 3 --continue-at - --output "$IMG_XZ.part" "$IMG_URL"
      mv "$IMG_XZ.part" "$IMG_XZ"
    fi
    if [ -n "$IMG_SHA" ]; then
      echo "[bbb] verifying image sha256"
      echo "$IMG_SHA  $IMG_XZ" | sha256sum -c - \
        || { echo "error: image sha256 mismatch" >&2; exit 1; }
    fi
    [ -s "$IMG" ] || { echo "[bbb] decompressing image"; xz -dkT0 "$IMG_XZ"; }

    echo "[bbb] flashing $(basename "$IMG") → $DEV  (ERASES $DEV; needs sudo)"
    for p in $(lsblk -lno NAME "$DEV" | tail -n +2); do
      findmnt -rno TARGET "/dev/$p" >/dev/null 2>&1 && sudo umount "/dev/$p" || true
    done
    sudo dd if="$IMG" of="$DEV" bs=16M oflag=direct conv=fsync status=progress
    sync
    if [ "$VERIFY_FLASH" = 1 ]; then
      echo "[bbb] verifying flash (readback compare)"
      sudo blockdev --flushbufs "$DEV" 2>/dev/null || true
      sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
      sudo cmp -n "$(stat -c %s "$IMG")" "$IMG" "$DEV" \
        || { echo "error: flash verification failed (failing/counterfeit card or reader)" >&2; exit 1; }
    fi
    sudo partprobe "$DEV" 2>/dev/null || true
    sudo udevadm settle 2>/dev/null || true

    echo "[bbb] staging lib + examples + tests → /home/$DUT_USER/drm-cxx on the card"
    ROOTPART=$(lsblk -brno NAME,SIZE,TYPE "$DEV" \
      | awk '$3=="part"{print $2"\t"$1}' | sort -n | tail -1 | cut -f2)
    [ -n "$ROOTPART" ] || { echo "error: no partition found on $DEV" >&2; exit 1; }
    ROOTPART="/dev/$ROOTPART"
    MP=$(mktemp -d)
    cleanup() { sudo umount -lq "$MP" 2>/dev/null || true; rmdir "$MP" 2>/dev/null || true; }
    trap cleanup EXIT
    sudo mount "$ROOTPART" "$MP"
    [ -d "$MP/home/$DUT_USER" ] \
      || { echo "error: /home/$DUT_USER not present on the image" >&2; exit 1; }
    DEST="$MP/home/$DUT_USER/drm-cxx"
    sudo install -d "$DEST"
    # Shared libs (preserve SONAME symlinks): drm-cxx + the tomlplusplus subproject.
    sudo cp -a "$BUILD_DIR/src/libdrm-cxx.so" "$DEST/"
    sudo cp -a "$BUILD_DIR"/subprojects/tomlplusplus/src/libtomlplusplus.so.* "$DEST/" 2>/dev/null || true
    # Only the examples verified to run on this board's tilcdc display. The
    # present demos negotiate RGB565 (tilcdc has no XRGB8888); the rest are
    # read-only diagnostics. Everything omitted needs hardware the AM335x lacks
    # (GPU/Vulkan, multiple CRTCs, a cursor plane, HDR, compression, a camera).
    BBB_EXAMPLES="software_present ring_present idle_present damage_present \
                  driver_caps plane_caps stream_probe minimal_kms_probe"
    for ex in $BBB_EXAMPLES; do
      f=$(find "$BUILD_DIR/examples" -type f -executable -name "$ex" | head -1)
      [ -n "$f" ] && sudo cp "$f" "$DEST/"
    done
    # Test binaries (gtest); the vkms-gated ones self-skip on this board.
    find "$BUILD_DIR/tests" -maxdepth 1 -type f -executable -name 'test_*' -print0 \
      | sudo xargs -0 -r cp -t "$DEST/"
    # Own everything by the DUT login (read the uid:gid from the image's passwd).
    OWNER=$(sudo awk -F: -v u="$DUT_USER" '$1==u{print $3":"$4}' "$MP/etc/passwd")
    [ -n "$OWNER" ] || OWNER="1000:1000"
    sudo chown -R "$OWNER" "$MP/home/$DUT_USER/drm-cxx"
    sync; cleanup; trap - EXIT

    n_ex=$(echo "$BBB_EXAMPLES" | wc -w)
    n_te=$(find "$BUILD_DIR/tests" -maxdepth 1 -type f -executable -name 'test_*' | wc -l)
    echo "[bbb] DONE — flashed $DEV; staged $n_ex examples + $n_te tests to /home/$DUT_USER/drm-cxx"
    echo "[bbb] on the BeagleBone Black (from a free VT — DRM master):"
    echo "        cd ~/drm-cxx && LD_LIBRARY_PATH=. ./software_present /dev/dri/card0 120 --vsync"
    exit 0
  fi

  # ── Build mode: cross-build drm-cxx inside the container ─────────────────────
  command -v podman >/dev/null 2>&1 \
    || { echo "build_beaglebone_black: podman is required" >&2; exit 1; }
  IMAGE="${BBB_BUILD_IMAGE:-docker.io/library/debian:trixie}"
  echo "build_beaglebone_black: cross-building drm-cxx (examples + tests) for the" \
       "BeagleBone Black (armhf, $IMAGE)…"
  exec podman run --rm -e IN_BBB_CONTAINER=1 -v "$REPO:/work:z" \
    "$IMAGE" bash /work/scripts/build_beaglebone_black.sh
fi

# ── Container side ────────────────────────────────────────────────────────────
export DEBIAN_FRONTEND=noninteractive

echo "[bbb] enabling armhf multiarch + apt deps"
# APT::Sandbox::User=root: the _apt sandbox user can't read the SELinux-relabeled
# bind mount; --no-install-recommends keeps the image lean.
dpkg --add-architecture armhf
apt-get -o APT::Sandbox::User=root update -qq >/dev/null
apt-get -o APT::Sandbox::User=root install -y -qq --no-install-recommends \
  crossbuild-essential-armhf meson ninja-build pkg-config git ca-certificates \
  python3 hwdata \
  libdrm-dev:armhf libgbm-dev:armhf libegl-dev:armhf libinput-dev:armhf \
  libudev-dev:armhf libxkbcommon-dev:armhf libdisplay-info-dev:armhf \
  libfmt-dev:armhf libseat-dev:armhf libgtest-dev:armhf >/dev/null

echo "[bbb] writing meson cross-file (cortex-a8, NEON, hard-float)"
CROSS=/tmp/bbb.cross
cat > "$CROSS" <<EOF
[binaries]
c = '${TRIPLE}-gcc'
cpp = '${TRIPLE}-g++'
# gcc-ar / gcc-ranlib load the LTO plugin, so static libs (e.g. the example
# helper .a's) archive LTO objects correctly — plain ar drops them.
ar = '${TRIPLE}-gcc-ar'
ranlib = '${TRIPLE}-gcc-ranlib'
strip = '${TRIPLE}-strip'
pkg-config = 'pkg-config'

[properties]
# Debian multiarch: the armhf .pc files live under the triplet dir.
pkg_config_libdir = '/usr/lib/${TRIPLE}/pkgconfig:/usr/share/pkgconfig'

[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'cortex-a8'
endian = 'little'

[built-in options]
c_args = ['-mcpu=cortex-a8', '-mfpu=neon', '-mfloat-abi=hard']
cpp_args = ['-mcpu=cortex-a8', '-mfpu=neon', '-mfloat-abi=hard']
EOF

echo "[bbb] meson setup (vulkan off; accelerated/heavy features off; examples + tests on)"
cd /work
rm -rf bbb-build
meson setup bbb-build --cross-file "$CROSS" \
  -Dexamples=true -Dtests=true \
  -Dvulkan=false \
  -Dgstreamer=disabled -Dblend2d=disabled -Dstreams=disabled \
  -Dnvbufsurface=disabled -Dcamera=disabled -Dthorvg_janitor=disabled

echo "[bbb] ninja"
ninja -C bbb-build

echo "[bbb] DONE → bbb-build/src/libdrm-cxx.so + bbb-build/examples/* + bbb-build/tests/*"
echo "[bbb] software examples (run these on-target):"
ls bbb-build/examples 2>/dev/null | grep -iE 'software_present|ring_present|atomic_modeset|signage|idle_present|driver_caps' | sed 's/^/  /'
