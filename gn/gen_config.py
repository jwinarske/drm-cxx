#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Generate drm-cxx's config.h from config.h.in for the GN build, substituting the
# same @VARS@ the CMake configure_file() fills. Mirrors CMakeLists.txt which
# writes ${binary}/drm-cxx/config.h.
import argparse
import sys


def main(argv):
    p = argparse.ArgumentParser()
    p.add_argument("--in", dest="src", required=True)
    p.add_argument("--out", dest="dst", required=True)
    p.add_argument("--version", required=True)  # e.g. 150.0.0
    p.add_argument("--have-vulkan", default="0")
    a = p.parse_args(argv[1:])

    major, minor, patch = (a.version.split(".") + ["0", "0", "0"])[:3]
    subs = {
        "@PROJECT_VERSION@": a.version,
        "@PROJECT_VERSION_MAJOR@": major,
        "@PROJECT_VERSION_MINOR@": minor,
        "@PROJECT_VERSION_PATCH@": patch,
        "@HAVE_VULKAN@": "1" if a.have_vulkan in ("1", "true", "True") else "0",
    }
    with open(a.src, "r", encoding="utf-8") as f:
        text = f.read()
    for k, v in subs.items():
        text = text.replace(k, v)
    with open(a.dst, "w", encoding="utf-8") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
