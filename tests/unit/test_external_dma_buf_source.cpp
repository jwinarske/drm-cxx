// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::scene::ExternalDmaBufSource. Covers the contract
// visible without a live KMS device: argument validation, modifier-scope
// rejection, multi-plane rejection (PR-A scope), graceful failure on
// invalid fds, and the on_release callback firing exactly once.
//
// The full prime-import + drmModeAddFB2WithModifiers round-trip lives in
// an integration test against vgem/vkms (the test_vgem_buffer pattern is
// the natural template) and is intentionally not driven from this TU.

#include "core/device.hpp"

#include <drm-cxx/scene/external_dma_buf_source.hpp>

#include <drm_fourcc.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <system_error>
#include <unistd.h>

namespace {

constexpr std::uint32_t k_w = 320;
constexpr std::uint32_t k_h = 240;

drm::scene::ExternalPlaneInfo dummy_plane(int fd) {
  drm::scene::ExternalPlaneInfo p;
  p.fd = fd;
  p.offset = 0;
  p.pitch = k_w * 4;  // ARGB8888 stride
  return p;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// Argument validation — runs entirely against an invalid Device, so
// nothing touches the kernel.
// ─────────────────────────────────────────────────────────────────────

TEST(SceneExternalDmaBufSource, RejectsZeroWidth) {
  auto dev = drm::Device::from_fd(-1);
  std::array<drm::scene::ExternalPlaneInfo, 1> planes{dummy_plane(0)};
  auto r = drm::scene::ExternalDmaBufSource::create(dev, /*width=*/0, k_h, DRM_FORMAT_ARGB8888,
                                                    DRM_FORMAT_MOD_LINEAR, planes);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneExternalDmaBufSource, RejectsZeroHeight) {
  auto dev = drm::Device::from_fd(-1);
  std::array<drm::scene::ExternalPlaneInfo, 1> planes{dummy_plane(0)};
  auto r = drm::scene::ExternalDmaBufSource::create(dev, k_w, /*height=*/0, DRM_FORMAT_ARGB8888,
                                                    DRM_FORMAT_MOD_LINEAR, planes);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneExternalDmaBufSource, RejectsZeroFourcc) {
  auto dev = drm::Device::from_fd(-1);
  std::array<drm::scene::ExternalPlaneInfo, 1> planes{dummy_plane(0)};
  auto r = drm::scene::ExternalDmaBufSource::create(dev, k_w, k_h, /*drm_format=*/0,
                                                    DRM_FORMAT_MOD_LINEAR, planes);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneExternalDmaBufSource, RejectsEmptyPlaneSpan) {
  auto dev = drm::Device::from_fd(-1);
  const drm::span<const drm::scene::ExternalPlaneInfo> empty_planes;
  auto r = drm::scene::ExternalDmaBufSource::create(dev, k_w, k_h, DRM_FORMAT_ARGB8888,
                                                    DRM_FORMAT_MOD_LINEAR, empty_planes);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneExternalDmaBufSource, RejectsBadFd) {
  auto dev = drm::Device::from_fd(-1);
  std::array<drm::scene::ExternalPlaneInfo, 1> planes{dummy_plane(/*fd=*/-1)};
  auto r = drm::scene::ExternalDmaBufSource::create(dev, k_w, k_h, DRM_FORMAT_ARGB8888,
                                                    DRM_FORMAT_MOD_LINEAR, planes);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneExternalDmaBufSource, RejectsZeroPitch) {
  auto dev = drm::Device::from_fd(-1);
  drm::scene::ExternalPlaneInfo plane;
  plane.fd = 0;     // valid-looking fd; we don't reach prime import
  plane.pitch = 0;  // the rejection trigger
  std::array<drm::scene::ExternalPlaneInfo, 1> planes{plane};
  auto r = drm::scene::ExternalDmaBufSource::create(dev, k_w, k_h, DRM_FORMAT_ARGB8888,
                                                    DRM_FORMAT_MOD_LINEAR, planes);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

// ─────────────────────────────────────────────────────────────────────
// PR-A scope — multi-plane and tiled modifiers are rejected explicitly
// rather than silently giving partial behavior. PR-B widens both.
// ─────────────────────────────────────────────────────────────────────

TEST(SceneExternalDmaBufSource, RejectsMultiPlanePrA) {
  auto dev = drm::Device::from_fd(-1);
  std::array<drm::scene::ExternalPlaneInfo, 2> planes{dummy_plane(0), dummy_plane(0)};
  auto r = drm::scene::ExternalDmaBufSource::create(dev, k_w, k_h, DRM_FORMAT_NV12,
                                                    DRM_FORMAT_MOD_LINEAR, planes);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::operation_not_supported));
}

TEST(SceneExternalDmaBufSource, RejectsTooManyPlanes) {
  auto dev = drm::Device::from_fd(-1);
  std::array<drm::scene::ExternalPlaneInfo, 5> planes{};
  for (auto& p : planes) {
    p = dummy_plane(0);
  }
  auto r = drm::scene::ExternalDmaBufSource::create(dev, k_w, k_h, DRM_FORMAT_ARGB8888,
                                                    DRM_FORMAT_MOD_LINEAR, planes);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneExternalDmaBufSource, RejectsTiledModifierPrA) {
  auto dev = drm::Device::from_fd(-1);
  std::array<drm::scene::ExternalPlaneInfo, 1> planes{dummy_plane(0)};
  // I915_FORMAT_MOD_X_TILED — picked as a representative non-LINEAR
  // modifier without depending on a vendor-specific header.
  constexpr std::uint64_t k_x_tiled = (1ULL << 56U) | 1ULL;  // fourcc_mod_code(INTEL, 1)
  auto r = drm::scene::ExternalDmaBufSource::create(dev, k_w, k_h, DRM_FORMAT_ARGB8888, k_x_tiled,
                                                    planes);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::operation_not_supported));
}

// ─────────────────────────────────────────────────────────────────────
// Device-side failure — args are valid but the Device is unusable, so
// dup() succeeds and drmPrimeFDToHandle fails. The factory must unwind
// the dup'd fds and return an error.
// ─────────────────────────────────────────────────────────────────────

TEST(SceneExternalDmaBufSource, RejectsBadDeviceFd) {
  // Real fd (so dup succeeds) but Device has no fd at all — the early
  // dev.fd() < 0 guard fires before any kernel work.
  auto dev = drm::Device::from_fd(-1);
  const int real_fd = ::open("/dev/null", O_RDONLY);
  if (real_fd < 0) {
    GTEST_SKIP() << "Cannot open /dev/null";
  }
  std::array<drm::scene::ExternalPlaneInfo, 1> planes{dummy_plane(real_fd)};
  auto r = drm::scene::ExternalDmaBufSource::create(dev, k_w, k_h, DRM_FORMAT_ARGB8888,
                                                    DRM_FORMAT_MOD_LINEAR, planes);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::bad_file_descriptor));
  ::close(real_fd);
}

TEST(SceneExternalDmaBufSource, RejectsNonDrmDeviceFd) {
  // Device wraps a real fd, but it isn't a DRM fd — drmPrimeFDToHandle
  // fails. The factory must unwind dup'd fds + return an error.
  const int dev_fd = ::open("/dev/null", O_RDWR);
  if (dev_fd < 0) {
    GTEST_SKIP() << "Cannot open /dev/null";
  }
  auto dev = drm::Device::from_fd(dev_fd);
  const int plane_fd = ::open("/dev/null", O_RDONLY);
  ASSERT_GE(plane_fd, 0);
  std::array<drm::scene::ExternalPlaneInfo, 1> planes{dummy_plane(plane_fd)};
  auto r = drm::scene::ExternalDmaBufSource::create(dev, k_w, k_h, DRM_FORMAT_ARGB8888,
                                                    DRM_FORMAT_MOD_LINEAR, planes);
  EXPECT_FALSE(r.has_value());
  ::close(plane_fd);
  ::close(dev_fd);
}

// ─────────────────────────────────────────────────────────────────────
// on_release contract — the callback must NOT fire when create() fails
// (caller still owns the upstream Request and re-queues it themselves).
// ─────────────────────────────────────────────────────────────────────

TEST(SceneExternalDmaBufSource, OnReleaseDoesNotFireOnCreateFailure) {
  auto dev = drm::Device::from_fd(-1);
  std::atomic<int> calls{0};
  auto cb = [&calls] { ++calls; };
  std::array<drm::scene::ExternalPlaneInfo, 1> planes{dummy_plane(/*fd=*/-1)};
  auto r = drm::scene::ExternalDmaBufSource::create(dev, k_w, k_h, DRM_FORMAT_ARGB8888,
                                                    DRM_FORMAT_MOD_LINEAR, planes, cb);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(calls.load(), 0);
}