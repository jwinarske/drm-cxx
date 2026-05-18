#!/bin/bash
# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# jetson_force_mode.sh — add a `video=CONN:WxH@Hz` argument to the
# Jetson kernel cmdline via /boot/extlinux/extlinux.conf, so the kernel
# DRM driver advertises a connector mode that isn't in the panel's EDID.
#
# Why this is needed on Jetson L4T: the Tegra DRM driver hard-validates
# every atomic-commit mode blob against the EDID-derived mode list. A
# custom CVT-RB modeline crafted at runtime by drm-cxx (cluster_sim's
# --mode flag) gets EINVAL at first commit, even when the panel's
# Display Range Limits descriptor says the timing is in range. The
# kernel-side `video=` cmdline parser injects the mode at boot, before
# any userspace atomic commits, so DRM treats it as a valid mode.
#
# Risks: a malformed APPEND line CAN brick the boot, leaving the board
# wedged at U-Boot or with a black screen. This script writes a backup
# before touching anything; the recovery path is documented at the end
# of the run.
#
# Usage:
#   sudo ./scripts/jetson_force_mode.sh --apply  CONNECTOR  WxH  Hz
#   sudo ./scripts/jetson_force_mode.sh --revert  [BACKUP_FILE]
#   sudo ./scripts/jetson_force_mode.sh --list-backups
#   sudo ./scripts/jetson_force_mode.sh --show
#
# Example:
#   sudo ./scripts/jetson_force_mode.sh --apply DP-1 1920x1080 240
#
# After --apply you must reboot for the change to take effect.

set -euo pipefail

EXTLINUX="/boot/extlinux/extlinux.conf"
BACKUP_DIR="/boot/extlinux"
BACKUP_PREFIX="extlinux.conf.bak-jetson_force_mode-"

die() {
  printf 'jetson_force_mode: %s\n' "$*" >&2
  exit 1
}

require_root() {
  if [[ $EUID -ne 0 ]]; then
    die "must run as root (modifies $EXTLINUX). Re-run with sudo."
  fi
}

require_extlinux() {
  [[ -f "$EXTLINUX" ]] || die "$EXTLINUX not found — is this a Jetson L4T system?"
}

cmd_show() {
  require_extlinux
  printf '%s\n' "$EXTLINUX:"
  awk '/^[[:space:]]*(LABEL|MENU|DEFAULT|TIMEOUT|APPEND|LINUX|INITRD)[[:space:]]/ { print "  " $0 }' "$EXTLINUX"
}

cmd_list_backups() {
  shopt -s nullglob
  local -a backups=("$BACKUP_DIR/$BACKUP_PREFIX"*)
  if [[ ${#backups[@]} -eq 0 ]]; then
    printf 'no backups found in %s\n' "$BACKUP_DIR"
    return 0
  fi
  printf 'available backups (newest first):\n'
  for b in $(ls -1t "${backups[@]}"); do
    printf '  %s\n' "$b"
  done
}

cmd_revert() {
  require_root
  require_extlinux
  local target="${1:-}"

  if [[ -z "$target" ]]; then
    # pick newest backup
    shopt -s nullglob
    local -a backups=("$BACKUP_DIR/$BACKUP_PREFIX"*)
    if [[ ${#backups[@]} -eq 0 ]]; then
      die "no backups found in $BACKUP_DIR and no path supplied"
    fi
    target=$(ls -1t "${backups[@]}" | head -n1)
    printf 'reverting from newest backup: %s\n' "$target"
  fi
  [[ -f "$target" ]] || die "backup file not readable: $target"

  printf 'restoring %s → %s ...\n' "$target" "$EXTLINUX"
  cp -a "$target" "$EXTLINUX"
  printf 'done. Reboot for the original cmdline to take effect.\n'
}

cmd_apply() {
  require_root
  require_extlinux
  local conn="$1"
  local res="$2"
  local hz="$3"

  # Validate the args minimally — kernel will reject obvious nonsense
  # but a typo here gets baked into the bootloader, so check up front.
  [[ "$conn" =~ ^[A-Za-z0-9_-]+$ ]] || die "bad connector name: $conn"
  [[ "$res"  =~ ^[0-9]+x[0-9]+$ ]]  || die "bad resolution (want WxH): $res"
  [[ "$hz"   =~ ^[0-9]+$ ]]         || die "bad refresh (want integer Hz): $hz"

  local payload="video=${conn}:${res}@${hz}"

  # Find the active LABEL. extlinux honors DEFAULT; on this system it's
  # `primary`. Bail if there's no DEFAULT line or the matching LABEL.
  local default_label
  default_label=$(awk '/^[[:space:]]*DEFAULT[[:space:]]+/ { print $2; exit }' "$EXTLINUX")
  [[ -n "$default_label" ]] || die "no DEFAULT line in $EXTLINUX"

  # Find the APPEND line for that label. If `video=DP-1:...` is already
  # present, refuse to double-add; the user can --revert first.
  if grep -qE "^[[:space:]]*APPEND[[:space:]].*${payload//./\\.}" "$EXTLINUX"; then
    die "already contains '$payload' in APPEND — use --revert first"
  fi
  if grep -qE "^[[:space:]]*APPEND[[:space:]].*video=${conn}:" "$EXTLINUX"; then
    printf 'note: existing video=%s:* entry will be left alone; the new\n' "$conn"
    printf 'one is appended after it. The kernel uses the last matching\n'
    printf 'entry for a given connector, so the new mode wins.\n'
  fi

  # Backup
  local ts
  ts=$(date -u +'%Y%m%dT%H%M%SZ')
  local backup="${BACKUP_DIR}/${BACKUP_PREFIX}${ts}"
  cp -a "$EXTLINUX" "$backup"
  printf 'backup: %s\n' "$backup"

  # Insert payload at end of the matching label's APPEND line. The line
  # ends right before the next LABEL/comment/blank, which on Jetson is
  # the literal newline. awk per-label state keeps us from touching
  # other labels.
  local tmp
  tmp=$(mktemp /tmp/extlinux.XXXXXX)
  awk -v label="$default_label" -v add="$payload" '
    /^[[:space:]]*LABEL[[:space:]]+/ {
      # The label name is the first word after LABEL, regardless of leading whitespace.
      n = split($0, f, /[[:space:]]+/)
      # f[1] may be "" if line starts with whitespace; LABEL is the
      # first non-empty token, label name follows it.
      for (k = 1; k <= n; k++) {
        if (f[k] == "LABEL" && k+1 <= n) { cur = f[k+1]; break }
      }
      print
      next
    }
    /^[[:space:]]*APPEND[[:space:]]+/ && cur == label {
      sub(/[[:space:]]*$/, "")
      print $0 " " add
      next
    }
    { print }
  ' "$EXTLINUX" > "$tmp"

  # Sanity: must still contain exactly one APPEND line for our label,
  # must mention root=, must have our new payload exactly once.
  local n_append n_root n_payload
  n_append=$(awk -v label="$default_label" '
    /^[[:space:]]*LABEL[[:space:]]+/ {
      n = split($0, f, /[[:space:]]+/)
      for (k = 1; k <= n; k++) {
        if (f[k] == "LABEL" && k+1 <= n) { cur = f[k+1]; break }
      }
    }
    /^[[:space:]]*APPEND[[:space:]]+/ && cur == label { n_app++ }
    END { print n_app+0 }
  ' "$tmp")
  n_root=$(grep -cE 'root=' "$tmp" || true)
  n_payload=$(grep -cF "$payload" "$tmp" || true)

  if [[ "$n_append" -ne 1 || "$n_root" -lt 1 || "$n_payload" -ne 1 ]]; then
    rm -f "$tmp"
    die "post-edit sanity check failed (APPEND=$n_append root=$n_root payload=$n_payload). $EXTLINUX unchanged."
  fi

  mv "$tmp" "$EXTLINUX"
  chmod 644 "$EXTLINUX"

  printf '\napplied: %s on LABEL %s\n' "$payload" "$default_label"
  printf '\nnew APPEND line:\n'
  awk -v label="$default_label" '
    /^[[:space:]]*LABEL[[:space:]]+/ {
      n = split($0, f, /[[:space:]]+/)
      for (k = 1; k <= n; k++) {
        if (f[k] == "LABEL" && k+1 <= n) { cur = f[k+1]; break }
      }
    }
    /^[[:space:]]*APPEND[[:space:]]+/ && cur == label { print "  " $0 }
  ' "$EXTLINUX"

  cat <<EOF

next steps:
  * reboot the board for the new cmdline to take effect.
  * verify with:   cat /proc/cmdline   (should contain $payload)
  * verify mode shows up in DRM:
       cat /sys/class/drm/card1-${conn}/modes

recovery if the board fails to boot:
  * the bootloader honors a 30-second timeout on the menu; another LABEL
    in $EXTLINUX (if any) can be selected from the serial console at boot.
  * if you have a recovery host, mount the rootfs and run:
       sudo cp $backup $EXTLINUX
  * OR re-run this script later with:
       sudo $0 --revert $backup
EOF
}

usage() {
  cat <<EOF
Usage:
  sudo $0 --apply  CONNECTOR WxH Hz
  sudo $0 --revert [BACKUP_FILE]
  sudo $0 --list-backups
  sudo $0 --show

Examples:
  sudo $0 --apply DP-1 1920x1080 240
  sudo $0 --revert
  sudo $0 --list-backups

CONNECTOR is the DRM connector name (e.g. DP-1, HDMI-A-1). On Jetson L4T
the kernel DRM driver validates atomic-commit modes strictly against
EDID; this script adds a kernel-cmdline video= entry so a non-EDID mode
becomes valid at boot.
EOF
}

if [[ $# -lt 1 ]]; then
  usage
  exit 1
fi

case "$1" in
  --apply)
    shift
    [[ $# -eq 3 ]] || { usage; exit 1; }
    cmd_apply "$1" "$2" "$3"
    ;;
  --revert)
    shift
    cmd_revert "${1:-}"
    ;;
  --list-backups)
    cmd_list_backups
    ;;
  --show)
    cmd_show
    ;;
  -h|--help)
    usage
    ;;
  *)
    usage
    exit 1
    ;;
esac
