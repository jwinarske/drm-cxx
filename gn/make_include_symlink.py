#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Create the build-tree `drm-cxx` -> `src` include symlink so the public headers'
# <drm-cxx/...> includes resolve, exactly as the CMake build does
# (${binary}/include/drm-cxx -> src). Idempotent; writes a stamp for GN ordering.
import argparse
import os
import sys


def main(argv):
    p = argparse.ArgumentParser()
    p.add_argument("--target", required=True)   # abs path to src/
    p.add_argument("--link", required=True)      # abs path of the drm-cxx symlink to create
    p.add_argument("--stamp", required=True)
    a = p.parse_args(argv[1:])

    # GN scripts run with cwd == root_build_dir; make these absolute so the
    # symlink is valid regardless of where it is dereferenced from.
    target = os.path.abspath(a.target)
    link = os.path.abspath(a.link)
    os.makedirs(os.path.dirname(link), exist_ok=True)
    # Refresh the symlink (target may move across `gn gen` runs).
    try:
        if os.path.islink(link) or os.path.exists(link):
            os.remove(link)
    except OSError:
        pass
    os.symlink(target, link)
    with open(a.stamp, "w", encoding="utf-8") as f:
        f.write("ok\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
