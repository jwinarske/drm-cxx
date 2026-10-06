// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// A display controller that lights fewer planes at once than it advertises
// (RK3566 VOP2: three eligible, two usable) refuses any commit that arms one
// more. vkms has no such limit, so this executable imposes one: it defines
// drmModeAtomicCommit, which libdrm-cxx then binds to (ELF interposition, as
// test_seat.cpp does for libseat), and refuses with EINVAL any commit -- TEST
// or real -- that would leave more than g_limit planes armed on the CRTC.
//
// The composition canvas is the plane that tips such a frame over: the layer
// assignment passes TEST, then the canvas joins it. The scene must notice and
// move another layer into the composition rather than commit a frame the
// kernel refuses.

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/capture/snapshot.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;
using drm::Device;
using drm::capture::snapshot;
using drm::scene::DumbBufferSource;
using drm::scene::LayerDesc;
using drm::scene::LayerScene;

namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::uint32_t g_crtc = 0;   // CRTC the limit applies to
std::size_t g_limit = 0;    // 0: no limit
std::size_t g_refused = 0;  // commits refused for exceeding it
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// libdrm's request layout (xf86drmMode.c), unchanged since atomic landed.
struct ReqItem {
  std::uint32_t object_id;
  std::uint32_t property_id;
  std::uint64_t value;
  std::uint32_t cursor;
};
struct Req {
  std::uint32_t cursor;
  std::uint32_t size_items;
  ReqItem* items;
};

std::optional<std::string> prop_name(int fd, std::uint32_t object, std::uint32_t prop) {
  drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, object, DRM_MODE_OBJECT_PLANE);
  if (props == nullptr) {
    return std::nullopt;
  }
  std::optional<std::string> name;
  for (std::uint32_t i = 0; i < props->count_props && !name.has_value(); ++i) {
    if (props->props[i] == prop) {
      if (drmModePropertyPtr p = drmModeGetProperty(fd, prop); p != nullptr) {
        name = p->name;
        drmModeFreeProperty(p);
      }
    }
  }
  drmModeFreeObjectProperties(props);
  return name;
}

// Planes armed on g_crtc once `req` applies on top of the current state.
std::size_t armed_after(int fd, const Req& req) {
  struct State {
    std::uint32_t fb;
    std::uint32_t crtc;
  };
  std::map<std::uint32_t, State> planes;
  if (drmModePlaneResPtr res = drmModeGetPlaneResources(fd); res != nullptr) {
    for (std::uint32_t i = 0; i < res->count_planes; ++i) {
      if (drmModePlanePtr p = drmModeGetPlane(fd, res->planes[i]); p != nullptr) {
        planes[p->plane_id] = {p->fb_id, p->crtc_id};
        drmModeFreePlane(p);
      }
    }
    drmModeFreePlaneResources(res);
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  for (const ReqItem* it = req.items; it != req.items + req.cursor; ++it) {
    auto found = planes.find(it->object_id);
    if (found == planes.end()) {
      continue;  // not a plane
    }
    const auto name = prop_name(fd, it->object_id, it->property_id);
    if (name == "FB_ID") {
      found->second.fb = static_cast<std::uint32_t>(it->value);
    } else if (name == "CRTC_ID") {
      found->second.crtc = static_cast<std::uint32_t>(it->value);
    }
  }
  std::size_t armed = 0;
  for (const auto& [id, st] : planes) {
    armed += (st.fb != 0 && st.crtc == g_crtc) ? 1U : 0U;
  }
  return armed;
}

}  // namespace

// NOLINTBEGIN(readability-identifier-naming,misc-use-internal-linkage)
extern "C" int drmModeAtomicCommit(int fd, drmModeAtomicReqPtr req, std::uint32_t flags,
                                   void* user_data) {
  using Fn = int (*)(int, drmModeAtomicReqPtr, std::uint32_t, void*);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  static const auto real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "drmModeAtomicCommit"));
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (g_limit != 0 && armed_after(fd, *reinterpret_cast<const Req*>(req)) > g_limit) {
    ++g_refused;
    return -EINVAL;
  }
  return real(fd, req, flags, user_data);
}
// NOLINTEND(readability-identifier-naming,misc-use-internal-linkage)

namespace {

std::optional<std::string> find_vkms_node() {
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/dev/dri", ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("card", 0) != 0) {
      continue;
    }
    const int fd = ::open(entry.path().c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    drmVersionPtr v = drmGetVersion(fd);
    const bool is_vkms = v != nullptr && v->name != nullptr && std::strcmp(v->name, "vkms") == 0;
    if (v != nullptr) {
      drmFreeVersion(v);
    }
    ::close(fd);
    if (is_vkms) {
      return entry.path().string();
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
  drmModeResPtr res = drmModeGetResources(fd);
  if (res == nullptr) {
    return std::nullopt;
  }
  std::optional<ActiveCrtc> found;
  for (int i = 0; i < res->count_connectors && !found.has_value(); ++i) {
    drmModeConnectorPtr conn = drmModeGetConnector(fd, res->connectors[i]);
    if (conn == nullptr) {
      continue;
    }
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0 && conn->encoder_id != 0) {
      if (drmModeEncoderPtr enc = drmModeGetEncoder(fd, conn->encoder_id); enc != nullptr) {
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

void fill(DumbBufferSource& source, std::uint32_t w, std::uint32_t h, std::uint32_t pixel) {
  auto mapping = source.map(drm::MapAccess::Write);
  ASSERT_TRUE(mapping.has_value());
  const auto pixels = mapping->pixels();
  for (std::uint32_t y = 0; y < h; ++y) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* row = reinterpret_cast<std::uint32_t*>(pixels.data() +
                                                 (static_cast<std::size_t>(y) * mapping->stride()));
    for (std::uint32_t x = 0; x < w; ++x) {
      row[x] = pixel;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
  }
}

}  // namespace

// NOLINTBEGIN(bugprone-unchecked-optional-access) -- every access follows an ASSERT_TRUE

// Four layers on a CRTC that lights two planes. The frame must commit, and
// every layer must reach the screen: the canvas carries what the planes can't.
TEST(LayerScenePlaneLimitVkms, CanvasCountsAgainstThePlaneLimit) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded — `sudo modprobe vkms enable_overlay=1`";
  }
  auto dev_r = Device::open(*node);
  ASSERT_TRUE(dev_r.has_value()) << dev_r.error().message();
  auto& dev = *dev_r;
  ASSERT_TRUE(dev.enable_universal_planes().has_value());
  ASSERT_TRUE(dev.enable_atomic().has_value());
  const auto active = pick_crtc(dev.fd());
  ASSERT_TRUE(active.has_value());
  const std::uint32_t fb_w = active->mode.hdisplay;
  const std::uint32_t fb_h = active->mode.vdisplay;

  LayerScene::Config cfg;
  cfg.crtc_id = active->crtc_id;
  cfg.connector_id = active->connector_id;
  cfg.mode = active->mode;
  auto scene_r = LayerScene::create(dev, cfg);
  ASSERT_TRUE(scene_r.has_value()) << scene_r.error().message();
  auto& scene = **scene_r;

  // A background and three disjoint tiles, each its own color.
  constexpr std::uint32_t k_side = 64;
  const std::array<std::uint32_t, 4> colors{0xFF202020U, 0xFFFF0000U, 0xFF00FF00U, 0xFF0000FFU};
  std::array<std::pair<std::uint32_t, std::uint32_t>, 4> probe{};
  for (std::size_t i = 0; i < colors.size(); ++i) {
    const bool bg = i == 0;
    const std::uint32_t w = bg ? fb_w : k_side;
    const std::uint32_t h = bg ? fb_h : k_side;
    auto src = DumbBufferSource::create(dev, w, h, DRM_FORMAT_ARGB8888);
    ASSERT_TRUE(src.has_value()) << src.error().message();
    fill(**src, w, h, colors.at(i));
    const auto x = bg ? 0 : static_cast<std::int32_t>(i * 2 * k_side);
    const auto y = bg ? 0 : static_cast<std::int32_t>(k_side);
    LayerDesc d;
    d.source = std::move(*src);
    d.display.src_rect = drm::scene::Rect{0, 0, w, h};
    d.display.dst_rect = drm::scene::Rect{x, y, w, h};
    d.display.zpos = static_cast<int>(i) + 3;
    ASSERT_TRUE(scene.add_layer(std::move(d)).has_value());
    probe.at(i) = bg ? std::pair{fb_w - 1U, fb_h - 1U}
                     : std::pair{static_cast<std::uint32_t>(x) + (k_side / 2U),
                                 static_cast<std::uint32_t>(y) + (k_side / 2U)};
  }

  g_crtc = active->crtc_id;
  g_limit = 2;
  g_refused = 0;
  auto first = scene.commit();
  auto second = scene.commit();  // steady state: warm start, cached canvas verdict
  g_limit = 0;
  ASSERT_TRUE(first.has_value()) << "the frame armed more planes than the CRTC lights: "
                                 << first.error().message();
  ASSERT_TRUE(second.has_value()) << second.error().message();
  EXPECT_EQ(first->layers_unassigned, 0U);
  EXPECT_GE(first->layers_composited, 2U);
  EXPECT_GT(g_refused, 0U) << "the limit never bit: the test proves nothing";
  EXPECT_LE(second->test_commits_issued, 1U) << "the demotion should hold: one warm-start TEST";

  auto img = snapshot(dev, active->crtc_id);
  drmModeSetCrtc(dev.fd(), active->crtc_id, 0, 0, 0, nullptr, 0, nullptr);
  ASSERT_TRUE(img.has_value()) << img.error().message();
  for (std::size_t i = 0; i < colors.size(); ++i) {
    const auto [px, py] = probe.at(i);
    EXPECT_EQ(img->pixels()[(static_cast<std::size_t>(py) * img->width()) + px], colors.at(i))
        << "layer " << i << " missing from the committed frame";
  }
}

// NOLINTEND(bugprone-unchecked-optional-access)
