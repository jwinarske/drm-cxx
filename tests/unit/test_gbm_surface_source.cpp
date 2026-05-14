// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::scene::GbmSurfaceSource. Covers argument
// validation, factory-failure error mapping, and the post-create
// invariants visible without a renderer attached. End-to-end scanout
// against a live producer (EGL/Vulkan) belongs in the example apps
// and a follow-up integration test — see the file comment in
// gbm_surface_source.hpp.

#include "core/device.hpp"
#include "drm-cxx/buffer_mapping.hpp"
#include "drm-cxx/detail/expected.hpp"

#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/gbm_surface_source.hpp>

#include <drm_fourcc.h>

#include <cstdint>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <system_error>

namespace {

drm::scene::GbmSurfaceConfig good_config() noexcept {
  drm::scene::GbmSurfaceConfig cfg;
  cfg.width = 640;
  cfg.height = 480;
  cfg.drm_format = DRM_FORMAT_XRGB8888;
  cfg.modifier = DRM_FORMAT_MOD_INVALID;
  cfg.usage = 0;
  return cfg;
}

// Tries every /dev/dri/card* in turn. Returns a usable Device or
// nullopt; many CI hosts only have a render node, but GBM surfaces
// for scanout target need the modeset/card node.
[[nodiscard]] drm::expected<drm::Device, std::error_code> open_card_device() noexcept {
  for (const char* path : {"/dev/dri/card0", "/dev/dri/card1", "/dev/dri/card2"}) {
    int const fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    return drm::Device::from_fd(fd);
  }
  return drm::unexpected<std::error_code>(
      std::make_error_code(std::errc::no_such_file_or_directory));
}

}  // namespace

// ── Argument validation ────────────────────────────────────────────────

TEST(SceneGbmSurfaceSource, RejectsZeroWidth) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.width = 0;
  auto r = drm::scene::GbmSurfaceSource::create(dev, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneGbmSurfaceSource, RejectsZeroHeight) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.height = 0;
  auto r = drm::scene::GbmSurfaceSource::create(dev, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneGbmSurfaceSource, RejectsZeroFormat) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.drm_format = 0;
  auto r = drm::scene::GbmSurfaceSource::create(dev, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneGbmSurfaceSource, RejectsUnsupportedFormat) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  // NV12 is semi-planar — explicitly out of v1 scope for the source.
  cfg.drm_format = DRM_FORMAT_NV12;
  auto r = drm::scene::GbmSurfaceSource::create(dev, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::not_supported));
}

TEST(SceneGbmSurfaceSource, RejectsNegativeFd) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::GbmSurfaceSource::create(dev, good_config());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::bad_file_descriptor));
}

TEST(SceneGbmSurfaceSource, RejectsZeroDimsAndZeroFormatAsInvalidArg) {
  // When both width and format are bad, validate_config rejects on
  // dims first — pin the ordering so callers see a stable error when
  // they pass partially-zeroed configs.
  auto dev = drm::Device::from_fd(-1);
  drm::scene::GbmSurfaceConfig const cfg{};
  auto r = drm::scene::GbmSurfaceSource::create(dev, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

// ── Live-device behavior (skipped when no GPU + KMS card node) ────────

TEST(SceneGbmSurfaceSource, CreatesAgainstRealCardNode) {
  auto dev = open_card_device();
  if (!dev.has_value()) {
    GTEST_SKIP() << "No /dev/dri/card* available for GBM surface creation";
  }
  auto src = drm::scene::GbmSurfaceSource::create(*dev, good_config());
  if (!src.has_value()) {
    // GBM can refuse on vgem (no scanout-capable BOs) or on broken
    // drivers. Treat the create-failure as a skip — what we wanted to
    // exercise was the path through validate_config, not the kernel
    // outcome.
    GTEST_SKIP() << "gbm_surface_create rejected the request: " << src.error().message();
  }
  ASSERT_NE((*src)->native_surface(), nullptr);

  const auto fmt = (*src)->format();
  EXPECT_EQ(fmt.drm_fourcc, static_cast<std::uint32_t>(DRM_FORMAT_XRGB8888));
  EXPECT_EQ(fmt.width, 640U);
  EXPECT_EQ(fmt.height, 480U);
  // No producer attached → modifier reflects the Config's INVALID
  // request and stays INVALID until the first BO is locked.
  EXPECT_EQ(fmt.modifier, DRM_FORMAT_MOD_INVALID);

  EXPECT_EQ((*src)->binding_model(), drm::scene::BindingModel::SceneSubmitsFbId);
}

// NOTE: there's no "fresh surface, never bound, expect acquire() ==
// EAGAIN" test here on purpose. Mesa's gbm_surface_lock_front_buffer
// dispatch table is only populated once an EGL/Vulkan producer has
// bound the surface (the platform hooks are filled in by the binding
// — bare gbm_surface_create leaves them NULL on at least amdgpu). The
// scene's contract is "don't acquire before the producer has pushed a
// frame," and there's no robust way to test pre-bind acquire without
// invoking UB inside mesa. End-to-end behavior is covered by the
// example apps.

TEST(SceneGbmSurfaceSource, MapReturnsFunctionNotSupported) {
  auto dev = open_card_device();
  if (!dev.has_value()) {
    GTEST_SKIP() << "No /dev/dri/card* available";
  }
  auto src = drm::scene::GbmSurfaceSource::create(*dev, good_config());
  if (!src.has_value()) {
    GTEST_SKIP() << "gbm_surface_create rejected the request: " << src.error().message();
  }

  // GbmSurfaceSource doesn't override the base's `map()` — it returns
  // function_not_supported, signalling "uncompositable" to the
  // composition fallback. Locked-front-buffer BOs live in
  // driver-allocated memory (often non-LINEAR) and aren't safe to
  // touch from the CPU through this path.
  auto m = (*src)->map(drm::MapAccess::Read);
  ASSERT_FALSE(m.has_value());
  EXPECT_EQ(m.error(), std::make_error_code(std::errc::function_not_supported));
}

TEST(SceneGbmSurfaceSource, ReleaseWithNullOpaqueIsSafe) {
  auto dev = open_card_device();
  if (!dev.has_value()) {
    GTEST_SKIP() << "No /dev/dri/card* available";
  }
  auto src = drm::scene::GbmSurfaceSource::create(*dev, good_config());
  if (!src.has_value()) {
    GTEST_SKIP() << "gbm_surface_create rejected the request: " << src.error().message();
  }

  // The scene's cleanup path may call release() with a zero-init
  // AcquiredBuffer on shutdown after a failed acquire. Must not
  // crash; not even a debug-build assertion.
  drm::scene::AcquiredBuffer const dummy;
  (*src)->release(dummy);
}

// ── Session hooks ──────────────────────────────────────────────────────

TEST(SceneGbmSurfaceSource, SessionPauseClearsState) {
  auto dev = open_card_device();
  if (!dev.has_value()) {
    GTEST_SKIP() << "No /dev/dri/card* available";
  }
  auto src = drm::scene::GbmSurfaceSource::create(*dev, good_config());
  if (!src.has_value()) {
    GTEST_SKIP() << "gbm_surface_create rejected the request: " << src.error().message();
  }

  (*src)->on_session_paused();

  // After pause: native_surface() must report null (the underlying
  // gbm_surface was torn down) and acquire() must return EAGAIN so
  // the scene drops this layer for the duration.
  EXPECT_EQ((*src)->native_surface(), nullptr);
  auto acq = (*src)->acquire();
  ASSERT_FALSE(acq.has_value());
  EXPECT_EQ(acq.error(), std::make_error_code(std::errc::resource_unavailable_try_again));
}

TEST(SceneGbmSurfaceSource, SessionResumeRebuildsAgainstNewDev) {
  auto dev = open_card_device();
  if (!dev.has_value()) {
    GTEST_SKIP() << "No /dev/dri/card* available";
  }
  auto src = drm::scene::GbmSurfaceSource::create(*dev, good_config());
  if (!src.has_value()) {
    GTEST_SKIP() << "gbm_surface_create rejected the request: " << src.error().message();
  }

  (*src)->on_session_paused();

  auto resumed = (*src)->on_session_resumed(*dev);
  ASSERT_TRUE(resumed.has_value()) << resumed.error().message();
  EXPECT_NE((*src)->native_surface(), nullptr);

  // Dimensions and fourcc must survive the resume verbatim — the
  // allocator's plane match keyed off the original SourceFormat.
  const auto fmt = (*src)->format();
  EXPECT_EQ(fmt.drm_fourcc, static_cast<std::uint32_t>(DRM_FORMAT_XRGB8888));
  EXPECT_EQ(fmt.width, 640U);
  EXPECT_EQ(fmt.height, 480U);
}

TEST(SceneGbmSurfaceSource, SessionResumeWithBadFdFails) {
  auto dev = open_card_device();
  if (!dev.has_value()) {
    GTEST_SKIP() << "No /dev/dri/card* available";
  }
  auto src = drm::scene::GbmSurfaceSource::create(*dev, good_config());
  if (!src.has_value()) {
    GTEST_SKIP() << "gbm_surface_create rejected the request: " << src.error().message();
  }

  (*src)->on_session_paused();
  auto stub = drm::Device::from_fd(-1);
  auto resumed = (*src)->on_session_resumed(stub);
  ASSERT_FALSE(resumed.has_value());
  EXPECT_EQ(resumed.error(), std::make_error_code(std::errc::bad_file_descriptor));
}