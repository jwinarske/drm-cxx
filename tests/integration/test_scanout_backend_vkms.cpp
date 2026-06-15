// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// tests/integration/test_scanout_backend_vkms.cpp
//
// End-to-end check of drm::present::ScanoutBackend against vkms: discover the
// output, negotiate, allocate a LINEAR GBM buffer via GbmScanoutProducer, build
// the single-layer scene, and commit one frame. Self-skips when vkms is not
// loaded (modprobe vkms enable_overlay=1). Not parallel: it modesets.

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/present/gbm_producer.hpp>
#include <drm-cxx/present/scanout_backend.hpp>

#include <xf86drm.h>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <unistd.h>

namespace {

[[nodiscard]] std::optional<std::string> find_vkms_node() noexcept {
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

}  // namespace

TEST(ScanoutBackendVkms, PresentFullScreen) {
  const auto path = find_vkms_node();
  if (!path.has_value()) {
    GTEST_SKIP() << "vkms not loaded; modprobe vkms enable_overlay=1.";
  }

  auto dev = drm::Device::open(*path);
  ASSERT_TRUE(dev.has_value()) << "Device::open: " << dev.error().message();

  drm::present::GbmScanoutProducer producer(*dev);
  auto backend = drm::present::ScanoutBackend::create(*dev, producer);
  ASSERT_TRUE(backend.has_value()) << "ScanoutBackend::create: " << backend.error().message();

  EXPECT_EQ((*backend)->profile().name, "vkms");
  EXPECT_NE((*backend)->target().primary_plane_id, 0U);
  EXPECT_GT((*backend)->target().mode.hdisplay, 0);

  auto report = (*backend)->present(0);
  ASSERT_TRUE(report.has_value()) << "present: " << report.error().message();
  EXPECT_GE(report->layers_total, 1U);
  EXPECT_GE(report->layers_assigned, 1U);  // the full-screen layer lands on a plane
}

TEST(ScanoutBackendVkms, IdleSuppression) {
  const auto path = find_vkms_node();
  if (!path.has_value()) {
    GTEST_SKIP() << "vkms not loaded; modprobe vkms enable_overlay=1.";
  }

  auto dev = drm::Device::open(*path);
  ASSERT_TRUE(dev.has_value()) << "Device::open: " << dev.error().message();

  drm::present::GbmScanoutProducer producer(*dev);
  auto backend = drm::present::ScanoutBackend::create(*dev, producer);
  ASSERT_TRUE(backend.has_value()) << "ScanoutBackend::create: " << backend.error().message();

  // First frame always commits (scanout contents are otherwise undefined),
  // even when flagged unchanged.
  auto first = (*backend)->present_if_changed(/*content_changed=*/false);
  ASSERT_TRUE(first.has_value()) << "present_if_changed: " << first.error().message();
  EXPECT_FALSE(first->skipped_idle);
  EXPECT_GE(first->layers_assigned, 1U);

  // A subsequent unchanged frame is suppressed: no commit, skipped_idle set,
  // every other count zero.
  auto idle = (*backend)->present_if_changed(/*content_changed=*/false);
  ASSERT_TRUE(idle.has_value()) << "present_if_changed: " << idle.error().message();
  EXPECT_TRUE(idle->skipped_idle);
  EXPECT_EQ(idle->layers_total, 0U);
  EXPECT_TRUE(idle->placements.empty());

  // A changed frame commits again.
  auto changed = (*backend)->present_if_changed(/*content_changed=*/true);
  ASSERT_TRUE(changed.has_value()) << "present_if_changed: " << changed.error().message();
  EXPECT_FALSE(changed->skipped_idle);
  EXPECT_GE(changed->layers_assigned, 1U);

  EXPECT_EQ((*backend)->frames_committed(), 2U);
  EXPECT_EQ((*backend)->frames_skipped(), 1U);
}
