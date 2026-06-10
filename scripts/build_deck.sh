#!/usr/bin/env bash
# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# build_deck.sh — cross-build drm-cxx (+ one example) for the Steam Deck.
#
# Why a container: SteamOS runs glibc ~2.41 / a gcc-13-class libstdc++. Building
# on a newer host links symbol versions the Deck lacks, so the binary won't run
# there. We build inside ubuntu:24.04 (glibc 2.39, gcc-13) for compatibility. The
# Deck already ships libdrm/gbm/input/udev/xkbcommon/libdisplay-info at runtime,
# so only the drm-cxx shared lib + the example binary need deploying.
#
# Usage (needs podman; run from anywhere in the tree):
#     scripts/build_deck.sh [EXAMPLE]          # EXAMPLE defaults to vrr_sweep
#
# Output (in the repo): deck-build/src/libdrm-cxx.so  and  deck-build/<EXAMPLE>
# Deploy + run on the Deck:
#     scp deck-build/src/libdrm-cxx.so deck-build/<EXAMPLE> deck@steamdeck:~/cs/
#     ssh deck@steamdeck 'LD_LIBRARY_PATH=~/cs ~/cs/<EXAMPLE> [args]'
#
# Override the container cache dir with DECK_BUILD_CACHE (default
# ~/.cache/drm-cxx-deck; holds the apt-built libdisplay-info between runs).
#
# Scope: builds the lib + "plain" examples (vrr_sweep, idle_present, ...). Examples
# that need blend2d (cluster_sim_vulkan, csd_*) or generated SPIR-V shaders need
# extra setup not covered here (build blend2d in-container; -I the host-generated
# *_spv.h) — see project memory / docs.
set -euo pipefail

EXAMPLE="${1:-vrr_sweep}"

# ── Host side: re-invoke self inside the build container ──────────────────────
if [ -z "${IN_DECK_CONTAINER:-}" ]; then
  if ! command -v podman >/dev/null 2>&1; then
    echo "build_deck: podman is required" >&2
    exit 1
  fi
  REPO=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
  CACHE="${DECK_BUILD_CACHE:-$HOME/.cache/drm-cxx-deck}"
  mkdir -p "$CACHE"
  echo "build_deck: cross-building '$EXAMPLE' for the Steam Deck (ubuntu:24.04)…"
  exec podman run --rm -e IN_DECK_CONTAINER=1 \
    -v "$REPO:/work:z" -v "$CACHE:/cache:z" \
    docker.io/library/ubuntu:24.04 bash /work/scripts/build_deck.sh "$EXAMPLE"
fi

# ── Container side ────────────────────────────────────────────────────────────
export DEBIAN_FRONTEND=noninteractive
echo "[deck] apt deps"
# APT::Sandbox::User=root: the _apt sandbox user can't read the SELinux-relabeled
# bind mount on the host; --no-install-recommends avoids a recommends postinst
# that fails in the minimal image.
apt-get -o APT::Sandbox::User=root update -qq >/dev/null
apt-get -o APT::Sandbox::User=root install -y -qq --no-install-recommends \
  meson ninja-build cmake g++ pkg-config git ca-certificates \
  libdrm-dev libgbm-dev libinput-dev libudev-dev libxkbcommon-dev hwdata python3 >/dev/null

echo "[deck] libdisplay-info 0.2 (noble ships 0.1.1; needs hwdata for pnp.ids)"
if [ ! -f /cache/ldi/b/libdisplay-info.so.2 ]; then
  rm -rf /cache/ldi
  git clone --depth 1 -b 0.2.0 \
    https://gitlab.freedesktop.org/emersion/libdisplay-info.git /cache/ldi
  meson setup /cache/ldi/b /cache/ldi --prefix=/usr -Dbuildtype=release
  ninja -C /cache/ldi/b
fi
ninja -C /cache/ldi/b install >/dev/null
# meson installs to /usr/lib64 here; make sure pkg-config + the linker see it.
export PKG_CONFIG_PATH="/usr/lib64/pkgconfig:/usr/lib/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="/usr/lib64:/usr/lib:/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
pkg-config --exists libdisplay-info || { echo "[deck] libdisplay-info not found after install" >&2; exit 1; }

cd /work
echo "[deck] drm-cxx lib (fmt via cmake subproject → static; no vulkan/examples/tests)"
[ -d deck-build ] || meson setup deck-build \
  -Dexamples=false -Dtests=false -Dvulkan=false --default-library=shared
ninja -C deck-build src/libdrm-cxx.so
FMTA=$(find deck-build -name libfmt.a | head -1)

SRC=$(find examples -path "*/$EXAMPLE/main.cpp" | head -1)
[ -n "$SRC" ] || { echo "[deck] example '$EXAMPLE' not found under examples/" >&2; exit 1; }
echo "[deck] compiling $SRC"
g++ -std=c++17 -O2 \
  -I src -I deck-build -I subprojects/tl-expected/include -I subprojects/tcb-span/include \
  -I subprojects/fmt/include -I examples -isystem third_party/Vulkan-Headers/include \
  $(pkg-config --cflags libdrm gbm) "$SRC" \
  -L deck-build/src -ldrm-cxx "$FMTA" $(pkg-config --libs libdrm gbm) -ldl -lpthread -lrt -lm \
  -Wl,-rpath,'$ORIGIN' -o "deck-build/$EXAMPLE"
echo "[deck] DONE → deck-build/src/libdrm-cxx.so + deck-build/$EXAMPLE"
