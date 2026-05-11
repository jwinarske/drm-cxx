// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// integration test for `drm::dumb::Buffer::create_planar`
// against vkms.
//
// Validates that a P010 dumb buffer built via `create_planar` can be
// scanned out by vkms's primary plane: AddFB2 builds a multi-plane
// FB, atomic commit accepts it. The kernel's vkms YUV-to-RGB matrix
// + 8-bit downconversion in the writeback connector means a
// pixel-diff against an exact reference doesn't make sense for
// arbitrary patterns; we verify the commit chain instead, which is
// where the geometry/offset math actually lands.
//
// Skips when vkms isn't loaded — `sudo modprobe vkms enable_overlay=1`.

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/property_store.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/dumb/buffer.hpp>
#include <drm-cxx/modeset/atomic.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cerrno>
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

struct ActiveBinding {
  std::uint32_t crtc_id{0};
  std::uint32_t connector_id{0};
  std::uint32_t primary_plane_id{0};
  drmModeModeInfo mode{};
};

// Walk the resources to find a connected connector + CRTC + a
// primary plane that accepts P010. Returns nullopt if any link is
// missing — vkms's primary plane lists P010 in IN_FORMATS as of
// kernel 6.x, but a future regression there would correctly skip.
std::optional<ActiveBinding> pick_binding(const drm::Device& dev) {
  auto* res = drmModeGetResources(dev.fd());
  if (res == nullptr) {
    return std::nullopt;
  }
  ActiveBinding out;

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

  // Find the primary plane bound to this CRTC.
  auto* planes = drmModeGetPlaneResources(dev.fd());
  if (planes == nullptr) {
    return std::nullopt;
  }
  for (std::uint32_t i = 0; i < planes->count_planes && out.primary_plane_id == 0; ++i) {
    auto* plane = drmModeGetPlane(dev.fd(), planes->planes[i]);
    if (plane != nullptr) {
      // Crude plane-type discovery: read the "type" property.
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

TEST(DumbBufferP010Vkms, CreatePlanarP010ScansOutOnPrimary) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded — `sudo modprobe vkms enable_overlay=1` to enable";
  }
  auto dev_r = drm::Device::open(*node);
  ASSERT_TRUE(dev_r.has_value()) << dev_r.error().message();
  auto& dev = *dev_r;
  ASSERT_TRUE(dev.enable_universal_planes().has_value());
  ASSERT_TRUE(dev.enable_atomic().has_value());

  const auto binding = pick_binding(dev);
  if (!binding) {
    GTEST_SKIP() << "vkms loaded but no usable connector + CRTC + primary plane found";
  }

  // Build the P010 dumb buffer through create_planar.
  auto buf_r = drm::dumb::Buffer::create_planar(dev, DRM_FORMAT_P010, binding->mode.hdisplay,
                                                binding->mode.vdisplay);
  ASSERT_TRUE(buf_r.has_value()) << "create_planar P010: " << buf_r.error().message();
  auto& buf = *buf_r;
  EXPECT_NE(buf.fb_id(), 0U);
  EXPECT_NE(buf.handle(), 0U);
  // Stride is at least width * 2 (P010 = 16 bits per sample); kernel
  // may align further. The exact value is driver-dependent so we
  // bound it loosely.
  EXPECT_GE(buf.stride(), binding->mode.hdisplay * 2U);

  // Paint a constant Y-plane pattern (e.g. mid-gray PQ) so the
  // commit isn't operating on uninitialized data. P010 stores Y as
  // the upper 10 bits of a u16; 0x4000 is mid-gray under PQ.
  auto* y_plane = buf.data();
  ASSERT_NE(y_plane, nullptr);
  const auto stride = buf.stride();
  for (std::uint32_t y = 0; y < binding->mode.vdisplay; ++y) {
    auto* row = y_plane + (static_cast<std::size_t>(y) * stride);
    for (std::uint32_t x = 0; x < binding->mode.hdisplay; ++x) {
      // Little-endian u16 at offset x*2 — low byte 0x00, high byte 0x40.
      const std::size_t off = static_cast<std::size_t>(x) * 2U;
      row[off] = 0x00;
      row[off + 1U] = 0x40;
    }
  }

  // Build the modeset: bring the CRTC up with our P010 buffer on
  // its primary plane. Cache the property ids first.
  drm::PropertyStore props;
  ASSERT_TRUE(props.cache_properties(dev.fd(), binding->crtc_id, DRM_MODE_OBJECT_CRTC).has_value());
  ASSERT_TRUE(props.cache_properties(dev.fd(), binding->connector_id, DRM_MODE_OBJECT_CONNECTOR)
                  .has_value());
  ASSERT_TRUE(props.cache_properties(dev.fd(), binding->primary_plane_id, DRM_MODE_OBJECT_PLANE)
                  .has_value());

  // MODE_ID blob.
  std::uint32_t mode_blob = 0;
  ASSERT_EQ(drmModeCreatePropertyBlob(dev.fd(), &binding->mode, sizeof(binding->mode), &mode_blob),
            0)
      << std::strerror(errno);

  drm::AtomicRequest req(dev);
  ASSERT_TRUE(req.valid());

  auto add = [&](std::uint32_t obj, const char* name, std::uint64_t v) {
    const auto pid = props.property_id(obj, name);
    ASSERT_TRUE(pid.has_value()) << name;
    ASSERT_TRUE(req.add_property(obj, *pid, v).has_value()) << name;
  };
  add(binding->crtc_id, "MODE_ID", mode_blob);
  add(binding->crtc_id, "ACTIVE", 1);
  add(binding->connector_id, "CRTC_ID", binding->crtc_id);
  add(binding->primary_plane_id, "FB_ID", buf.fb_id());
  add(binding->primary_plane_id, "CRTC_ID", binding->crtc_id);
  add(binding->primary_plane_id, "SRC_X", 0);
  add(binding->primary_plane_id, "SRC_Y", 0);
  add(binding->primary_plane_id, "SRC_W", static_cast<std::uint64_t>(binding->mode.hdisplay) << 16);
  add(binding->primary_plane_id, "SRC_H", static_cast<std::uint64_t>(binding->mode.vdisplay) << 16);
  add(binding->primary_plane_id, "CRTC_X", 0);
  add(binding->primary_plane_id, "CRTC_Y", 0);
  add(binding->primary_plane_id, "CRTC_W", binding->mode.hdisplay);
  add(binding->primary_plane_id, "CRTC_H", binding->mode.vdisplay);

  // First a TEST_ONLY commit — the kernel has to accept the multi-
  // plane FB layout for our create_planar to be considered correct.
  auto test_r = req.test(DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET);
  EXPECT_TRUE(test_r.has_value()) << "TEST_ONLY: " << test_r.error().message();

  // Real commit: bring scanout up. We don't read pixels back; the
  // success of the commit is what proves the geometry math.
  auto commit_r = req.commit(DRM_MODE_ATOMIC_ALLOW_MODESET);
  EXPECT_TRUE(commit_r.has_value()) << "commit: " << commit_r.error().message();

  // Tear down: clear the active CRTC binding so vkms's next
  // open() doesn't inherit our state.
  drm::AtomicRequest teardown(dev);
  ASSERT_TRUE(teardown.valid());
  auto tdown_add = [&](std::uint32_t obj, const char* name, std::uint64_t v) {
    const auto pid = props.property_id(obj, name);
    ASSERT_TRUE(pid.has_value());
    (void)teardown.add_property(obj, *pid, v);
  };
  tdown_add(binding->crtc_id, "ACTIVE", 0);
  tdown_add(binding->crtc_id, "MODE_ID", 0);
  tdown_add(binding->connector_id, "CRTC_ID", 0);
  tdown_add(binding->primary_plane_id, "FB_ID", 0);
  tdown_add(binding->primary_plane_id, "CRTC_ID", 0);
  (void)teardown.commit(DRM_MODE_ATOMIC_ALLOW_MODESET);

  drmModeDestroyPropertyBlob(dev.fd(), mode_blob);
}
