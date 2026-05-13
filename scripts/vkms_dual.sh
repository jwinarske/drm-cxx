#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# vkms_dual.sh — provision a vkms configfs instance with 2 connected
# virtual outputs, suitable for running tests/integration/test_scene_set_vkms.
#
# Usage:
#   sudo scripts/vkms_dual.sh up    [name]   # create + enable (default name: dual)
#   sudo scripts/vkms_dual.sh down  [name]   # disable + remove
#   sudo scripts/vkms_dual.sh status [name]  # print provisioned objects
#
# Requirements:
#   * Linux >= 6.11 (vkms configfs interface).
#   * configfs mounted at /sys/kernel/config (systemd does this by default).
#   * vkms module built into the running kernel.
#
# After `up`, a second /dev/dri/cardN appears with 2 CONNECTED virtual
# connectors driven by separate CRTCs. test_scene_set_vkms enumerates
# /dev/dri/card*, picks the first vkms node with >= 2 connected outputs,
# and runs its combined-commit + mirrored-layer assertions against it.
set -euo pipefail

CONFIGFS_ROOT=/sys/kernel/config/vkms
ACTION=${1:-up}
NAME=${2:-dual}
INSTANCE="${CONFIGFS_ROOT}/${NAME}"

DRM_PLANE_TYPE_OVERLAY=0
DRM_PLANE_TYPE_PRIMARY=1

die() {
  printf 'vkms_dual.sh: %s\n' "$*" >&2
  exit 1
}

require_root() {
  if [[ $EUID -ne 0 ]]; then
    die "must run as root (sudo $0 $*)"
  fi
}

ensure_module() {
  if [[ ! -d ${CONFIGFS_ROOT} ]]; then
    modprobe vkms || die "modprobe vkms failed; ensure CONFIG_DRM_VKMS=m or =y"
  fi
  if [[ ! -d ${CONFIGFS_ROOT} ]]; then
    die "${CONFIGFS_ROOT} missing after modprobe — kernel < 6.11 or vkms configfs disabled"
  fi
}

write_attr() {
  local path=$1 value=$2
  printf '%s' "${value}" > "${path}"
}

provision_up() {
  if [[ -d ${INSTANCE} ]] && [[ $(cat "${INSTANCE}/enabled" 2>/dev/null || echo 0) == 1 ]]; then
    printf 'vkms instance %s already enabled, leaving alone\n' "${NAME}"
    return 0
  fi

  if [[ ! -d ${INSTANCE} ]]; then
    mkdir "${INSTANCE}"
  else
    # Pre-existing but disabled instance — wipe its subobjects so we
    # don't double-create.
    write_attr "${INSTANCE}/enabled" 0 2>/dev/null || true
    rmdir "${INSTANCE}"/planes/* "${INSTANCE}"/connectors/* \
          "${INSTANCE}"/encoders/* "${INSTANCE}"/crtcs/* 2>/dev/null || true
  fi

  # Two pipelines: pipe0 (crtc0/enc0/conn0/plane0) and pipe1.
  # vkms requires a PRIMARY plane per CRTC.
  for i in 0 1; do
    local crtc="${INSTANCE}/crtcs/crtc${i}"
    local enc="${INSTANCE}/encoders/enc${i}"
    local conn="${INSTANCE}/connectors/conn${i}"
    local plane="${INSTANCE}/planes/plane${i}"

    mkdir "${crtc}"
    mkdir "${enc}"
    mkdir "${conn}"
    mkdir "${plane}"

    # encoder -> crtc
    ln -s "${crtc}" "${enc}/possible_crtcs/crtc${i}"
    # connector -> encoder
    ln -s "${enc}" "${conn}/possible_encoders/enc${i}"
    # plane: PRIMARY type, reachable from crtc${i}
    write_attr "${plane}/type" ${DRM_PLANE_TYPE_PRIMARY}
    ln -s "${crtc}" "${plane}/possible_crtcs/crtc${i}"
  done

  write_attr "${INSTANCE}/enabled" 1

  # Settle: udev takes a tick to publish the new /dev/dri/cardN node.
  for _ in $(seq 1 20); do
    if find /dev/dri -maxdepth 1 -name 'card*' -newer "${INSTANCE}/enabled" \
         -print -quit 2>/dev/null | grep -q .; then
      break
    fi
    sleep 0.1
  done

  printf 'vkms instance %s enabled\n' "${NAME}"
  provision_status
}

provision_down() {
  if [[ ! -d ${INSTANCE} ]]; then
    printf 'vkms instance %s not present, nothing to do\n' "${NAME}"
    return 0
  fi
  write_attr "${INSTANCE}/enabled" 0 || true
  # rmdir leaves are in reverse-dependency order: planes/connectors/encoders, then crtcs.
  rmdir "${INSTANCE}"/planes/* 2>/dev/null || true
  rmdir "${INSTANCE}"/connectors/* 2>/dev/null || true
  rmdir "${INSTANCE}"/encoders/* 2>/dev/null || true
  rmdir "${INSTANCE}"/crtcs/* 2>/dev/null || true
  rmdir "${INSTANCE}"
  printf 'vkms instance %s removed\n' "${NAME}"
}

provision_status() {
  if [[ ! -d ${INSTANCE} ]]; then
    printf 'vkms instance %s: not present\n' "${NAME}"
    return 0
  fi
  printf 'vkms instance %s: enabled=%s\n' "${NAME}" "$(cat "${INSTANCE}/enabled" 2>/dev/null || echo ?)"
  for sub in crtcs encoders connectors planes; do
    local entries
    entries=$(ls -1 "${INSTANCE}/${sub}" 2>/dev/null | tr '\n' ' ')
    printf '  %s: %s\n' "${sub}" "${entries:-<none>}"
  done
}

case "${ACTION}" in
  up)
    require_root "$@"
    ensure_module
    provision_up
    ;;
  down)
    require_root "$@"
    provision_down
    ;;
  status)
    provision_status
    ;;
  *)
    die "unknown action '${ACTION}' (expected: up | down | status)"
    ;;
esac
