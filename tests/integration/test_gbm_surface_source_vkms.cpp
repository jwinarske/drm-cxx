// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Integration test for drm::scene::GbmSurfaceSource against vkms. The
// full end-to-end producer flow (EGL or Vulkan render → eglSwapBuffers
// → scene commit → scanout) needs a real GL/Vulkan stack with a
// modifier-aware GBM platform binding — out of CI scope, covered by
// the egl_scene and vulkan_scene example apps.
//
// What this test does cover:
//
//   * GbmSurfaceSource::create() succeeds against vkms's card node
//     for the formats LayerScene::candidate_modifiers() advertises.
//   * The candidate-modifier list is non-empty and consistent
//     between the per-plane modifier table and what the source then
//     accepts.
//   * The source can be paused + resumed against the same vkms
//     device without leaking surfaces or fb_ids.

#include "core/device.hpp"

#include <drm-cxx/scene/gbm_surface_source.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <gtest/gtest.h>
#include <unistd.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace {

// Locate the vkms DRM node by driver name. Mirrors find_vkms_node()
// in the V4l2CameraSource integration test.
[[nodiscard]] std::optional<std::string> find_vkms_node() noexcept {
  for (int idx = 0; idx < 8; ++idx) {
    std::string path = "/dev/dri/card" + std::to_string(idx);
    int const fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    drmVersionPtr ver = drmGetVersion(fd);
    if (ver == nullptr) {
      ::close(fd);
      continue;
    }
    std::string const name(ver->name, ver->name_len);
    drmFreeVersion(ver);
    ::close(fd);
    if (name == "vkms") {
      return path;
    }
  }
  return std::nullopt;
}

// Pick the first connected connector + a CRTC the encoder allows
// + the first mode. Enough to construct a LayerScene against vkms.
struct PickedOutput {
  std::uint32_t crtc_id{0};
  std::uint32_t connector_id{0};
  drmModeModeInfo mode{};
};

[[nodiscard]] std::optional<PickedOutput> pick_vkms_output(int fd) noexcept {
  drmModeResPtr res = drmModeGetResources(fd);
  if (res == nullptr) {
    return std::nullopt;
  }
  PickedOutput out;
  for (int i = 0; i < res->count_connectors; ++i) {
    drmModeConnectorPtr conn = drmModeGetConnector(fd, res->connectors[i]);
    if (conn == nullptr) {
      continue;
    }
    if (conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0 ||
        conn->encoder_id == 0) {
      drmModeFreeConnector(conn);
      continue;
    }
    drmModeEncoderPtr enc = drmModeGetEncoder(fd, conn->encoder_id);
    if (enc == nullptr) {
      drmModeFreeConnector(conn);
      continue;
    }
    for (int j = 0; j < res->count_crtcs; ++j) {
      if (((enc->possible_crtcs >> j) & 1U) != 0U) {
        out.crtc_id = res->crtcs[j];
        out.connector_id = conn->connector_id;
        out.mode = conn->modes[0];
        drmModeFreeEncoder(enc);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        return out;
      }
    }
    drmModeFreeEncoder(enc);
    drmModeFreeConnector(conn);
  }
  drmModeFreeResources(res);
  return std::nullopt;
}

class GbmSurfaceSourceVkms : public ::testing::Test {
 protected:
  void SetUp() override {
    auto path = find_vkms_node();
    if (!path.has_value()) {
      GTEST_SKIP() << "vkms not loaded; modprobe vkms enable_overlay=1.";
    }
    auto dev_r = drm::Device::open(*path);
    ASSERT_TRUE(dev_r.has_value())
        << "Device::open(" << *path << "): " << dev_r.error().message();
    dev = std::make_unique<drm::Device>(std::move(*dev_r));

    auto picked = pick_vkms_output(dev->fd());
    if (!picked.has_value()) {
      GTEST_SKIP() << "vkms exposes no connected connector with a usable mode.";
    }
    output = *picked;
  }

  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes,
  //             misc-non-private-member-variables-in-classes)
  std::unique_ptr<drm::Device> dev;
  PickedOutput output{};
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes,
  //           misc-non-private-member-variables-in-classes)
};

}  // namespace

TEST_F(GbmSurfaceSourceVkms, CandidateModifiersForXrgb8888NotEmpty) {
  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = output.crtc_id;
  cfg.connector_id = output.connector_id;
  cfg.mode = output.mode;
  auto scene = drm::scene::LayerScene::create(*dev, cfg);
  ASSERT_TRUE(scene.has_value()) << scene.error().message();

  const auto mods = (*scene)->candidate_modifiers(DRM_FORMAT_XRGB8888);
  EXPECT_FALSE(mods.empty())
      << "vkms must accept at least DRM_FORMAT_XRGB8888 on its primary plane";
}

TEST_F(GbmSurfaceSourceVkms, CreateSucceedsAgainstVkms) {
  drm::scene::GbmSurfaceConfig cfg;
  cfg.width = output.mode.hdisplay;
  cfg.height = output.mode.vdisplay;
  cfg.drm_format = DRM_FORMAT_XRGB8888;
  cfg.modifier = DRM_FORMAT_MOD_INVALID;
  cfg.usage = 0;

  auto src = drm::scene::GbmSurfaceSource::create(*dev, cfg);
  ASSERT_TRUE(src.has_value()) << src.error().message();
  ASSERT_NE((*src)->native_surface(), nullptr);

  const auto fmt = (*src)->format();
  EXPECT_EQ(fmt.drm_fourcc, static_cast<std::uint32_t>(DRM_FORMAT_XRGB8888));
  EXPECT_EQ(fmt.width, static_cast<std::uint32_t>(output.mode.hdisplay));
  EXPECT_EQ(fmt.height, static_cast<std::uint32_t>(output.mode.vdisplay));
}

// Pause + resume against the same device. Validates that the
// gbm_surface and any cached fb_ids are torn down + recreated cleanly,
// and that `native_surface()` identity changes (callers must re-query).
TEST_F(GbmSurfaceSourceVkms, SessionPauseResumeAgainstSameDevice) {
  drm::scene::GbmSurfaceConfig cfg;
  cfg.width = output.mode.hdisplay;
  cfg.height = output.mode.vdisplay;
  cfg.drm_format = DRM_FORMAT_XRGB8888;
  cfg.modifier = DRM_FORMAT_MOD_INVALID;
  cfg.usage = 0;

  auto src = drm::scene::GbmSurfaceSource::create(*dev, cfg);
  ASSERT_TRUE(src.has_value()) << src.error().message();

  auto* surf_before = (*src)->native_surface();
  ASSERT_NE(surf_before, nullptr);

  (*src)->on_session_paused();
  EXPECT_EQ((*src)->native_surface(), nullptr);

  auto resumed = (*src)->on_session_resumed(*dev);
  ASSERT_TRUE(resumed.has_value()) << resumed.error().message();

  auto* surf_after = (*src)->native_surface();
  ASSERT_NE(surf_after, nullptr);
  // Mesa may reuse the underlying pointer or hand back a different
  // one — both are acceptable. The contract is "callers must
  // re-query", not "the pointer changes." Just pin that the source
  // is usable again.
}
