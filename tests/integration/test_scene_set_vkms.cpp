// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// SceneSet integration tests against vkms.
//
// Requires a vkms instance with >= 2 connected virtual connectors.
// scripts/vkms_dual.sh handles the configfs provisioning:
//
//   sudo scripts/vkms_dual.sh up      # create the "dual" instance
//   sudo scripts/vkms_dual.sh down    # tear it down
//
// Tests GTEST_SKIP when no vkms instance has >=2 connected outputs.

#include "../../examples/common/multi_crtc_probe.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/display_params.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/scene/scene_set.hpp>

#include <drm_fourcc.h>
#include <xf86drm.h>

#include <cstdint>
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

namespace {

// Reuse the multi_crtc_probe helper for output enumeration so this
// test stays aligned with the bare-TTY validation path on amdgpu.
using drm::examples::multi_crtc::ConnectedOutput;
using drm::examples::multi_crtc::enumerate_connected_outputs;

// Locate the first vkms /dev/dri/cardN node with at least
// `min_outputs` connected virtual connectors. Returns nullopt when
// no qualifying instance is present, including when vkms isn't
// loaded at all.
std::optional<std::string> find_vkms_multi_crtc(std::size_t min_outputs) {
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/dev/dri", ec)) {
    const auto& p = entry.path();
    const std::string name = p.filename().string();
    if (name.rfind("card", 0) != 0) {
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
    if (!is_vkms) {
      ::close(fd);
      continue;
    }
    // Have a vkms fd; check whether its enumeration yields >= min_outputs.
    auto dev = drm::Device::from_fd(fd);
    (void)dev.enable_universal_planes();
    (void)dev.enable_atomic();
    const auto outputs = enumerate_connected_outputs(dev);
    ::close(fd);
    if (outputs.size() >= min_outputs) {
      return p.string();
    }
  }
  return std::nullopt;
}

// Build N LayerScenes against the supplied outputs. Owns the
// underlying Device too so the lifetime nesting matches LayerScene's
// captured-by-reference contract.
struct ScenesFixture {
  std::unique_ptr<drm::Device> dev;
  std::vector<ConnectedOutput> outputs;
  std::vector<std::unique_ptr<drm::scene::LayerScene>> scenes;
};

drm::expected<ScenesFixture, std::error_code> open_scenes(const std::string& node) {
  auto dev_r = drm::Device::open(node);
  if (!dev_r) {
    return drm::unexpected<std::error_code>(dev_r.error());
  }
  auto dev = std::make_unique<drm::Device>(std::move(*dev_r));
  if (auto r = dev->enable_universal_planes(); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  if (auto r = dev->enable_atomic(); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  auto outputs = enumerate_connected_outputs(*dev);
  if (outputs.size() < 2) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::no_such_device_or_address));
  }
  std::vector<std::unique_ptr<drm::scene::LayerScene>> scenes;
  scenes.reserve(outputs.size());
  for (const auto& o : outputs) {
    drm::scene::LayerScene::Config cfg;
    cfg.crtc_id = o.crtc_id;
    cfg.connector_id = o.connector_id;
    cfg.mode = o.mode;
    auto s = drm::scene::LayerScene::create(*dev, cfg);
    if (!s) {
      return drm::unexpected<std::error_code>(s.error());
    }
    scenes.push_back(std::move(*s));
  }
  return ScenesFixture{std::move(dev), std::move(outputs), std::move(scenes)};
}

}  // namespace

// Combined cross-CRTC TEST_ONLY: each scene carries its own dumb-buffer
// layer sized to its mode; SceneSet packs the two scenes' property
// writes into one drmModeAtomicCommit and asks the kernel to validate
// the combination. Acceptance proves SceneSet's build → submit → finalize
// path produces a kernel-acceptable atomic on multi-CRTC vkms.
TEST(SceneSetVkms, CombinedTestAcceptsMultiCrtc) {
  const auto node = find_vkms_multi_crtc(2);
  if (!node) {
    GTEST_SKIP() << "no vkms instance with >=2 connected outputs "
                    "(run `sudo scripts/vkms_dual.sh up`)";
  }
  auto fx_r = open_scenes(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;

  for (std::size_t i = 0; i < fx.outputs.size(); ++i) {
    const auto& o = fx.outputs[i];
    auto src = drm::scene::DumbBufferSource::create(*fx.dev, o.mode.hdisplay, o.mode.vdisplay,
                                                    DRM_FORMAT_ARGB8888);
    ASSERT_TRUE(src.has_value()) << "DumbBufferSource for " << o.connector_name << ": "
                                 << src.error().message();
    drm::scene::LayerDesc desc;
    desc.source = std::move(*src);
    desc.display.src_rect = drm::scene::Rect{0, 0, o.mode.hdisplay, o.mode.vdisplay};
    desc.display.dst_rect = drm::scene::Rect{0, 0, o.mode.hdisplay, o.mode.vdisplay};
    auto h = fx.scenes[i]->add_layer(std::move(desc));
    ASSERT_TRUE(h.has_value()) << "add_layer for " << o.connector_name << ": "
                               << h.error().message();
  }

  auto set_r = drm::scene::SceneSet::create(*fx.dev, std::move(fx.scenes));
  ASSERT_TRUE(set_r.has_value()) << set_r.error().message();

  auto reports = (*set_r)->test();
  ASSERT_TRUE(reports.has_value()) << reports.error().message();
  EXPECT_EQ(reports->size(), fx.outputs.size());
  for (std::size_t i = 0; i < reports->size(); ++i) {
    const auto& r = (*reports)[i];
    EXPECT_EQ(r.layers_total, 1U) << "scene " << i << " (" << fx.outputs[i].connector_name
                                  << ") should report exactly one layer";
  }
}

// Mirrored single shared source across N scenes via
// SceneSet::add_layer. Validates the SharedLayerBufferSource forwarder
// path under a real kernel — every participating scene calls the
// forwarder's acquire(), which round-trips to the same underlying
// DumbBufferSource, and the combined TEST commit lands.
TEST(SceneSetVkms, MirroredLayerAcceptsAcrossMultiCrtc) {
  const auto node = find_vkms_multi_crtc(2);
  if (!node) {
    GTEST_SKIP() << "no vkms instance with >=2 connected outputs";
  }
  auto fx_r = open_scenes(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;

  auto set_r = drm::scene::SceneSet::create(*fx.dev, std::move(fx.scenes));
  ASSERT_TRUE(set_r.has_value()) << set_r.error().message();

  constexpr std::uint32_t mirror_side = 256;
  auto shared =
      drm::scene::DumbBufferSource::create(*fx.dev, mirror_side, mirror_side, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(shared.has_value()) << shared.error().message();
  const std::shared_ptr<drm::scene::LayerBufferSource> shared_src(std::move(*shared));

  drm::scene::SceneSetLayerSpec spec;
  spec.source = shared_src;
  spec.targets.reserve(fx.outputs.size());
  for (std::size_t i = 0; i < fx.outputs.size(); ++i) {
    const auto& o = fx.outputs[i];
    const std::uint32_t cx =
        (o.mode.hdisplay > mirror_side) ? (o.mode.hdisplay - mirror_side) / 2 : 0;
    const std::uint32_t cy =
        (o.mode.vdisplay > mirror_side) ? (o.mode.vdisplay - mirror_side) / 2 : 0;
    drm::scene::DisplayParams display;
    display.src_rect = drm::scene::Rect{0, 0, mirror_side, mirror_side};
    display.dst_rect = drm::scene::Rect{static_cast<std::int32_t>(cx),
                                        static_cast<std::int32_t>(cy), mirror_side, mirror_side};
    spec.targets.push_back({.scene_index = i, .display = display, .force_composited = false});
  }

  auto handle = (*set_r)->add_layer(spec);
  ASSERT_TRUE(handle.has_value()) << handle.error().message();
  EXPECT_TRUE(handle->valid());

  auto reports = (*set_r)->test();
  ASSERT_TRUE(reports.has_value()) << reports.error().message();
  EXPECT_EQ(reports->size(), fx.outputs.size());
  for (std::size_t i = 0; i < reports->size(); ++i) {
    const auto& r = (*reports)[i];
    EXPECT_EQ(r.layers_total, 1U) << "scene " << i << " should see exactly the mirrored layer";
  }

  // Removing the handle should clear every per-scene layer, leaving
  // an empty scene that still accepts a combined test commit.
  (*set_r)->remove_layer(*handle);
  auto post_remove = (*set_r)->test();
  ASSERT_TRUE(post_remove.has_value()) << post_remove.error().message();
  for (const auto& r : *post_remove) {
    EXPECT_EQ(r.layers_total, 0U);
  }
}
