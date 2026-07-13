// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Standing syscall / property census for LayerScene: pinned scenario
// workloads run through the scene, and the per-frame CommitReport counts
// (test commits, property writes, FB attaches, damage clips, fast-path hits,
// idle skips) are aggregated, printed as a machine-readable CENSUS line, and
// asserted against pinned expectations. A regression that reintroduces a
// redundant TEST_ONLY, loses the FB-only fast path, or stops emitting damage
// clips fails here.
//
// Scenarios: static (FB-only steady state), widget-scale damage, full-frame
// scroll (translate), and a multi-layer overlap. Each prints:
//   CENSUS scenario=<name> frames=N commits=C test_commits=T props=P fbs=F \
//          damage_clips=D fast_path=X idle=I
// so a HIL run on a real driver (vc4/VOP2) produces the same parseable census
// the vkms gate asserts on.
//
// Self-skips when VKMS isn't loaded (or set DRM_CXX_TEST_CARD to a modeset
// card to run on real hardware) — same pattern as the other integration files.

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/display/driver_profile.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_handle.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using drm::Device;
using drm::scene::AcquiredBuffer;
using drm::scene::BindingModel;
using drm::scene::CommitReport;
using drm::scene::DamageRect;
using drm::scene::DumbBufferSource;
using drm::scene::LayerBufferSource;
using drm::scene::LayerDesc;
using drm::scene::LayerScene;
using drm::scene::SourceFormat;

namespace {

std::optional<std::string> find_vkms_node() {
  const char* env = std::getenv("DRM_CXX_TEST_CARD");
  if (env != nullptr && *env != '\0') {
    return std::string(env);
  }
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
  drmModeModeInfo mode{};
};

drm::expected<ActiveCrtc, std::error_code> pick_crtc(int fd) {
  auto* res = drmModeGetResources(fd);
  if (res == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::no_such_device));
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
            ActiveCrtc out;
            out.connector_id = conn->connector_id;
            out.mode = conn->modes[0];
            out.crtc_id = res->crtcs[c];
            found = out;
            break;
          }
        }
        drmModeFreeEncoder(enc);
      }
    }
    drmModeFreeConnector(conn);
  }
  drmModeFreeResources(res);
  if (!found.has_value()) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::no_such_device_or_address));
  }
  return *found;
}

struct SceneFixture {
  std::unique_ptr<Device> dev;
  ActiveCrtc active;
  std::unique_ptr<LayerScene> scene;
};

drm::expected<SceneFixture, std::error_code> open_vkms_scene(const std::string& node) {
  auto dev_r = Device::open(node);
  if (!dev_r) {
    return drm::unexpected<std::error_code>(dev_r.error());
  }
  auto dev = std::make_unique<Device>(std::move(*dev_r));
  if (auto r = dev->enable_universal_planes(); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  if (auto r = dev->enable_atomic(); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  auto active_r = pick_crtc(dev->fd());
  if (!active_r) {
    return drm::unexpected<std::error_code>(active_r.error());
  }
  LayerScene::Config cfg;
  cfg.crtc_id = active_r->crtc_id;
  cfg.connector_id = active_r->connector_id;
  cfg.mode = active_r->mode;
  auto scene_r = LayerScene::create(*dev, cfg);
  if (!scene_r) {
    return drm::unexpected<std::error_code>(scene_r.error());
  }
  return SceneFixture{std::move(dev), *active_r, std::move(*scene_r)};
}

void cleanup_crtc(int fd, std::uint32_t crtc_id) {
  drmModeSetCrtc(fd, crtc_id, 0, 0, 0, nullptr, 0, nullptr);
}

// Wraps a real DumbBufferSource but injects a configurable per-frame damage
// region on each acquire — the only piece the existing test sources lack, and
// what lets the census drive the FB_DAMAGE_CLIPS path.
class ScenarioSource : public LayerBufferSource {
 public:
  explicit ScenarioSource(std::unique_ptr<DumbBufferSource> inner) : inner_(std::move(inner)) {}

  void set_damage(std::vector<DamageRect> damage) { next_damage_ = std::move(damage); }

  drm::expected<AcquiredBuffer, std::error_code> acquire() override {
    auto r = inner_->acquire();
    if (r) {
      r->damage = next_damage_;
    }
    return r;
  }
  void release(AcquiredBuffer acquired) noexcept override { inner_->release(std::move(acquired)); }
  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return inner_->binding_model();
  }
  [[nodiscard]] SourceFormat format() const noexcept override { return inner_->format(); }
  drm::expected<drm::BufferMapping, std::error_code> map(drm::MapAccess access) override {
    return inner_->map(access);
  }
  void on_session_paused() noexcept override { inner_->on_session_paused(); }
  drm::expected<void, std::error_code> on_session_resumed(const drm::Device& new_dev) override {
    return inner_->on_session_resumed(new_dev);
  }

 private:
  std::unique_ptr<DumbBufferSource> inner_;
  std::vector<DamageRect> next_damage_;
};

// Aggregated per-scenario census. Idle-skipped frames are counted but their
// (all-zero) commit counts are not folded in.
struct Census {
  std::size_t frames{0};
  std::size_t commits{0};
  std::size_t test_commits{0};
  std::size_t props{0};
  std::size_t fbs{0};
  std::size_t damage_clips{0};
  std::size_t fast_path_frames{0};
  std::size_t idle_frames{0};

  void add(const CommitReport& r) {
    ++frames;
    if (r.skipped_idle) {
      ++idle_frames;
      return;
    }
    ++commits;
    test_commits += r.test_commits_issued;
    props += r.properties_written;
    fbs += r.fbs_attached;
    damage_clips += r.damage_clips_armed;
    if (r.fb_delta_fast_path) {
      ++fast_path_frames;
    }
  }

  void print(const char* scenario) const {
    // Machine-readable: one CENSUS line per scenario, grep-parseable on HIL.
    std::printf(
        "CENSUS scenario=%s frames=%zu commits=%zu test_commits=%zu props=%zu fbs=%zu "
        "damage_clips=%zu fast_path=%zu idle=%zu\n",
        scenario, frames, commits, test_commits, props, fbs, damage_clips, fast_path_frames,
        idle_frames);
  }
};

}  // namespace

// Static single-layer scene: only FB_ID changes per frame. After the cold
// commit every frame must take the FB-only fast path (zero TEST_ONLY), emit no
// damage clips, and never idle-skip (commit() always commits).
TEST(LayerSceneCensusVkms, StaticSteadyState) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded (or set DRM_CXX_TEST_CARD to a modeset card)";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;
  const auto w = fx.active.mode.hdisplay;
  const auto h = fx.active.mode.vdisplay;

  auto inner = DumbBufferSource::create(*fx.dev, w, h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(inner.has_value()) << inner.error().message();
  LayerDesc d;
  d.source = std::make_unique<ScenarioSource>(std::move(*inner));
  d.display.src_rect = drm::scene::Rect{0, 0, w, h};
  d.display.dst_rect = drm::scene::Rect{0, 0, w, h};
  d.display.zpos = 1;
  ASSERT_TRUE(fx.scene->add_layer(std::move(d)).has_value());

  constexpr int k_frames = 30;
  Census c;
  for (int i = 0; i < k_frames; ++i) {
    auto r = fx.scene->commit();
    ASSERT_TRUE(r.has_value()) << "frame " << i << ": " << r.error().message();
    c.add(*r);
  }
  c.print("static");

  EXPECT_EQ(c.commits, static_cast<std::size_t>(k_frames));
  EXPECT_EQ(c.idle_frames, 0U);
  EXPECT_EQ(c.test_commits, 1U) << "only the cold frame runs a TEST_ONLY";
  EXPECT_EQ(c.fast_path_frames, static_cast<std::size_t>(k_frames - 1))
      << "every post-cold frame takes the FB-only fast path";
  EXPECT_EQ(c.damage_clips, 0U) << "no damage reported → no FB_DAMAGE_CLIPS";
  EXPECT_GE(c.fbs, c.commits) << "FB_ID re-attaches every frame";

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}

// Widget-scale damage: a small dirty rect every frame. Damage changes content,
// not placement, so the fast path still engages — and each damaged frame must
// arm exactly one FB_DAMAGE_CLIPS blob (when the driver exposes the property).
TEST(LayerSceneCensusVkms, WidgetScaleDamage) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded (or set DRM_CXX_TEST_CARD to a modeset card)";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;
  const auto w = fx.active.mode.hdisplay;
  const auto h = fx.active.mode.vdisplay;

  auto inner = DumbBufferSource::create(*fx.dev, w, h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(inner.has_value()) << inner.error().message();
  auto src = std::make_unique<ScenarioSource>(std::move(*inner));
  auto* src_raw = src.get();
  LayerDesc d;
  d.source = std::move(src);
  d.display.src_rect = drm::scene::Rect{0, 0, w, h};
  d.display.dst_rect = drm::scene::Rect{0, 0, w, h};
  d.display.zpos = 1;
  ASSERT_TRUE(fx.scene->add_layer(std::move(d)).has_value());

  constexpr int k_frames = 30;
  Census c;
  for (int i = 0; i < k_frames; ++i) {
    // A 64x64 widget-scale dirty region, moving slightly so it stays non-empty.
    src_raw->set_damage({DamageRect{10, 10, 64, 64}});
    auto r = fx.scene->commit();
    ASSERT_TRUE(r.has_value()) << "frame " << i << ": " << r.error().message();
    c.add(*r);
  }
  c.print("widget_damage");

  EXPECT_EQ(c.commits, static_cast<std::size_t>(k_frames));
  EXPECT_EQ(c.idle_frames, 0U);
  EXPECT_EQ(c.test_commits, 1U) << "damage is content, not placement — still one cold TEST_ONLY";
  EXPECT_EQ(c.fast_path_frames, static_cast<std::size_t>(k_frames - 1))
      << "a damaged frame keeps the FB-only fast path (geometry unchanged)";
  // The damage-clip count is driver-gated. On a driver that exposes
  // FB_DAMAGE_CLIPS (amdgpu, vc4, …) every committed frame arms exactly one
  // for this full-screen layer; on a driver without it (vkms) the scene falls
  // back to full-frame and arms none. Probe the driver and assert the exact
  // count for its capability — so the gate is precise on both.
  auto profile = drm::display::DriverProfile::probe(*fx.dev);
  ASSERT_TRUE(profile.has_value()) << profile.error().message();
  if (profile->fb_damage_clips) {
    EXPECT_EQ(c.damage_clips, c.commits)
        << "a damage-capable driver must arm one FB_DAMAGE_CLIPS blob per damaged frame";
  } else {
    EXPECT_EQ(c.damage_clips, 0U) << "driver without FB_DAMAGE_CLIPS falls back to full-frame";
  }

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}

// Full-frame scroll: a sub-screen layer translated every few frames. Each move
// changes CRTC_X, defeats the fast path, and re-validates with one TEST_ONLY;
// the frames between are back on the fast path.
TEST(LayerSceneCensusVkms, FullFrameScroll) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded (or set DRM_CXX_TEST_CARD to a modeset card)";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;
  const auto w = fx.active.mode.hdisplay;
  const auto h = fx.active.mode.vdisplay;
  const std::uint32_t lw = w / 2U;
  const std::uint32_t lh = h / 2U;

  auto inner = DumbBufferSource::create(*fx.dev, lw, lh, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(inner.has_value()) << inner.error().message();
  LayerDesc d;
  d.source = std::make_unique<ScenarioSource>(std::move(*inner));
  d.display.src_rect = drm::scene::Rect{0, 0, lw, lh};
  d.display.dst_rect = drm::scene::Rect{0, 0, lw, lh};
  d.display.zpos = 1;
  auto handle_r = fx.scene->add_layer(std::move(d));
  ASSERT_TRUE(handle_r.has_value());
  const auto handle = *handle_r;

  constexpr int k_frames = 30;
  constexpr int k_move_every = 10;  // move at frames 10, 20 (not the cold frame)
  int moves = 0;
  Census c;
  for (int i = 0; i < k_frames; ++i) {
    if (i > 0 && i % k_move_every == 0) {
      auto* layer = fx.scene->get_layer(handle);
      ASSERT_NE(layer, nullptr);
      const std::int32_t x = 16 * (i / k_move_every);  // same size, no scaling
      layer->set_dst_rect(drm::scene::Rect{x, 0, lw, lh});
      ++moves;
    }
    auto r = fx.scene->commit();
    ASSERT_TRUE(r.has_value()) << "frame " << i << ": " << r.error().message();
    c.add(*r);
  }
  c.print("scroll");

  EXPECT_EQ(c.commits, static_cast<std::size_t>(k_frames));
  EXPECT_EQ(c.idle_frames, 0U);
  // One cold TEST + one per move; every other frame is on the fast path.
  EXPECT_EQ(c.test_commits, static_cast<std::size_t>(1 + moves));
  EXPECT_EQ(c.fast_path_frames, static_cast<std::size_t>(k_frames - 1 - moves))
      << "only the cold and move frames leave the fast path";

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}

// Multi-layer overlap: three stacked layers. Steady state must reach the fast
// path for the whole set — the census proves the optimization holds beyond one
// layer.
TEST(LayerSceneCensusVkms, MultiLayerOverlap) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded (or set DRM_CXX_TEST_CARD to a modeset card)";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;
  const auto w = fx.active.mode.hdisplay;
  const auto h = fx.active.mode.vdisplay;

  // A full-screen background plus two smaller overlapping tiles.
  struct Tile {
    std::uint32_t w, h, x, y, z;
  };
  const Tile tiles[] = {{w, h, 0, 0, 1}, {w / 3U, h / 3U, 32, 32, 2}, {w / 4U, h / 4U, 64, 64, 3}};
  for (const auto& t : tiles) {
    auto inner = DumbBufferSource::create(*fx.dev, t.w, t.h, DRM_FORMAT_ARGB8888);
    ASSERT_TRUE(inner.has_value()) << inner.error().message();
    LayerDesc d;
    d.source = std::make_unique<ScenarioSource>(std::move(*inner));
    d.display.src_rect = drm::scene::Rect{0, 0, t.w, t.h};
    d.display.dst_rect =
        drm::scene::Rect{static_cast<std::int32_t>(t.x), static_cast<std::int32_t>(t.y), t.w, t.h};
    d.display.zpos = t.z;
    ASSERT_TRUE(fx.scene->add_layer(std::move(d)).has_value());
  }

  constexpr int k_frames = 20;
  Census c;
  std::size_t last_assigned = 0;
  for (int i = 0; i < k_frames; ++i) {
    auto r = fx.scene->commit();
    ASSERT_TRUE(r.has_value()) << "frame " << i << ": " << r.error().message();
    last_assigned = r->layers_assigned + r->layers_composited;
    c.add(*r);
  }
  c.print("multilayer");

  EXPECT_EQ(c.commits, static_cast<std::size_t>(k_frames));
  EXPECT_EQ(last_assigned, 3U) << "all three layers reach scanout (plane or composited)";
  EXPECT_EQ(c.test_commits, 1U) << "steady multi-layer state still costs one cold TEST_ONLY only";
  EXPECT_EQ(c.fast_path_frames, static_cast<std::size_t>(k_frames - 1))
      << "the fast path holds for a multi-layer scene, not just single-layer";

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}
