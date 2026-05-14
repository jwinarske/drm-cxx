// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "gbm_surface_source.hpp"

#include "buffer_source.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/gbm/device.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <gbm.h>
#include <xf86drmMode.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace drm::scene {

namespace {

constexpr std::uint32_t k_default_usage = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;

[[nodiscard]] bool is_supported_single_plane_format(std::uint32_t fmt) noexcept {
  switch (fmt) {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_RGB565:
    case DRM_FORMAT_BGR565:
    case DRM_FORMAT_XRGB2101010:
    case DRM_FORMAT_ARGB2101010:
    case DRM_FORMAT_XBGR2101010:
    case DRM_FORMAT_ABGR2101010:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] std::error_code validate_config(const GbmSurfaceConfig& cfg) noexcept {
  if (cfg.width == 0U || cfg.height == 0U) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (cfg.drm_format == 0U) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (!is_supported_single_plane_format(cfg.drm_format)) {
    return std::make_error_code(std::errc::not_supported);
  }
  return {};
}

[[nodiscard]] std::error_code make_errno(int err) noexcept {
  return {err, std::generic_category()};
}

[[nodiscard]] drm::expected<struct gbm_surface*, std::error_code> create_gbm_surface(
    struct gbm_device* gdev, const GbmSurfaceConfig& cfg, std::uint32_t usage) {
  errno = 0;
  struct gbm_surface* surf = nullptr;

  if (cfg.modifier != DRM_FORMAT_MOD_INVALID) {
#if defined(HAVE_GBM_BO_CREATE_WITH_MODIFIERS2)
    const std::uint64_t mod = cfg.modifier;
    surf = gbm_surface_create_with_modifiers2(gdev, cfg.width, cfg.height, cfg.drm_format, &mod, 1,
                                              usage);
#else
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_supported));
#endif
  } else {
    surf = gbm_surface_create(gdev, cfg.width, cfg.height, cfg.drm_format, usage);
  }

  if (surf == nullptr) {
    const int err = errno;
    return drm::unexpected<std::error_code>(err != 0 ? make_errno(err)
                                                     : std::make_error_code(std::errc::io_error));
  }
  return surf;
}

[[nodiscard]] drm::expected<std::uint32_t, std::error_code> add_fb_for_bo(
    int drm_fd, struct gbm_bo* bo, std::uint32_t drm_format, std::uint32_t width,
    std::uint32_t height) noexcept {
  std::array<std::uint32_t, 4> handles{gbm_bo_get_handle(bo).u32, 0U, 0U, 0U};
  std::array<std::uint32_t, 4> strides{gbm_bo_get_stride(bo), 0U, 0U, 0U};
  std::array<std::uint32_t, 4> offsets{0U, 0U, 0U, 0U};
  const std::uint64_t modifier = gbm_bo_get_modifier(bo);
  std::array<std::uint64_t, 4> modifiers{modifier, 0U, 0U, 0U};
  const bool use_modifiers = modifier != DRM_FORMAT_MOD_INVALID;

  std::uint32_t fb_id = 0;
  const int rc = drmModeAddFB2WithModifiers(
      drm_fd, width, height, drm_format, handles.data(), strides.data(), offsets.data(),
      use_modifiers ? modifiers.data() : nullptr, &fb_id,
      use_modifiers ? DRM_MODE_FB_MODIFIERS : 0U);
  if (rc != 0 || fb_id == 0) {
    const int err = errno;
    return drm::unexpected<std::error_code>(make_errno(err != 0 ? err : EIO));
  }
  return fb_id;
}

}  // namespace

struct GbmSurfaceSource::Impl {
  // gbm_dev outlives surf — surf holds a back-reference into the
  // device, so teardown must release surf before resetting gbm_dev.
  // Optional so Impl can default-construct cleanly; populated by
  // GbmSurfaceSource::create after the device is built successfully.
  std::optional<drm::gbm::GbmDevice> gbm_dev{};
  struct gbm_surface* surf{nullptr};
  int drm_fd{-1};

  GbmSurfaceConfig cfg{};
  SourceFormat fmt{};
  std::uint32_t resolved_usage{k_default_usage};

  // fb_id per gbm_bo*. A gbm_surface rotates among 2–3 BOs in
  // practice; reusing the registration avoids RmFB/AddFB2 churn.
  std::unordered_map<struct gbm_bo*, std::uint32_t> fb_cache{};

  // BOs that have been locked via acquire() and not yet retired via
  // release(). On destruction or pause we release them back to the
  // surface (drop_locked_buffers) before dropping the cache.
  std::unordered_set<struct gbm_bo*> live_bos{};

  bool session_paused{false};

  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  ~Impl() { teardown(); }

  void teardown() noexcept {
    drop_live_bos();
    drop_fb_cache();
    if (surf != nullptr) {
      gbm_surface_destroy(surf);
      surf = nullptr;
    }
    gbm_dev.reset();
    drm_fd = -1;
  }

  void drop_live_bos() noexcept {
    if (surf == nullptr) {
      live_bos.clear();
      return;
    }
    for (auto* bo : live_bos) {
      gbm_surface_release_buffer(surf, bo);
    }
    live_bos.clear();
  }

  void drop_fb_cache() noexcept {
    if (drm_fd >= 0) {
      for (auto& [bo, fb_id] : fb_cache) {
        (void)bo;
        drmModeRmFB(drm_fd, fb_id);
      }
    }
    fb_cache.clear();
  }

  // Pause path. The DRM fd is going away — we must not issue any
  // ioctls against it, including the RmFB inside drop_fb_cache. The
  // gbm_surface_destroy call may issue drmCloseBufferHandle for BOs
  // it still owns; those calls go through the soon-dead fd and the
  // kernel will refuse them, which is harmless (the kernel reclaims
  // the handles on fd close anyway). Mirrors the posture
  // GbmBufferSource takes around gbm_device_destroy at resume time.
  void forget_for_pause() noexcept {
    live_bos.clear();
    fb_cache.clear();
    if (surf != nullptr) {
      gbm_surface_destroy(surf);
      surf = nullptr;
    }
    gbm_dev.reset();
    drm_fd = -1;
  }
};

GbmSurfaceSource::GbmSurfaceSource() : impl_(std::make_unique<Impl>()) {}
GbmSurfaceSource::~GbmSurfaceSource() = default;

drm::expected<std::unique_ptr<GbmSurfaceSource>, std::error_code> GbmSurfaceSource::create(
    const drm::Device& dev, const GbmSurfaceConfig& cfg) {
  if (auto ec = validate_config(cfg); ec) {
    return drm::unexpected<std::error_code>(ec);
  }
  const int drm_fd = dev.fd();
  if (drm_fd < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  auto gbm_dev = drm::gbm::GbmDevice::create(drm_fd);
  if (!gbm_dev) {
    return drm::unexpected<std::error_code>(gbm_dev.error());
  }

  const std::uint32_t usage = (cfg.usage != 0U) ? cfg.usage : k_default_usage;

  auto surf = create_gbm_surface(gbm_dev->raw(), cfg, usage);
  if (!surf) {
    return drm::unexpected<std::error_code>(surf.error());
  }

  std::unique_ptr<GbmSurfaceSource> src(new GbmSurfaceSource());
  src->impl_->gbm_dev.emplace(std::move(*gbm_dev));
  src->impl_->surf = *surf;
  src->impl_->drm_fd = drm_fd;
  src->impl_->cfg = cfg;
  src->impl_->resolved_usage = usage;
  src->impl_->fmt =
      SourceFormat{cfg.drm_format, cfg.modifier, cfg.width, cfg.height};
  return src;
}

struct gbm_surface* GbmSurfaceSource::native_surface() const noexcept {
  return impl_ ? impl_->surf : nullptr;
}

struct gbm_device* GbmSurfaceSource::native_device() const noexcept {
  if (!impl_ || !impl_->gbm_dev.has_value()) {
    return nullptr;
  }
  return impl_->gbm_dev->raw();
}

drm::expected<AcquiredBuffer, std::error_code> GbmSurfaceSource::acquire() {
  if (!impl_ || impl_->session_paused || impl_->surf == nullptr || impl_->drm_fd < 0) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  }

  errno = 0;
  struct gbm_bo* bo = gbm_surface_lock_front_buffer(impl_->surf);
  if (bo == nullptr) {
    // No front buffer yet — producer hasn't pushed a frame. EAGAIN is
    // a flow-control return; the scene re-acquires next vblank.
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  }

  std::uint32_t fb_id = 0;
  if (auto it = impl_->fb_cache.find(bo); it != impl_->fb_cache.end()) {
    fb_id = it->second;
  } else {
    auto fb = add_fb_for_bo(impl_->drm_fd, bo, impl_->fmt.drm_fourcc, impl_->fmt.width,
                            impl_->fmt.height);
    if (!fb) {
      gbm_surface_release_buffer(impl_->surf, bo);
      return drm::unexpected<std::error_code>(fb.error());
    }
    fb_id = *fb;
    impl_->fb_cache.emplace(bo, fb_id);
  }

  // Latch the BO's actual modifier on first acquire — the surface
  // factory may have resolved INVALID-or-unset modifier to a concrete
  // one. Subsequent acquires verify consistency.
  if (impl_->fmt.modifier == DRM_FORMAT_MOD_INVALID) {
    impl_->fmt.modifier = gbm_bo_get_modifier(bo);
  }

  impl_->live_bos.insert(bo);
  AcquiredBuffer acq;
  acq.fb_id = fb_id;
  acq.acquire_fence_fd = -1;
  acq.opaque = bo;
  return acq;
}

void GbmSurfaceSource::release(AcquiredBuffer acquired) noexcept {
  if (!impl_ || impl_->surf == nullptr) {
    return;
  }
  auto* bo = static_cast<struct gbm_bo*>(acquired.opaque);
  if (bo == nullptr) {
    return;
  }
  auto it = impl_->live_bos.find(bo);
  if (it == impl_->live_bos.end()) {
    return;
  }
  impl_->live_bos.erase(it);
  gbm_surface_release_buffer(impl_->surf, bo);
}

SourceFormat GbmSurfaceSource::format() const noexcept {
  return impl_ ? impl_->fmt : SourceFormat{};
}

void GbmSurfaceSource::on_session_paused() noexcept {
  if (!impl_) {
    return;
  }
  impl_->session_paused = true;
  impl_->forget_for_pause();
}

drm::expected<void, std::error_code> GbmSurfaceSource::on_session_resumed(
    const drm::Device& new_dev) {
  if (!impl_) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  const int new_drm_fd = new_dev.fd();
  if (new_drm_fd < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  auto gbm_dev = drm::gbm::GbmDevice::create(new_drm_fd);
  if (!gbm_dev) {
    return drm::unexpected<std::error_code>(gbm_dev.error());
  }

  auto surf = create_gbm_surface(gbm_dev->raw(), impl_->cfg, impl_->resolved_usage);
  if (!surf) {
    return drm::unexpected<std::error_code>(surf.error());
  }

  impl_->gbm_dev.emplace(std::move(*gbm_dev));
  impl_->surf = *surf;
  impl_->drm_fd = new_drm_fd;
  impl_->session_paused = false;
  // dimensions / fourcc / requested modifier are preserved. Reset the
  // latched modifier so the first post-resume acquire re-reads it
  // from the BO (the driver may resolve differently on a fresh fd).
  impl_->fmt.modifier = impl_->cfg.modifier;
  return {};
}

}  // namespace drm::scene