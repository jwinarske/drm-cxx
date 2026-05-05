#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# build-deps.sh — build libcamera, blend2d, libseat, and libyuv from
# source and install them to /usr/local (or $PREFIX).
#
# Usage:
#   scripts/build-deps.sh [libcamera|blend2d|libseat|libyuv] ...
#
# With no arguments, all four are built. Environment overrides:
#   PREFIX    install prefix (default: /usr/local)
#   WORKDIR   scratch directory for clones/builds (default: /tmp/build-deps)
#   JOBS      parallel build jobs (default: nproc)
#   SUDO      command used for install step (default: sudo, or empty when root)
#   LIBCAMERA_REF / BLEND2D_REF / LIBSEAT_REF / LIBYUV_REF
#             git ref to check out (default: each project's main branch)

set -euo pipefail

PREFIX="${PREFIX:-/usr/local}"
WORKDIR="${WORKDIR:-/tmp/build-deps}"
JOBS="${JOBS:-$(nproc)}"

if [[ "${EUID}" -eq 0 ]]; then
    SUDO="${SUDO-}"
else
    SUDO="${SUDO-sudo}"
fi

LIBCAMERA_URL="https://git.libcamera.org/libcamera/libcamera.git"
LIBCAMERA_REF="${LIBCAMERA_REF:-master}"

BLEND2D_URL="https://github.com/blend2d/blend2d.git"
BLEND2D_REF="${BLEND2D_REF:-master}"
ASMJIT_URL="https://github.com/asmjit/asmjit.git"
ASMJIT_REF="${ASMJIT_REF:-master}"

LIBSEAT_URL="https://git.sr.ht/~kennylevinsen/seatd"
LIBSEAT_REF="${LIBSEAT_REF:-master}"

LIBYUV_URL="https://chromium.googlesource.com/libyuv/libyuv"
LIBYUV_REF="${LIBYUV_REF:-main}"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

require() {
    for tool in "$@"; do
        command -v "${tool}" >/dev/null 2>&1 \
            || die "required tool '${tool}' not found in PATH"
    done
}

# Clone if missing, otherwise fetch + reset to the requested ref.
fetch_repo() {
    local url="$1" dir="$2" ref="$3"
    if [[ -d "${dir}/.git" ]]; then
        log "updating ${dir} (${ref})"
        git -C "${dir}" fetch --tags --prune origin
    else
        log "cloning ${url} -> ${dir}"
        git clone "${url}" "${dir}"
    fi
    git -C "${dir}" checkout -f "${ref}"
    # If ref is a branch, fast-forward to its tip.
    if git -C "${dir}" show-ref --verify --quiet "refs/remotes/origin/${ref}"; then
        git -C "${dir}" reset --hard "origin/${ref}"
    fi
}

build_libcamera() {
    require git meson ninja pkg-config
    local src="${WORKDIR}/libcamera"
    fetch_repo "${LIBCAMERA_URL}" "${src}" "${LIBCAMERA_REF}"

    log "configuring libcamera"
    meson setup "${src}/build" "${src}" \
        --prefix="${PREFIX}" \
        --buildtype=release \
        --reconfigure \
        -Dpipelines=auto \
        -Dipas=ipu3,mali-c55,rkisp1,rpi/pisp,rpi/vc4,simple,vimc \
        -Dtest=false \
        -Ddocumentation=disabled \
        -Dgstreamer=disabled \
        -Dqcam=disabled \
        -Dpycamera=disabled \
        -Dcam=disabled \
        -Dlc-compliance=disabled

    log "building libcamera"
    ninja -C "${src}/build" -j "${JOBS}"

    log "installing libcamera to ${PREFIX}"
    ${SUDO} ninja -C "${src}/build" install
}

build_blend2d() {
    require git cmake
    local src="${WORKDIR}/blend2d"
    fetch_repo "${BLEND2D_URL}" "${src}" "${BLEND2D_REF}"
    # blend2d expects asmjit as a sibling at 3rdparty/asmjit by default.
    fetch_repo "${ASMJIT_URL}" "${src}/3rdparty/asmjit" "${ASMJIT_REF}"

    log "configuring blend2d"
    cmake -S "${src}" -B "${src}/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
        -DBLEND2D_STATIC=FALSE \
        -DBLEND2D_TEST=FALSE

    log "building blend2d"
    cmake --build "${src}/build" -j "${JOBS}"

    log "installing blend2d to ${PREFIX}"
    ${SUDO} cmake --install "${src}/build"
}

build_libseat() {
    require git meson ninja pkg-config
    local src="${WORKDIR}/seatd"
    fetch_repo "${LIBSEAT_URL}" "${src}" "${LIBSEAT_REF}"

    log "configuring libseat (seatd)"
    meson setup "${src}/build" "${src}" \
        --prefix="${PREFIX}" \
        --buildtype=release \
        --reconfigure \
        -Dlibseat-logind=auto \
        -Dlibseat-seatd=enabled \
        -Dserver=enabled \
        -Dexamples=disabled \
        -Dman-pages=disabled

    log "building libseat"
    ninja -C "${src}/build" -j "${JOBS}"

    log "installing libseat to ${PREFIX}"
    ${SUDO} ninja -C "${src}/build" install
}

build_libyuv() {
    require git cmake
    local src="${WORKDIR}/libyuv"
    fetch_repo "${LIBYUV_URL}" "${src}" "${LIBYUV_REF}"

    log "configuring libyuv"
    cmake -S "${src}" -B "${src}/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${PREFIX}"

    log "building libyuv"
    cmake --build "${src}/build" -j "${JOBS}"

    log "installing libyuv to ${PREFIX}"
    ${SUDO} cmake --install "${src}/build"
}

main() {
    mkdir -p "${WORKDIR}"
    local targets=("$@")
    if [[ ${#targets[@]} -eq 0 ]]; then
        targets=(libcamera blend2d libseat libyuv)
    fi

    for target in "${targets[@]}"; do
        case "${target}" in
            libcamera) build_libcamera ;;
            blend2d)   build_blend2d ;;
            libseat|seatd) build_libseat ;;
            libyuv)    build_libyuv ;;
            *) die "unknown target: ${target} (expected libcamera|blend2d|libseat|libyuv)" ;;
        esac
    done

    log "refreshing dynamic linker cache"
    ${SUDO} ldconfig

    log "done. installed to ${PREFIX}"
}

main "$@"
