// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Which /dev/dri node the vkms integration tests run on.
//
// A host can carry more than one vkms device: the module's own, plus configfs
// instances built for multi-pipe tests (scripts/vkms_dual.sh, or another
// project's). Taking the first one a directory listing returns made the plane
// layout under test depend on the listing order. The single-pipe tests take
// the vkms device with the fewest CRTCs, which is the module's own; the
// multi-pipe tests look for theirs by connected outputs.

#pragma once

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <string>
#include <unistd.h>

namespace drm::test {

/// The vkms device with the fewest CRTCs, lowest card number first; nullopt
/// when vkms is not loaded.
[[nodiscard]] inline std::optional<std::string> find_vkms_node() {
  std::optional<std::string> best;
  int best_crtcs = std::numeric_limits<int>::max();
  for (int idx = 0; idx < 64; ++idx) {
    const std::string path = "/dev/dri/card" + std::to_string(idx);
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    drmVersionPtr v = drmGetVersion(fd);
    const bool is_vkms =
        (v != nullptr) && (v->name != nullptr) && (std::strcmp(v->name, "vkms") == 0);
    drmFreeVersion(v);
    drmModeResPtr res = is_vkms ? drmModeGetResources(fd) : nullptr;
    if (res != nullptr && res->count_crtcs < best_crtcs) {
      best_crtcs = res->count_crtcs;
      best = path;
    }
    drmModeFreeResources(res);
    ::close(fd);
  }
  return best;
}

/// DRM_CXX_TEST_CARD when set (a modeset card, to run on real hardware), else
/// find_vkms_node().
[[nodiscard]] inline std::optional<std::string> find_test_card_or_vkms() {
  if (const char* env = std::getenv("DRM_CXX_TEST_CARD"); env != nullptr && *env != '\0') {
    return std::string(env);
  }
  return find_vkms_node();
}

}  // namespace drm::test
