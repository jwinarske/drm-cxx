#!/usr/bin/env bash
# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# build_visionfive2.sh — cross-build drm-cxx (examples + tests) for the StarFive
# VisionFive 2 (StarFive JH7110, 64-bit RISC-V, quad SiFive U74 rv64gc, riscv64).
#
# Why a container: the VisionFive 2 runs a Debian riscv64 userland. We build
# inside debian:trixie (riscv64 is an official release architecture there) with
# the riscv64 multiarch -dev packages + the riscv64-linux-gnu cross-toolchain, so
# the binaries link exactly the libdrm / fmt / libdisplay-info / input / udev
# versions the board ships and run there unmodified.
#
# Scope: the JH7110's GPU is an Imagination PowerVR (BXE-4-32). Mainline open
# Vulkan/GL (Mesa "powervr") is experimental and far too slow to be useful — it
# runs vkcube under Weston and not much else. The display side is the Verisilicon
# DC8200 controller (drm/verisilicon), which does KMS dumb-buffer scanout fine.
# So Vulkan is off and the examples that run usefully on-target are the software /
# dumb-buffer ones (software_present, ring_present, atomic_modeset, signage_player,
# …). The GL examples still build but need llvmpipe (slow) and are not the point.
#
# Usage:
#   Build (needs podman; run from anywhere in the tree):
#     scripts/build_visionfive2.sh
#       Output (in the repo): vf2-build/src/libdrm-cxx.so + vf2-build/examples/* +
#       vf2-build/tests/*.
#
#   Flash the OS image to an SD card AND stage the built binaries to the board's
#   $HOME (needs sudo; ERASES the card; run the build first):
#     scripts/build_visionfive2.sh --image-sd /dev/sdX [--dut-user user]
#       Fetches + flashes the pinned StarFive VisionFive 2 Ubuntu 24.04.3 image,
#       then copies the lib + every example + every test binary to
#       /home/<dut-user>/drm-cxx on the card's rootfs. Then on the board, from a
#       free VT (DRM master):
#         cd ~/drm-cxx && LD_LIBRARY_PATH=. ./software_present /dev/dri/card0 120 --vsync
#       Options: --no-verify-flash, --image-url <url>, --image-sha256 <hex>.
#
#       Note the JH7110 needs a matching SPL + U-Boot in SPI NOR (the board boots
#       U-Boot from flash, then the OS from SD) before a flashed card will boot —
#       see the VisionFive 2 quick-start; this script only writes the SD image. To
#       track a different release, override with --image-url (+ --image-sha256).
#
# Override the container image with VF2_BUILD_IMAGE (default debian:trixie).
set -euo pipefail

TRIPLE=riscv64-linux-gnu

# ── Host side ─────────────────────────────────────────────────────────────────
if [ -z "${IN_VF2_CONTAINER:-}" ]; then
  REPO=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
  BUILD_DIR="$REPO/vf2-build"

  CACHE="${VF2_BUILD_CACHE:-$HOME/.cache/drm-cxx-vf2}"

  # Pinned StarFive VisionFive 2 Ubuntu 24.04.3 preinstalled-desktop image
  # (starfive-tech/VisionFive2 6.0.0 release, 1864042287 bytes). The pinned SHA256
  # matches the release's published sha256sum.txt — which still lists the
  # pre-rename "ubuntu-24.03-…" filename, but it is the same bytes as the current
  # "ubuntu-24.04.3-…" asset (verified).
  IMAGE_SD_DEV=""
  IMG_BASE="https://github.com/starfive-tech/VisionFive2/releases/download"
  IMG_URL="$IMG_BASE/6.0.0/ubuntu-24.04.3-preinstalled-desktop-riscv64+vf2-lite.img.bz2"
  IMG_SHA="6d7d9b864aea29be58f5e5dccc921bfb697317023e7de4c85720e1676f6c4814"
  DUT_USER="user"   # VisionFive 2 Ubuntu image default login
  VERIFY_FLASH=1
  STAGE_ONLY=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --image-sd)        IMAGE_SD_DEV="$2"; shift 2 ;;
      --dut-user)        DUT_USER="$2"; shift 2 ;;
      --no-verify-flash) VERIFY_FLASH=0; shift ;;
      --stage-only)      STAGE_ONLY=1; shift ;;
      --image-url)       IMG_URL="$2"; shift 2 ;;
      --image-sha256)    IMG_SHA="$2"; shift 2 ;;
      *) echo "build_visionfive2: unknown option: $1" >&2; exit 1 ;;
    esac
  done

  # ── Flash the OS image to an SD card + stage the cross-built binaries ────────
  if [ -n "$IMAGE_SD_DEV" ]; then
    DEV="$IMAGE_SD_DEV"
    [ -n "$IMG_URL" ] || { echo "error: --image-url is empty" >&2; exit 1; }
    [ -b "$DEV" ] || { echo "error: $DEV is not a block device" >&2; exit 1; }
    [ "$(lsblk -dno RM "$DEV" 2>/dev/null)" = 1 ] \
      || { echo "error: $DEV is not removable — refusing to erase it" >&2; exit 1; }
    [ "$(lsblk -dno TYPE "$DEV" 2>/dev/null)" = disk ] \
      || { echo "error: $DEV is not a whole disk (pass the device, not a partition)" >&2; exit 1; }
    [ -f "$BUILD_DIR/src/libdrm-cxx.so" ] \
      || { echo "error: no vf2-build — run the build (no args) first" >&2; exit 1; }
    command -v sudo >/dev/null 2>&1 || { echo "error: sudo is required to flash" >&2; exit 1; }

    mkdir -p "$CACHE"
    IMG_COMP="$CACHE/$(basename "$IMG_URL")"
    case "$IMG_COMP" in
      *.xz)  IMG="${IMG_COMP%.xz}";  DECOMP="xz -dkT0" ;;
      *.bz2) IMG="${IMG_COMP%.bz2}"; DECOMP="bunzip2 -kf" ;;
      *.gz)  IMG="${IMG_COMP%.gz}";  DECOMP="gunzip -kf" ;;
      *.img) IMG="$IMG_COMP";        DECOMP="" ;;
      *) echo "error: unrecognized image suffix on $(basename "$IMG_COMP")" \
              "(expected .img/.xz/.bz2/.gz)" >&2; exit 1 ;;
    esac
    # Verify a sha256 against $1, printing $2 as context on mismatch.
    verify_sha() {
      echo "$IMG_SHA  $1" | sha256sum -c --status - && return 0
      echo "error: image sha256 mismatch ($2)" >&2; return 1
    }
    if [ "$STAGE_ONLY" = 1 ]; then
      echo "[vf2] --stage-only: skipping download + flash; staging onto $DEV as-is"
    else
      if [ ! -s "$IMG_COMP" ]; then
        echo "[vf2] fetching $(basename "$IMG_COMP")"
        # No --continue-at: resuming a stale/over-long .part through GitHub's CDN
        # (which can answer a Range request with a full 200) appends bytes past EOF
        # and corrupts the file. --retry-all-errors restarts cleanly on transient
        # failures instead. We verify the .part BEFORE promoting it into the cache,
        # so a bad fetch never becomes the trusted image (and a re-run re-fetches).
        rm -f "$IMG_COMP.part"
        curl --fail --location --retry 3 --retry-all-errors \
          --output "$IMG_COMP.part" "$IMG_URL"
        if [ -n "$IMG_SHA" ]; then
          echo "[vf2] verifying downloaded image sha256"
          verify_sha "$IMG_COMP.part" "delete the cache and re-run to re-fetch" \
            || { rm -f "$IMG_COMP.part"; exit 1; }
        else
          echo "[vf2] WARNING: no --image-sha256 given — flashing an unverified image"
        fi
        mv "$IMG_COMP.part" "$IMG_COMP"
      elif [ -n "$IMG_SHA" ]; then
        echo "[vf2] verifying cached image sha256"
        verify_sha "$IMG_COMP" "delete $IMG_COMP and re-run to re-fetch" || exit 1
      else
        echo "[vf2] WARNING: no --image-sha256 given — flashing an unverified image"
      fi
      if [ -n "$DECOMP" ]; then
        [ -s "$IMG" ] || { echo "[vf2] decompressing image"; $DECOMP "$IMG_COMP"; }
      fi

      echo "[vf2] flashing $(basename "$IMG") → $DEV  (ERASES $DEV; needs sudo)"
      for p in $(lsblk -lno NAME "$DEV" | tail -n +2); do
        findmnt -rno TARGET "/dev/$p" >/dev/null 2>&1 && sudo umount "/dev/$p" || true
      done
      sudo dd if="$IMG" of="$DEV" bs=16M oflag=direct conv=fsync status=progress
      sync
      if [ "$VERIFY_FLASH" = 1 ]; then
        echo "[vf2] verifying flash (readback compare)"
        sudo blockdev --flushbufs "$DEV" 2>/dev/null || true
        sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
        sudo cmp -n "$(stat -c %s "$IMG")" "$IMG" "$DEV" \
          || { echo "error: flash verification failed (failing/counterfeit card or reader)" >&2; exit 1; }
      fi
    fi
    sudo partprobe "$DEV" 2>/dev/null || true
    sudo udevadm settle 2>/dev/null || true

    ROOTPART=$(lsblk -brno NAME,SIZE,TYPE "$DEV" \
      | awk '$3=="part"{print $2"\t"$1}' | sort -n | tail -1 | cut -f2)
    [ -n "$ROOTPART" ] || { echo "error: no partition found on $DEV" >&2; exit 1; }
    ROOTPART="/dev/$ROOTPART"
    MP=$(mktemp -d)
    cleanup() { sudo umount -lq "$MP" 2>/dev/null || true; rmdir "$MP" 2>/dev/null || true; }
    trap cleanup EXIT
    sudo mount "$ROOTPART" "$MP"
    # Where to stage: the DUT login's $HOME if it exists, else /opt/drm-cxx.
    # Preinstalled Ubuntu images create the login (and its $HOME) on first boot via
    # cloud-init, so /home/<user> is absent in the freshly-flashed rootfs — fall
    # back to a world-readable /opt/drm-cxx the account can reach after first boot.
    if [ -d "$MP/home/$DUT_USER" ]; then
      DESTREL="home/$DUT_USER/drm-cxx"
      OWNER=$(sudo awk -F: -v u="$DUT_USER" '$1==u{print $3":"$4}' "$MP/etc/passwd")
      [ -n "$OWNER" ] || OWNER="1000:1000"
      ON_BOARD="~/drm-cxx"
    else
      echo "[vf2] /home/$DUT_USER absent (created on first boot) — staging to /opt/drm-cxx"
      DESTREL="opt/drm-cxx"
      OWNER="0:0"
      ON_BOARD="/opt/drm-cxx"
    fi
    echo "[vf2] staging lib + examples + tests → /$DESTREL on the card"
    DEST="$MP/$DESTREL"
    sudo install -d "$DEST"
    # Shared libs (preserve SONAME symlinks): drm-cxx + the tomlplusplus subproject.
    sudo cp -a "$BUILD_DIR/src/libdrm-cxx.so" "$DEST/"
    sudo cp -a "$BUILD_DIR"/subprojects/tomlplusplus/src/libtomlplusplus.so.* "$DEST/" 2>/dev/null || true
    # Trixie runtime libs the build step stashed (Ubuntu 24.04 lacks these SONAMEs).
    sudo cp -a "$BUILD_DIR"/libfmt.so.10 "$BUILD_DIR"/libdisplay-info.so.2 \
               "$BUILD_DIR"/libseat.so.1 "$DEST/" 2>/dev/null || true
    # Every example + test binary, flattened so `LD_LIBRARY_PATH=. ./<name>` works.
    find "$BUILD_DIR/examples" -type f -executable ! -name '*.so*' -print0 \
      | sudo xargs -0 -r cp -t "$DEST/"
    find "$BUILD_DIR/tests" -maxdepth 1 -type f -executable -name 'test_*' -print0 \
      | sudo xargs -0 -r cp -t "$DEST/"
    sudo chown -R "$OWNER" "$DEST"
    sudo chmod -R a+rX "$DEST"   # world-readable so any login can run it
    sync; cleanup; trap - EXIT

    n_ex=$(find "$BUILD_DIR/examples" -type f -executable ! -name '*.so*' | wc -l)
    n_te=$(find "$BUILD_DIR/tests" -maxdepth 1 -type f -executable -name 'test_*' | wc -l)
    echo "[vf2] DONE — staged $n_ex examples + $n_te tests to /$DESTREL"
    echo "[vf2] on the VisionFive 2 (from a free VT, or 'sudo systemctl stop gdm'"
    echo "      first — DRM master). card1 = DC8200 KMS node; card0/renderD128 ="
    echo "      PowerVR render node. cd $ON_BOARD ; export LD_LIBRARY_PATH=."
    echo "      HW-validated examples (riscv64, Ubuntu 24.04, kernel 6.12.5):"
    echo "        # CPU present (dumb ring + atomic flip); DRM_FORCE_MODE=1280x720 for a lower mode"
    echo "        ./software_present /dev/dri/card1 120 --vsync   # + ring_/idle_/damage_present"
    echo "        # GPU-accelerated via Mesa kmsro (renders on the PowerVR, scans out on card1)"
    echo "        ./egl_scene /dev/dri/card1                      # + gl_present, gbm_surface_scanout"
    echo "        # scene / planes / cursor / modeset on the multi-plane DC8200"
    echo "        ./scene_priority /dev/dri/card1 --no-seat       # + scene_formats, layered_demo,"
    echo "        ./cursor_scene /dev/dri/card1 --no-seat         #   overlay_planes, atomic_modeset"
    echo "        # diagnostics (read-only)"
    echo "        ./driver_caps /dev/dri/card1 --no-seat          # + plane_caps, minimal_kms_probe,"
    echo "        ./compressed_scanout /dev/dri/card1 --no-seat   #   multi_crtc_probe, stream_probe"
    echo "      NOTE: egl_offload_scanout (manual GPU->display dma-buf import) does NOT work here —"
    echo "            the DC8200 can't import a foreign buffer; use the kmsro path above instead."
    exit 0
  fi

  # ── Build mode: cross-build drm-cxx inside the container ─────────────────────
  command -v podman >/dev/null 2>&1 \
    || { echo "build_visionfive2: podman is required" >&2; exit 1; }
  IMAGE="${VF2_BUILD_IMAGE:-docker.io/library/debian:trixie}"
  echo "build_visionfive2: cross-building drm-cxx (examples + tests) for the" \
       "VisionFive 2 (riscv64, $IMAGE)…"
  exec podman run --rm -e IN_VF2_CONTAINER=1 -v "$REPO:/work:z" \
    "$IMAGE" bash /work/scripts/build_visionfive2.sh
fi

# ── Container side ────────────────────────────────────────────────────────────
export DEBIAN_FRONTEND=noninteractive

echo "[vf2] enabling riscv64 multiarch + apt deps"
# APT::Sandbox::User=root: the _apt sandbox user can't read the SELinux-relabeled
# bind mount; --no-install-recommends keeps the image lean.
dpkg --add-architecture riscv64
apt-get -o APT::Sandbox::User=root update -qq >/dev/null
apt-get -o APT::Sandbox::User=root install -y -qq --no-install-recommends \
  crossbuild-essential-riscv64 meson ninja-build pkg-config git ca-certificates \
  python3 hwdata \
  libdrm-dev:riscv64 libgbm-dev:riscv64 libegl-dev:riscv64 libgles-dev:riscv64 libinput-dev:riscv64 \
  libudev-dev:riscv64 libxkbcommon-dev:riscv64 libdisplay-info-dev:riscv64 \
  libfmt-dev:riscv64 libseat-dev:riscv64 libgtest-dev:riscv64 >/dev/null

echo "[vf2] writing meson cross-file (rv64gc, lp64d hard-float)"
CROSS=/tmp/vf2.cross
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
# Debian multiarch: the riscv64 .pc files live under the triplet dir.
pkg_config_libdir = '/usr/lib/${TRIPLE}/pkgconfig:/usr/share/pkgconfig'

[host_machine]
system = 'linux'
cpu_family = 'riscv64'
cpu = 'rv64gc'
endian = 'little'

[built-in options]
# JH7110 U74 is rv64imafdc with the lp64d (hard-float double) ABI — the Debian
# riscv64 baseline. Stated explicitly to document intent.
c_args = ['-march=rv64gc', '-mabi=lp64d']
cpp_args = ['-march=rv64gc', '-mabi=lp64d']
EOF

echo "[vf2] meson setup (vulkan off; accelerated/heavy features off; examples + tests on)"
cd /work
rm -rf vf2-build
meson setup vf2-build --cross-file "$CROSS" \
  -Dexamples=true -Dtests=true \
  -Dvulkan=false \
  -Degl=enabled \
  -Dgstreamer=disabled -Dblend2d=disabled -Dstreams=disabled \
  -Dnvbufsurface=disabled -Dcamera=disabled -Dthorvg_janitor=disabled

echo "[vf2] ninja"
ninja -C vf2-build

# Stash the trixie riscv64 runtime libs the binaries need but the VisionFive 2
# Ubuntu 24.04 image lacks at the SONAMEs we built against (cross-distro gap:
# Ubuntu ships libfmt.so.9 / libdisplay-info.so.1, we link .so.10 / .so.2).
# The staging step ships these next to the binaries so LD_LIBRARY_PATH=. works.
echo "[vf2] stashing runtime libs (libfmt/libdisplay-info/libseat) for staging"
for so in libfmt.so.10 libdisplay-info.so.2 libseat.so.1; do
  src=$(find /usr/lib/${TRIPLE} /usr/lib -name "${so}" 2>/dev/null | head -1)
  [ -n "$src" ] && cp -L "$src" "vf2-build/${so}"
done

echo "[vf2] DONE → vf2-build/src/libdrm-cxx.so + vf2-build/examples/* + vf2-build/tests/*"
echo "[vf2] software examples (run these on-target):"
ls vf2-build/examples 2>/dev/null | grep -iE 'software_present|ring_present|atomic_modeset|signage|idle_present|driver_caps' | sed 's/^/  /'
