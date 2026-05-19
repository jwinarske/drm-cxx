#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# build-matrix.sh — exercise drm-cxx's build under the
# {GCC, Clang} × {Meson, CMake} × {feature gates} grid.
#
# Catches breakage that the default-feature build hides: unconditional
# includes that don't survive `-Dgstreamer=disabled`, modernize-use-auto
# regressions under a newer compiler, MSVC-style attribute differences
# between clang versions, etc.
#
# Usage:
#   scripts/build-matrix.sh [--meson|--cmake|--all] [--quick]
#
# Default is --all. --quick runs only the gcc-defaults cell of each
# build system (5-second smoke instead of full matrix).
#
# Environment overrides:
#   WORKDIR    scratch directory for build dirs (default: /tmp/build-matrix)
#   GCC_CC     C compiler for the gcc cells (default: gcc)
#   GCC_CXX    C++ compiler for the gcc cells (default: g++)
#   CLANG_CC   C compiler for the modern-clang cell (default: clang)
#   CLANG_CXX  C++ compiler for the modern-clang cell (default: clang++)
#   CI_CC      C compiler matching CI's clang version (default: clang-19)
#   CI_CXX     C++ compiler matching CI's clang version (default: clang++-19)
#   PKG_CONFIG_PATH / LD_LIBRARY_PATH passed through (set these if
#   `scripts/build-deps.sh libdisplay-info` installed to a non-system
#   prefix; without that the libdisplay-info >=0.2.0 probe will fail
#   on hosts whose distro ships an older version).

set -uo pipefail

WORKDIR="${WORKDIR:-/tmp/build-matrix}"
GCC_CC="${GCC_CC:-gcc}"
GCC_CXX="${GCC_CXX:-g++}"
CLANG_CC="${CLANG_CC:-clang}"
CLANG_CXX="${CLANG_CXX:-clang++}"
CI_CC="${CI_CC:-clang-19}"
CI_CXX="${CI_CXX:-clang++-19}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="all"
QUICK=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --meson) MODE="meson"; shift ;;
        --cmake) MODE="cmake"; shift ;;
        --all)   MODE="all";   shift ;;
        --quick) QUICK=1;      shift ;;
        -h|--help)
            sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
            exit 0
            ;;
        *)
            printf 'unknown arg: %s (try --help)\n' "$1" >&2
            exit 2
            ;;
    esac
done

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
pass() { printf '\033[1;32mPASS\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31mFAIL\033[0m %s\n' "$*"; }

RESULTS=()
FAILED=0

# Run one matrix cell. Configures + builds. On failure prints the tail
# of the offending log; on success records the link target count so
# the summary can show coverage at a glance.
#
# args: name, kind (meson|cmake), CC, CXX, then build-system args
cell() {
    local name="$1"; shift
    local kind="$1"; shift
    local cc="$1";   shift
    local cxx="$1";  shift
    local builddir="${WORKDIR}/${kind}-${name}"
    rm -rf "${builddir}"

    log "${kind}/${name}  CC=${cc} CXX=${cxx}  ${*}"

    local cfg_log="${WORKDIR}/${kind}-${name}.cfg.log"
    local build_log="${WORKDIR}/${kind}-${name}.build.log"

    case "${kind}" in
        meson)
            if ! CC="${cc}" CXX="${cxx}" meson setup "${builddir}" "${ROOT}" "$@" \
                    >"${cfg_log}" 2>&1; then
                fail "${kind}/${name} (configure)"
                tail -10 "${cfg_log}"
                RESULTS+=("${kind}/${name}: CONFIGURE_FAIL")
                FAILED=$((FAILED + 1))
                return
            fi
            if ! ninja -C "${builddir}" >"${build_log}" 2>&1; then
                fail "${kind}/${name} (build)"
                tail -15 "${build_log}"
                RESULTS+=("${kind}/${name}: BUILD_FAIL")
                FAILED=$((FAILED + 1))
                return
            fi
            local targets
            targets=$(grep -cE "Linking target" "${build_log}")
            pass "${kind}/${name} (${targets} targets)"
            RESULTS+=("${kind}/${name}: OK (${targets} targets)")
            ;;
        cmake)
            if ! CC="${cc}" CXX="${cxx}" cmake -S "${ROOT}" -B "${builddir}" -G Ninja "$@" \
                    >"${cfg_log}" 2>&1; then
                fail "${kind}/${name} (configure)"
                tail -10 "${cfg_log}"
                RESULTS+=("${kind}/${name}: CONFIGURE_FAIL")
                FAILED=$((FAILED + 1))
                return
            fi
            if ! cmake --build "${builddir}" >"${build_log}" 2>&1; then
                fail "${kind}/${name} (build)"
                tail -15 "${build_log}"
                RESULTS+=("${kind}/${name}: BUILD_FAIL")
                FAILED=$((FAILED + 1))
                return
            fi
            local targets
            targets=$(grep -cE "^\[[0-9]+/[0-9]+\] Linking " "${build_log}")
            pass "${kind}/${name} (${targets} targets)"
            RESULTS+=("${kind}/${name}: OK (${targets} targets)")
            ;;
        *)
            printf 'internal error: unknown kind %s\n' "${kind}" >&2
            exit 99
            ;;
    esac
}

run_meson_matrix() {
    log "meson matrix"
    cell "gcc-defaults"   meson "${GCC_CC}"   "${GCC_CXX}"
    if [[ ${QUICK} -eq 1 ]]; then return; fi
    cell "clang-defaults" meson "${CLANG_CC}" "${CLANG_CXX}"
    cell "ci-clang"       meson "${CI_CC}"    "${CI_CXX}"
    # Feature flips: pin the most common gates that have historically
    # bitten (unconditional GST includes, Vulkan-off compilation).
    cell "gst-off"        meson "${GCC_CC}" "${GCC_CXX}" -Dgstreamer=disabled
    cell "vk-off"         meson "${GCC_CC}" "${GCC_CXX}" -Dvulkan=false
    cell "minimal"        meson "${GCC_CC}" "${GCC_CXX}" \
        -Dvulkan=false -Dgstreamer=disabled -Dstreams=disabled \
        -Dnvbufsurface=disabled -Dblend2d=disabled -Dcamera=disabled \
        -Dsession=disabled -Dcursor=disabled \
        -Dexamples=false -Dtests=false -Dbenchmarks=false
}

run_cmake_matrix() {
    log "cmake matrix"
    # CMake gates: VULKAN, SESSION, CURSOR, STREAMS are explicit
    # options; GST / Blend2D / NvBufSurface are auto-detected from
    # pkg-config, so flipping them off cleanly needs the build env
    # itself (covered by the meson matrix). Three compiler cells +
    # two feature-flip cells.
    cell "gcc-defaults"   cmake "${GCC_CC}"   "${GCC_CXX}" \
        -DDRM_CXX_VULKAN=ON -DDRM_CXX_BUILD_EXAMPLES=ON -DDRM_CXX_BUILD_TESTS=ON
    if [[ ${QUICK} -eq 1 ]]; then return; fi
    cell "clang-defaults" cmake "${CLANG_CC}" "${CLANG_CXX}" \
        -DDRM_CXX_VULKAN=ON -DDRM_CXX_BUILD_EXAMPLES=ON -DDRM_CXX_BUILD_TESTS=ON
    cell "ci-clang"       cmake "${CI_CC}"    "${CI_CXX}" \
        -DDRM_CXX_VULKAN=ON -DDRM_CXX_BUILD_EXAMPLES=ON -DDRM_CXX_BUILD_TESTS=ON
    cell "vk-off"         cmake "${GCC_CC}"   "${GCC_CXX}" \
        -DDRM_CXX_VULKAN=OFF -DDRM_CXX_BUILD_EXAMPLES=ON -DDRM_CXX_BUILD_TESTS=ON
    cell "minimal"        cmake "${GCC_CC}"   "${GCC_CXX}" \
        -DDRM_CXX_VULKAN=OFF -DDRM_CXX_SESSION=OFF -DDRM_CXX_CURSOR=OFF \
        -DDRM_CXX_STREAMS=OFF -DDRM_CXX_BUILD_EXAMPLES=OFF \
        -DDRM_CXX_BUILD_TESTS=OFF -DDRM_CXX_BUILD_BENCHMARKS=OFF
}

mkdir -p "${WORKDIR}"

START=$(date +%s)

case "${MODE}" in
    meson) run_meson_matrix ;;
    cmake) run_cmake_matrix ;;
    all)   run_meson_matrix; run_cmake_matrix ;;
esac

ELAPSED=$(( $(date +%s) - START ))

echo
log "summary (${ELAPSED}s)"
for r in "${RESULTS[@]}"; do
    echo "  ${r}"
done
echo "  cells: ${#RESULTS[@]} total, ${FAILED} failed"

if [[ ${FAILED} -gt 0 ]]; then
    exit 1
fi
