#!/usr/bin/env bash
# SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# Make sure every package named on the command line is installed, after
# awalsh128/cache-apt-pkgs-action has had its go.
#
# That action does not fail when apt does: if the runner's mirror returns 404
# for part of a package set -- its index names a version its pool does not
# have yet -- it installs nothing, saves an empty cache under the key anyway,
# and reports success. The job then dies much later with "meson: command not
# found", and every later run with the same package versions restores the
# empty cache and dies the same way.
#
# So check what actually got installed. Install whatever is missing straight
# from apt, and if the runner's mirror still cannot serve it, from
# archive.ubuntu.com. Fail with the list of what is missing if neither can.
set -euo pipefail

if [ "$#" -eq 0 ]; then
  echo "usage: $0 <package>..." >&2
  exit 2
fi

missing_packages() {
  local pkg
  for pkg in "$@"; do
    if [ "$(dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null || true)" != "install ok installed" ]; then
      printf '%s\n' "$pkg"
    fi
  done
}

mapfile -t missing < <(missing_packages "$@")
if [ "${#missing[@]}" -eq 0 ]; then
  echo "all ${#} apt packages installed"
  exit 0
fi

echo "::warning::the cached apt install left ${#missing[@]} package(s) missing: ${missing[*]}; installing them directly"

install_missing() {
  sudo apt-get update -q &&
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends "$@"
}

if ! install_missing "${missing[@]}"; then
  # The runner's mirror list (apt-mirrors.txt, used through the mirror+file
  # method) does not fall back when a mirror 404s a file its own index names.
  # Point apt at the primary archive and try once more.
  echo "::warning::the runner's apt mirror could not serve them; retrying against archive.ubuntu.com"
  if [ -f /etc/apt/apt-mirrors.txt ]; then
    printf 'http://archive.ubuntu.com/ubuntu/\tpriority:1\n' | sudo tee /etc/apt/apt-mirrors.txt >/dev/null
  fi
  install_missing "${missing[@]}" || true
fi

mapfile -t still_missing < <(missing_packages "$@")
if [ "${#still_missing[@]}" -ne 0 ]; then
  echo "::error::apt packages still missing after a direct install: ${still_missing[*]}"
  exit 1
fi
echo "installed the ${#missing[@]} missing apt package(s) directly"
