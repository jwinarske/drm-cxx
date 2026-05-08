// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// integration test: drive a CRTC color pipeline blob through
// vkms's atomic state machine. vkms exposes GAMMA_LUT (256 entries)
// but not DEGAMMA / CTM, so the test exercises the GAMMA half plus
// the apply() property write. Tests for full DEGAMMA / CTM / GAMMA
// pipelines need amdgpu / i915 hardware and run as out-of-tree
// smoke for now.

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/property_store.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/display/crtc_capabilities.hpp>
#include <drm-cxx/display/crtc_color_pipeline.hpp>
#include <drm-cxx/dumb/buffer.hpp>
#include <drm-cxx/modeset/atomic.hpp>

#include <drm/drm_mode.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::optional<std::string> find_vkms_node() {
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/dev/dri", ec)) {
    const auto& p = entry.path();
    if (p.filename().string().rfind("card", 0) != 0) {
      continue;
    }
    const int fd = ::open(p.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    drmVersionPtr v = drmGetVersion(fd);
    const bool is_vkms =
        (v != nullptr) && (v->name != nullptr) && (std::strcmp(v->name, "vkms") == 0);
    if (v != nullptr) {
      drmFreeVersion(v);
    }
    ::close(fd);
    if (is_vkms) {
      return p.string();
    }
  }
  return std::nullopt;
}

struct ActiveCrtc {
  std::uint32_t crtc_id{0};
  std::uint32_t connector_id{0};
  std::uint32_t primary_plane_id{0};
  drmModeModeInfo mode{};
};

std::optional<ActiveCrtc> pick_crtc(const drm::Device& dev) {
  auto* res = drmModeGetResources(dev.fd());
  if (res == nullptr) {
    return std::nullopt;
  }
  ActiveCrtc out;
  for (int i = 0; i < res->count_connectors && out.crtc_id == 0; ++i) {
    auto* conn = drmModeGetConnector(dev.fd(), res->connectors[i]);
    if (conn != nullptr && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
      for (int e = 0; e < conn->count_encoders && out.crtc_id == 0; ++e) {
        auto* enc = drmModeGetEncoder(dev.fd(), conn->encoders[e]);
        if (enc != nullptr) {
          for (int c = 0; c < res->count_crtcs && out.crtc_id == 0; ++c) {
            if ((enc->possible_crtcs & (1U << static_cast<unsigned>(c))) != 0U) {
              out.crtc_id = res->crtcs[c];
              out.connector_id = conn->connector_id;
              out.mode = conn->modes[0];
            }
          }
          drmModeFreeEncoder(enc);
        }
      }
    }
    if (conn != nullptr) {
      drmModeFreeConnector(conn);
    }
  }
  drmModeFreeResources(res);
  if (out.crtc_id == 0) {
    return std::nullopt;
  }

  auto* planes = drmModeGetPlaneResources(dev.fd());
  if (planes == nullptr) {
    return std::nullopt;
  }
  for (std::uint32_t i = 0; i < planes->count_planes && out.primary_plane_id == 0; ++i) {
    auto* plane = drmModeGetPlane(dev.fd(), planes->planes[i]);
    if (plane != nullptr) {
      auto* props = drmModeObjectGetProperties(dev.fd(), plane->plane_id, DRM_MODE_OBJECT_PLANE);
      if (props != nullptr) {
        for (std::uint32_t pi = 0; pi < props->count_props; ++pi) {
          auto* p = drmModeGetProperty(dev.fd(), props->props[pi]);
          if (p != nullptr) {
            if (std::strcmp(p->name, "type") == 0 &&
                props->prop_values[pi] == DRM_PLANE_TYPE_PRIMARY) {
              out.primary_plane_id = plane->plane_id;
            }
            drmModeFreeProperty(p);
          }
        }
        drmModeFreeObjectProperties(props);
      }
      drmModeFreePlane(plane);
    }
  }
  drmModeFreePlaneResources(planes);
  if (out.primary_plane_id == 0) {
    return std::nullopt;
  }
  return out;
}

}  // namespace

TEST(CrtcColorPipelineVkms, ProbeReportsGammaOnly) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded";
  }
  auto dev_r = drm::Device::open(*node);
  ASSERT_TRUE(dev_r.has_value());
  auto& dev = *dev_r;
  ASSERT_TRUE(dev.enable_atomic().has_value());

  const auto active = pick_crtc(dev);
  if (!active) {
    GTEST_SKIP() << "vkms loaded but no active CRTC";
  }

  const auto caps = drm::display::probe_crtc_capabilities(dev, active->crtc_id);
  ASSERT_TRUE(caps.has_value()) << caps.error().message();
  EXPECT_TRUE(caps->has_gamma_lut);
  EXPECT_GT(caps->gamma_lut_size, 0U) << "vkms advertises a non-zero GAMMA_LUT_SIZE";
  // vkms has no DEGAMMA or CTM as of kernel 6.x.
  EXPECT_FALSE(caps->has_degamma_lut);
  EXPECT_FALSE(caps->has_ctm);
  EXPECT_FALSE(caps->has_full_pipeline());
}

TEST(CrtcColorPipelineVkms, IdentityGammaApplyAndCommit) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded";
  }
  auto dev_r = drm::Device::open(*node);
  ASSERT_TRUE(dev_r.has_value());
  auto& dev = *dev_r;
  ASSERT_TRUE(dev.enable_universal_planes().has_value());
  ASSERT_TRUE(dev.enable_atomic().has_value());

  const auto active = pick_crtc(dev);
  if (!active) {
    GTEST_SKIP() << "vkms loaded but no usable CRTC + connector + primary plane";
  }

  // Build a primary-plane FB so the modeset commit has something
  // to scan out — vkms rejects ACTIVE=1 + no FB on primary.
  drm::dumb::Config cfg;
  cfg.width = active->mode.hdisplay;
  cfg.height = active->mode.vdisplay;
  cfg.drm_format = DRM_FORMAT_XRGB8888;
  cfg.bpp = 32;
  cfg.add_fb = true;
  auto fb_r = drm::dumb::Buffer::create(dev, cfg);
  ASSERT_TRUE(fb_r.has_value()) << "fb create: " << fb_r.error().message();

  auto pipe_r = drm::display::CrtcColorPipeline::create(dev, active->crtc_id);
  ASSERT_TRUE(pipe_r.has_value()) << "pipeline create: " << pipe_r.error().message();
  auto& pipe = *pipe_r;

  // set_identity touches only stages the CRTC exposes — on vkms
  // that's GAMMA only.
  ASSERT_TRUE(pipe.set_identity().has_value());
  EXPECT_EQ(pipe.degamma_blob_id(), 0U) << "vkms has no DEGAMMA — should remain unset";
  EXPECT_EQ(pipe.ctm_blob_id(), 0U) << "vkms has no CTM";
  EXPECT_NE(pipe.gamma_blob_id(), 0U) << "GAMMA blob must be created";

  // Build a real modeset commit that includes the GAMMA blob via apply().
  drm::PropertyStore props;
  ASSERT_TRUE(props.cache_properties(dev.fd(), active->crtc_id, DRM_MODE_OBJECT_CRTC).has_value());
  ASSERT_TRUE(props.cache_properties(dev.fd(), active->connector_id, DRM_MODE_OBJECT_CONNECTOR)
                  .has_value());
  ASSERT_TRUE(props.cache_properties(dev.fd(), active->primary_plane_id, DRM_MODE_OBJECT_PLANE)
                  .has_value());

  std::uint32_t mode_blob = 0;
  ASSERT_EQ(drmModeCreatePropertyBlob(dev.fd(), &active->mode, sizeof(active->mode), &mode_blob),
            0);

  drm::AtomicRequest req(dev);
  ASSERT_TRUE(req.valid());
  auto add = [&](std::uint32_t obj, const char* name, std::uint64_t v) {
    const auto pid = props.property_id(obj, name);
    ASSERT_TRUE(pid.has_value()) << name;
    ASSERT_TRUE(req.add_property(obj, *pid, v).has_value()) << name;
  };
  add(active->crtc_id, "MODE_ID", mode_blob);
  add(active->crtc_id, "ACTIVE", 1);
  add(active->connector_id, "CRTC_ID", active->crtc_id);
  add(active->primary_plane_id, "FB_ID", fb_r->fb_id());
  add(active->primary_plane_id, "CRTC_ID", active->crtc_id);
  add(active->primary_plane_id, "SRC_X", 0);
  add(active->primary_plane_id, "SRC_Y", 0);
  add(active->primary_plane_id, "SRC_W", static_cast<std::uint64_t>(active->mode.hdisplay) << 16);
  add(active->primary_plane_id, "SRC_H", static_cast<std::uint64_t>(active->mode.vdisplay) << 16);
  add(active->primary_plane_id, "CRTC_X", 0);
  add(active->primary_plane_id, "CRTC_Y", 0);
  add(active->primary_plane_id, "CRTC_W", active->mode.hdisplay);
  add(active->primary_plane_id, "CRTC_H", active->mode.vdisplay);

  // Pipeline-side property writes go through the same AtomicRequest.
  ASSERT_TRUE(pipe.apply(req).has_value());

  // TEST_ONLY first; the kernel validates the LUT blob here.
  EXPECT_TRUE(req.test(DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET).has_value());
  EXPECT_TRUE(req.commit(DRM_MODE_ATOMIC_ALLOW_MODESET).has_value());

  // Tear down.
  drm::AtomicRequest teardown(dev);
  ASSERT_TRUE(teardown.valid());
  auto td_add = [&](std::uint32_t obj, const char* name, std::uint64_t v) {
    const auto pid = props.property_id(obj, name);
    ASSERT_TRUE(pid.has_value());
    (void)teardown.add_property(obj, *pid, v);
  };
  td_add(active->crtc_id, "ACTIVE", 0);
  td_add(active->crtc_id, "MODE_ID", 0);
  td_add(active->connector_id, "CRTC_ID", 0);
  td_add(active->primary_plane_id, "FB_ID", 0);
  td_add(active->primary_plane_id, "CRTC_ID", 0);
  (void)teardown.commit(DRM_MODE_ATOMIC_ALLOW_MODESET);
  drmModeDestroyPropertyBlob(dev.fd(), mode_blob);
}

TEST(CrtcColorPipelineVkms, RejectsDegammaWhenNotExposed) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded";
  }
  auto dev_r = drm::Device::open(*node);
  ASSERT_TRUE(dev_r.has_value());
  auto& dev = *dev_r;
  ASSERT_TRUE(dev.enable_atomic().has_value());

  const auto active = pick_crtc(dev);
  if (!active) {
    GTEST_SKIP();
  }
  auto pipe_r = drm::display::CrtcColorPipeline::create(dev, active->crtc_id);
  ASSERT_TRUE(pipe_r.has_value());
  auto& pipe = *pipe_r;

  // vkms doesn't expose DEGAMMA / CTM — calls targeting them
  // surface as operation_not_supported.
  EXPECT_EQ(pipe.set_pq_to_linear().error(),
            std::make_error_code(std::errc::operation_not_supported));
  EXPECT_EQ(pipe.set_hlg_to_linear().error(),
            std::make_error_code(std::errc::operation_not_supported));
  EXPECT_EQ(pipe.set_bt2020_to_bt709().error(),
            std::make_error_code(std::errc::operation_not_supported));
}
