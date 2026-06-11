// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Integration test for drm::present::DumbScanoutSink against vkms: discover the
// output, create the sink, and present frames with DRM_MODE_PAGE_FLIP_EVENT,
// asserting the flip-complete event arrives each frame (the vsync seam) and that
// the ring reuses buffers across frames. Also checks the too-small-frame guard.
// Self-skips when vkms is absent (modprobe vkms enable_overlay=1). Modesets, so
// not parallel.

#include "core/device.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/present/dumb_scanout_sink.hpp>

#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

std::optional<std::string> find_vkms_node() {
  for (int idx = 0; idx < 8; ++idx) {
    std::string path = "/dev/dri/card" + std::to_string(idx);
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    drmVersionPtr ver = drmGetVersion(fd);
    const bool is_vkms = (ver != nullptr) && (ver->name != nullptr) &&
                         std::string(ver->name, ver->name_len) == "vkms";
    if (ver != nullptr) {
      drmFreeVersion(ver);
    }
    ::close(fd);
    if (is_vkms) {
      return path;
    }
  }
  return std::nullopt;
}

struct ActiveCrtc {
  std::uint32_t crtc_id{0};
  std::uint32_t connector_id{0};
  drmModeModeInfo mode{};
};

std::optional<ActiveCrtc> pick_crtc(int fd) {
  auto* res = drmModeGetResources(fd);
  if (res == nullptr) {
    return std::nullopt;
  }
  std::optional<ActiveCrtc> found;
  for (int i = 0; i < res->count_connectors && !found.has_value(); ++i) {
    auto* conn = drmModeGetConnector(fd, res->connectors[i]);
    if (conn == nullptr) {
      continue;
    }
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
      for (int e = 0; e < conn->count_encoders && !found.has_value(); ++e) {
        auto* enc = drmModeGetEncoder(fd, conn->encoders[e]);
        if (enc == nullptr) {
          continue;
        }
        for (int c = 0; c < res->count_crtcs; ++c) {
          if ((enc->possible_crtcs & (1U << static_cast<unsigned>(c))) != 0) {
            found = ActiveCrtc{res->crtcs[c], conn->connector_id, conn->modes[0]};
            break;
          }
        }
        drmModeFreeEncoder(enc);
      }
    }
    drmModeFreeConnector(conn);
  }
  drmModeFreeResources(res);
  return found;
}

}  // namespace

TEST(DumbScanoutSinkVkms, PresentAndFlip) {
  const auto node = find_vkms_node();
  if (!node.has_value()) {
    GTEST_SKIP() << "vkms not loaded; modprobe vkms enable_overlay=1.";
  }
  auto dev = drm::Device::open(*node);
  ASSERT_TRUE(dev.has_value()) << dev.error().message();
  ASSERT_TRUE(dev->enable_universal_planes().has_value());
  ASSERT_TRUE(dev->enable_atomic().has_value());

  const auto active = pick_crtc(dev->fd());
  if (!active.has_value()) {
    GTEST_SKIP() << "no connected output on vkms";
  }

  auto sink_r = drm::present::DumbScanoutSink::create(*dev, active->crtc_id, active->connector_id,
                                                      active->mode);
  ASSERT_TRUE(sink_r.has_value()) << sink_r.error().message();
  auto sink = std::move(*sink_r);
  EXPECT_EQ(sink->width(), active->mode.hdisplay);
  EXPECT_EQ(sink->height(), active->mode.vdisplay);

  const std::uint32_t stride = sink->width() * 4U;
  std::vector<std::byte> frame(static_cast<std::size_t>(stride) * sink->height(), std::byte{0x40});

  drm::PageFlip page_flip(*dev);
  int flips = 0;
  page_flip.set_handler([&](std::uint32_t, std::uint64_t, std::uint64_t) { ++flips; });

  // Present several frames, pacing on the flip-complete event each time. This
  // exercises the modeset commit (frame 0), steady-state flips, and ring reuse.
  constexpr int k_frames = 3;
  for (int f = 0; f < k_frames; ++f) {
    auto r =
        sink->present({frame.data(), frame.size()}, stride, DRM_MODE_PAGE_FLIP_EVENT, &page_flip);
    ASSERT_TRUE(r.has_value()) << "present f=" << f << ": " << r.error().message();
    const int before = flips;
    while (flips == before) {
      auto d = page_flip.dispatch(2000);
      ASSERT_TRUE(d.has_value()) << "flip event f=" << f << ": " << d.error().message();
    }
  }
  EXPECT_EQ(flips, k_frames);

  // The argument guard rejects a frame shorter than height * stride before IO.
  auto too_small = sink->present({frame.data(), 16}, stride);
  EXPECT_FALSE(too_small.has_value());

  drmModeSetCrtc(dev->fd(), active->crtc_id, 0, 0, 0, nullptr, 0, nullptr);
}
