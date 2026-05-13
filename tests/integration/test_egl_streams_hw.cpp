// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Hardware-gated integration tests for the EGL Streams plumbing.
//
// All TEST bodies open /dev/dri/card0 and bail out via GTEST_SKIP
// before any EGL/scene work when `drm::scene::probe_stream_capability`
// returns Unsupported. On a Mesa-only CI host the probe correctly
// reports Unsupported and every test skips; on a host with a working
// proprietary EGL stack (NVIDIA proprietary, Tegra) the tests
// exercise the real protocol against actual hardware.
//
// What's tested:
//
//   * Probe contract on a real device (mixing != Unsupported,
//     extension chain present, vendor / version strings populated).
//   * EglStreamBuilder::build succeeds and populates every Result
//     field (display, config, context, source_ptr, stream).
//   * The full scene-level flow: LayerScene::create -> add_layer ->
//     commit. After the first commit `source->bound_plane()` is
//     populated and `source->producer_surface()` is non-null. On
//     drivers that export EGL_NV_output_drm_flip_event,
//     `source->flip_event_data()` is also populated.
//
// The full-flow test runs a modeset against the picked connector
// and briefly takes over the display. That's the same disruption
// any vkms integration test imposes; CI skips it by virtue of the
// probe gate.

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/scene/stream_capability.hpp>

#include <gtest/gtest.h>

#if DRM_CXX_HAS_EGL_STREAMS

#include "scene/egl_stream_source.hpp"

#include <drm-cxx/core/resources.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/modeset/mode.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/egl_stream_builder.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <xf86drmMode.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cstdint>
#include <optional>
#include <system_error>
#include <utility>

namespace {

constexpr const char* k_drm_node = "/dev/dri/card0";

struct PickedOutput {
  std::uint32_t crtc_id{0};
  std::uint32_t connector_id{0};
  drmModeModeInfo mode{};
};

// Permissive picker mirroring the stream_demo's: handles connectors
// the kernel has never modeset (encoder_id == 0) by falling back to
// the connector's encoders[0] and the encoder's first allowed CRTC.
// Standard examples/common/open_output.hpp requires a previously-
// modeset connector, which a fresh TTY-only test host doesn't have.
std::optional<PickedOutput> pick_output(int fd) {
  const auto res = drm::get_resources(fd);
  if (!res) {
    return std::nullopt;
  }
  const auto connector_ids = drm::span<const std::uint32_t>(res->connectors, res->count_connectors);
  for (const auto cid : connector_ids) {
    const auto conn = drm::get_connector(fd, cid);
    if (!conn || conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0) {
      continue;
    }
    std::uint32_t enc_id = conn->encoder_id;
    if (enc_id == 0 && conn->count_encoders > 0) {
      enc_id = conn->encoders[0];
    }
    if (enc_id == 0) {
      continue;
    }
    const auto enc = drm::get_encoder(fd, enc_id);
    if (!enc) {
      continue;
    }
    std::uint32_t crtc_id = enc->crtc_id;
    if (crtc_id == 0) {
      for (int c = 0; c < res->count_crtcs; ++c) {
        if ((enc->possible_crtcs & (1U << static_cast<unsigned>(c))) != 0) {
          crtc_id = res->crtcs[c];
          break;
        }
      }
    }
    if (crtc_id == 0) {
      continue;
    }
    const auto mode_res = drm::select_preferred_mode(
        drm::span<const drmModeModeInfo>(conn->modes, conn->count_modes));
    return PickedOutput{crtc_id, cid, mode_res ? mode_res->drm_mode : conn->modes[0]};
  }
  return std::nullopt;
}

struct StreamFixture {
  drm::Device device;
  PickedOutput output;
  drm::scene::StreamCapability capability;
};

[[nodiscard]] std::optional<StreamFixture> open_stream_fixture() {
  auto dev_r = drm::Device::open(k_drm_node);
  if (!dev_r) {
    return std::nullopt;
  }
  auto dev = std::move(*dev_r);
  if (auto r = dev.enable_universal_planes(); !r) {
    return std::nullopt;
  }
  if (auto r = dev.enable_atomic(); !r) {
    return std::nullopt;
  }
  const auto cap = drm::scene::probe_stream_capability(dev);
  if (!cap.usable()) {
    return std::nullopt;
  }
  const auto picked = pick_output(dev.fd());
  if (!picked.has_value()) {
    return std::nullopt;
  }
  return StreamFixture{std::move(dev), *picked, cap};
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// Probe contract on a real device
// ─────────────────────────────────────────────────────────────────────

TEST(EglStreamsHw, ProbeReportsUsableExtensionChain) {
  auto fx = open_stream_fixture();
  if (!fx.has_value()) {
    GTEST_SKIP() << "EGL Streams not usable on this host (Mesa-only, no libEGL_nvidia, "
                    "or no connected output)";
  }
  const auto& cap = fx->capability;
  EXPECT_NE(cap.mixing, drm::scene::StreamMixingMode::Unsupported);
  EXPECT_TRUE(cap.has_egl_runtime);
  EXPECT_TRUE(cap.has_platform_device);
  EXPECT_TRUE(cap.has_device_drm);
  EXPECT_TRUE(cap.has_output_drm);
  EXPECT_TRUE(cap.has_khr_stream);
  EXPECT_TRUE(cap.has_stream_consumer_egloutput);
  EXPECT_TRUE(cap.has_stream_producer_eglsurface);
  EXPECT_FALSE(cap.vendor.empty());
  EXPECT_FALSE(cap.version.empty());
}

// ─────────────────────────────────────────────────────────────────────
// EglStreamBuilder::build populates every Result field
// ─────────────────────────────────────────────────────────────────────

TEST(EglStreamsHw, BuilderResultFieldsArePopulated) {
  auto fx = open_stream_fixture();
  if (!fx.has_value()) {
    GTEST_SKIP() << "EGL Streams not usable on this host";
  }
  drm::scene::EglStreamBuilder::Request req;
  req.capability = fx->capability;
  req.device = &fx->device;
  req.format = drm::scene::SourceFormat{DRM_FORMAT_ARGB8888, 0, 320, 180};
  auto built = drm::scene::EglStreamBuilder::build(req);
  ASSERT_TRUE(built.has_value()) << built.error().message();

  auto& result = *built;
  EXPECT_NE(result.display, EGL_NO_DISPLAY);
  EXPECT_NE(result.egl_config, nullptr);
  EXPECT_NE(result.context, EGL_NO_CONTEXT);
  EXPECT_TRUE(result.context_created_by_builder);
  EXPECT_NE(result.source_ptr, nullptr);
  EXPECT_NE(result.source, nullptr);
  EXPECT_EQ(result.source->binding_model(), drm::scene::BindingModel::DriverOwnsBinding);
  // Pre-bind contract: producer surface and bound plane are nullopt
  // until LayerScene's first commit drives bind_to_plane. The stream
  // handle is non-null because EglStreamSource::create allocates it.
  EXPECT_EQ(result.source_ptr->producer_surface(), EGL_NO_SURFACE);
  EXPECT_FALSE(result.source_ptr->bound_plane().has_value());
  EXPECT_FALSE(result.source_ptr->flip_event_data().has_value());
  EXPECT_NE(result.stream, EGL_NO_STREAM_KHR);

  // Process exit handles EGL teardown. The source's unique_ptr
  // destructor releases the EGLStream / surface; eglDestroyContext
  // and eglTerminate would otherwise be needed but linking libEGL
  // directly into a test binary adds dependency surface for no
  // assertion gain. Leaked-at-exit is fine for the test runner.
  result.source.reset();
}

// ─────────────────────────────────────────────────────────────────────
// EglStreamBuilder rejection paths on the real device
// ─────────────────────────────────────────────────────────────────────

TEST(EglStreamsHw, BuilderRejectsUnsupportedAndZeroDims) {
  auto fx = open_stream_fixture();
  if (!fx.has_value()) {
    GTEST_SKIP() << "EGL Streams not usable on this host";
  }

  drm::scene::EglStreamBuilder::Request req;
  req.device = &fx->device;
  req.format = drm::scene::SourceFormat{DRM_FORMAT_ARGB8888, 0, 64, 64};

  // Real probe + Unsupported capability override.
  req.capability = drm::scene::stream_capability_unsupported();
  {
    auto r = drm::scene::EglStreamBuilder::build(req);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), std::make_error_code(std::errc::function_not_supported));
  }

  // Real (usable) capability + zero width.
  req.capability = fx->capability;
  req.format.width = 0;
  req.format.height = 64;
  {
    auto r = drm::scene::EglStreamBuilder::build(req);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
  }
}

// ─────────────────────────────────────────────────────────────────────
// Full scene flow: add_layer + first commit drives bind_to_plane.
//
// This test runs a modeset on the picked connector and briefly takes
// over the display. CI hosts without a working streams stack skip
// before any of that happens.
// ─────────────────────────────────────────────────────────────────────

TEST(EglStreamsHw, SceneCommitWiresUpStreamLayerEndToEnd) {
  auto fx = open_stream_fixture();
  if (!fx.has_value()) {
    GTEST_SKIP() << "EGL Streams not usable on this host";
  }

  const std::uint32_t fb_w = fx->output.mode.hdisplay;
  const std::uint32_t fb_h = fx->output.mode.vdisplay;

  drm::scene::LayerScene::Config scene_cfg;
  scene_cfg.crtc_id = fx->output.crtc_id;
  scene_cfg.connector_id = fx->output.connector_id;
  scene_cfg.mode = fx->output.mode;
  scene_cfg.stream_capability = fx->capability;
  auto scene_r = drm::scene::LayerScene::create(fx->device, scene_cfg);
  ASSERT_TRUE(scene_r.has_value()) << scene_r.error().message();
  auto& scene = *scene_r;

  // Full-screen background dumb buffer to keep PRIMARY armed.
  auto bg_r = drm::scene::DumbBufferSource::create(fx->device, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(bg_r.has_value()) << bg_r.error().message();
  drm::scene::LayerDesc bg_desc;
  bg_desc.source = std::move(*bg_r);
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.zpos = 1;
  ASSERT_TRUE(scene->add_layer(std::move(bg_desc)).has_value());

  // Stream layer.
  drm::scene::EglStreamBuilder::Request bld_req;
  bld_req.capability = fx->capability;
  bld_req.device = &fx->device;
  bld_req.format = drm::scene::SourceFormat{DRM_FORMAT_ARGB8888, 0, 320, 180};
  auto bld_r = drm::scene::EglStreamBuilder::build(bld_req);
  ASSERT_TRUE(bld_r.has_value()) << bld_r.error().message();
  auto& bld = *bld_r;
  auto* stream_source = bld.source_ptr;

  drm::scene::LayerDesc stream_desc;
  stream_desc.source = std::move(bld.source);
  stream_desc.display.src_rect = drm::scene::Rect{0, 0, 320, 180};
  stream_desc.display.dst_rect = drm::scene::Rect{0, 0, 320, 180};
  stream_desc.display.zpos = 2;
  ASSERT_TRUE(scene->add_layer(std::move(stream_desc)).has_value());

  auto commit_r = scene->commit();
  ASSERT_TRUE(commit_r.has_value()) << commit_r.error().message();

  // Post-commit invariants: bind_to_plane ran during do_commit's
  // pre-pass, so the source's plane id and producer surface are
  // now populated.
  EXPECT_TRUE(stream_source->bound_plane().has_value());
  EXPECT_NE(stream_source->producer_surface(), EGL_NO_SURFACE);
  EXPECT_NE(stream_source->stream(), EGL_NO_STREAM_KHR);

  // Empirical mixing probe runs against the same stream consumer
  // state. Verdict must be Mixed or Exclusive (never Unsupported,
  // since we already passed the usable() gate above).
  auto mix_r = scene->probe_stream_mixing();
  ASSERT_TRUE(mix_r.has_value()) << mix_r.error().message();
  EXPECT_NE(*mix_r, drm::scene::StreamMixingMode::Unsupported);

  // Tear the scene down so the source's destructor runs against
  // the still-valid EGLDisplay (the source's unique_ptr lives in
  // the scene). eglDestroyContext / eglTerminate are skipped --
  // process exit handles them, and linking libEGL directly into
  // the test binary would just add dependency surface.
  scene.reset();
}

#else  // !DRM_CXX_HAS_EGL_STREAMS

TEST(EglStreamsHw, StreamsBuildGateDisabled) {
  GTEST_SKIP() << "drm-cxx built without -DDRM_CXX_STREAMS=ON";
}

#endif  // DRM_CXX_HAS_EGL_STREAMS
