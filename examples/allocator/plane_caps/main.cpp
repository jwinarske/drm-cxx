// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// examples/allocator/plane_caps/main.cpp
//
// Dumps every plane's scanout (fourcc, modifier) pairs, classified by bandwidth
// behaviour. First thing to run on a new board: it verifies the IN_FORMATS
// parser and tells you, per plane, whether the display engine can actually scan
// out a compressed layout -- or whether (DragonBoard MDP5, PVRIC on tidss/
// rcar-du) compression simply never reaches the scanout path.
//
// Run:   ./plane_caps [/dev/dri/card0]

#include <drm-cxx/fmt/format_mod.hpp>

#include <drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace fmt = drm::fmt;

namespace {
const char* class_name(fmt::BandwidthClass c) {
  switch (c) {
    case fmt::BandwidthClass::Linear:
      return "linear";
    case fmt::BandwidthClass::Tiling:
      return "tiling";
    case fmt::BandwidthClass::Compression:
      return "COMPRESSION";
  }
  return "?";
}
}  // namespace

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "/dev/dri/card0";

  int const fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    std::perror("open");
    return 1;
  }
  drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

  drmModePlaneRes* pr = drmModeGetPlaneResources(fd);
  if (pr == nullptr) {
    std::perror("drmModeGetPlaneResources");
    close(fd);
    return 1;
  }

  std::printf("%u plane(s) on %s\n", pr->count_planes, path);
  for (std::uint32_t i = 0; i < pr->count_planes; ++i) {
    const std::uint32_t plane_id = pr->planes[i];

    auto table = fmt::FormatTable::from_plane(fd, plane_id);
    if (!table) {
      std::printf("\nplane %u: no IN_FORMATS (%s) -- assume LINEAR-only\n", plane_id,
                  table.error().message().c_str());
      continue;
    }

    std::printf("\nplane %u: %zu (fourcc,modifier) pair(s)\n", plane_id, table->all().size());

    std::uint32_t last = 0;
    bool first = true;
    for (const fmt::FormatMod& fm : table->all()) {
      if (first || fm.fourcc != last) {
        char cc[5] = {char(fm.fourcc), char(fm.fourcc >> 8), char(fm.fourcc >> 16),
                      char(fm.fourcc >> 24), 0};
        std::printf("  %s:\n", cc);
        last = fm.fourcc;
        first = false;
      }
      std::printf("      %-28s [%s]\n", fmt::describe(fm.modifier).c_str(),
                  class_name(fmt::classify(fm.modifier)));
    }
  }

  drmModeFreePlaneResources(pr);
  close(fd);
  return 0;
}