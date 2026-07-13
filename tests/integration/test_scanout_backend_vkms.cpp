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
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <cstdlib>
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

// A non-null out_fence is filled in with this commit's OUT_FENCE on a CRTC that
// advertises OUT_FENCE_PTR (vkms does, for flips). Guards the public-caller path
// in do_commit: with no internal release-fence consumer (GbmScanoutProducer is a
// plain source) the captured fence is moved straight into *out_fence.
TEST(ScanoutBackendVkms, PresentDeliversOutFence) {
  const auto path = find_vkms_node();
  if (!path.has_value()) {
    GTEST_SKIP() << "vkms not loaded; modprobe vkms enable_overlay=1.";
  }

  auto dev = drm::Device::open(*path);
  ASSERT_TRUE(dev.has_value()) << "Device::open: " << dev.error().message();

  drm::present::GbmScanoutProducer producer(*dev);
  auto backend = drm::present::ScanoutBackend::create(*dev, producer);
  ASSERT_TRUE(backend.has_value()) << "ScanoutBackend::create: " << backend.error().message();

  // First present brings the CRTC up (modeset). Assert the out_fence on the
  // second (a flip), which is the steady-state buffer-reuse case the fence is for.
  drm::sync::SyncFence first_fence;
  auto first = (*backend)->present(0, &first_fence);
  ASSERT_TRUE(first.has_value()) << "present: " << first.error().message();

  drm::sync::SyncFence flip_fence;
  auto flip = (*backend)->present(0, &flip_fence);
  ASSERT_TRUE(flip.has_value()) << "present: " << flip.error().message();
  EXPECT_TRUE(flip_fence.valid())
      << "public out_fence must be delivered when the CRTC advertises OUT_FENCE_PTR";
}

TEST(ScanoutBackendVkms, VrrAutoArmsFromProfileAndCommits) {
  const auto path = find_vkms_node();
  if (!path.has_value()) {
    GTEST_SKIP() << "vkms not loaded; modprobe vkms enable_overlay=1.";
  }

  auto dev = drm::Device::open(*path);
  ASSERT_TRUE(dev.has_value()) << "Device::open: " << dev.error().message();

  // Auto drives VRR from the driver profile. vkms's CRTC advertises VRR_ENABLED,
  // so Auto arms it -- and the resulting commit (which the scene tags with
  // ALLOW_MODESET on the enable transition) must still land.
  drm::present::GbmScanoutProducer producer(*dev);
  drm::present::ScanoutBackend::Config cfg;
  cfg.vrr = drm::present::ScanoutBackend::VrrPolicy::Auto;
  auto backend = drm::present::ScanoutBackend::create(*dev, producer, cfg);
  ASSERT_TRUE(backend.has_value()) << "create: " << backend.error().message();

  // vrr_capable() mirrors the discovered profile.
  EXPECT_EQ((*backend)->vrr_capable(), (*backend)->profile().vrr_capable);

  auto r1 = (*backend)->present(0);
  ASSERT_TRUE(r1.has_value()) << "present (vrr Auto): " << r1.error().message();

  // Runtime toggles: disarm then re-arm; each change commits cleanly (the enable
  // transition again ORs ALLOW_MODESET in the scene).
  (*backend)->set_vrr(false);
  auto r2 = (*backend)->present(0);
  ASSERT_TRUE(r2.has_value()) << "present (vrr off): " << r2.error().message();

  (*backend)->set_vrr(true);
  auto r3 = (*backend)->present(0);
  ASSERT_TRUE(r3.has_value()) << "present (vrr on): " << r3.error().message();
}

// RestorePolicy::SavedCrtc snapshots the CRTC at create() and puts it back when
// the backend is destroyed, so a display board returns to its prior state (fbcon
// / the previous framebuffer) instead of freezing on the last committed frame.
//
// Observed via the legacy CRTC framebuffer (`drmModeGetCrtc::buffer_id`): the
// backend commits atomically, which does NOT update that legacy field, so this
// test drives it directly with legacy modesets. Establish a known framebuffer
// on the CRTC, create the backend (which snapshots it), disable the CRTC, then
// destroy the backend and confirm SavedCrtc reprogrammed the original.
TEST(ScanoutBackendVkms, SavedCrtcRestoresCrtcOnTeardown) {
  const char* env = std::getenv("DRM_CXX_TEST_CARD");
  const std::optional<std::string> path =
      (env != nullptr && *env != '\0') ? std::optional<std::string>(env) : find_vkms_node();
  if (!path.has_value()) {
    GTEST_SKIP() << "vkms not loaded (or set DRM_CXX_TEST_CARD to a modeset card).";
  }

  auto dev = drm::Device::open(*path);
  ASSERT_TRUE(dev.has_value()) << "Device::open: " << dev.error().message();
  const int fd = dev->fd();

  // Find a connected connector + a usable CRTC + its first mode.
  drmModeRes* res = drmModeGetResources(fd);
  ASSERT_NE(res, nullptr);
  std::uint32_t crtc_id = 0;
  std::uint32_t connector_id = 0;
  drmModeModeInfo mode{};
  for (int i = 0; i < res->count_connectors && crtc_id == 0; ++i) {
    drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[i]);
    if (conn == nullptr) {
      continue;
    }
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0 && res->count_crtcs > 0) {
      connector_id = conn->connector_id;
      crtc_id = res->crtcs[0];
      mode = conn->modes[0];
    }
    drmModeFreeConnector(conn);
  }
  drmModeFreeResources(res);
  if (crtc_id == 0) {
    GTEST_SKIP() << "no connected connector with a mode";
  }

  // A dumb framebuffer to stand in for the prior on-screen contents (fbcon).
  // DumbBufferSource owns the buffer + FB; keep it alive for the whole test.
  auto prior =
      drm::scene::DumbBufferSource::create(*dev, mode.hdisplay, mode.vdisplay, DRM_FORMAT_XRGB8888);
  ASSERT_TRUE(prior.has_value()) << "dumb source: " << prior.error().message();
  auto prior_buf = (*prior)->acquire();
  ASSERT_TRUE(prior_buf.has_value()) << "acquire: " << prior_buf.error().message();
  const std::uint32_t prior_fb = prior_buf->fb_id;
  ASSERT_NE(prior_fb, 0U);

  // Establish the known prior CRTC state via a legacy modeset.
  ASSERT_EQ(drmModeSetCrtc(fd, crtc_id, prior_fb, 0, 0, &connector_id, 1, &mode), 0);
  {
    drmModeCrtcPtr c = drmModeGetCrtc(fd, crtc_id);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->buffer_id, prior_fb) << "prior legacy modeset did not take";
    drmModeFreeCrtc(c);
  }

  drm::present::GbmScanoutProducer producer(*dev);
  drm::present::ScanoutBackend::Config cfg;
  cfg.restore = drm::present::ScanoutBackend::RestorePolicy::SavedCrtc;
  auto backend = drm::present::ScanoutBackend::create(*dev, producer, cfg);
  ASSERT_TRUE(backend.has_value()) << "create: " << backend.error().message();
  ASSERT_EQ((*backend)->target().crtc_id, crtc_id);

  // Take the CRTC away from its prior contents (disable it) so the restore is
  // observable rather than a no-op.
  ASSERT_EQ(drmModeSetCrtc(fd, crtc_id, 0, 0, 0, nullptr, 0, nullptr), 0);
  {
    drmModeCrtcPtr c = drmModeGetCrtc(fd, crtc_id);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->buffer_id, 0U) << "CRTC should be disabled before teardown";
    drmModeFreeCrtc(c);
  }

  // Destroy the backend: SavedCrtc must reprogram the framebuffer captured at
  // create() time.
  (*backend).reset();
  drmModeCrtcPtr after = drmModeGetCrtc(fd, crtc_id);
  ASSERT_NE(after, nullptr);
  const std::uint32_t restored_fb = after->buffer_id;
  drmModeFreeCrtc(after);
  EXPECT_EQ(restored_fb, prior_fb) << "SavedCrtc must restore the prior CRTC framebuffer";

  drmModeSetCrtc(fd, crtc_id, 0, 0, 0, nullptr, 0, nullptr);  // leave the CRTC clean
}
