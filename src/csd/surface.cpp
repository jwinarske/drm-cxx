// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "surface.hpp"

#include "core/device.hpp"
#include "gbm/device.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/dumb/buffer.hpp>
#include <drm-cxx/gbm/buffer.hpp>
#include <drm-cxx/log.hpp>

#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <system_error>
#include <utility>
#include <variant>

namespace drm::csd {

namespace {

drm::unexpected<std::error_code> make_error(std::errc code) {
  return drm::unexpected<std::error_code>(std::make_error_code(code));
}

drm::unexpected<std::error_code> errno_or(int fallback) {
  return drm::unexpected<std::error_code>(
      std::error_code(errno != 0 ? errno : fallback, std::system_category()));
}

drm::expected<drm::gbm::Buffer, std::error_code> try_gbm(drm::gbm::GbmDevice& gbm,
                                                         const SurfaceConfig& cfg) {
  drm::gbm::Config gcfg;
  gcfg.width = cfg.width;
  gcfg.height = cfg.height;
  gcfg.drm_format = DRM_FORMAT_ARGB8888;
  // The explicit LINEAR modifier pins the layout; usage stays at
  // SCANOUT only. amdgpu mesa rejects GBM_BO_USE_LINEAR when an
  // explicit LINEAR modifier is present (treats it as redundant), and
  // rejects GBM_BO_USE_WRITE in every combination — WRITE is only
  // needed for gbm_bo_write(), and Buffer::map uses gbm_bo_map which
  // doesn't depend on it. RENDERING stays off; V1 is CPU-only.
  gcfg.usage = GBM_BO_USE_SCANOUT;
  gcfg.modifier = DRM_FORMAT_MOD_LINEAR;
  gcfg.add_fb = true;
  return drm::gbm::Buffer::create(gbm, gcfg);
}

drm::expected<drm::dumb::Buffer, std::error_code> try_dumb(drm::Device& device,
                                                           const SurfaceConfig& cfg) {
  drm::dumb::Config dcfg;
  dcfg.width = cfg.width;
  dcfg.height = cfg.height;
  dcfg.drm_format = DRM_FORMAT_ARGB8888;
  dcfg.bpp = 32;
  dcfg.add_fb = true;
  return drm::dumb::Buffer::create(device, dcfg);
}

}  // namespace

Surface::Surface(drm::Device* device, drm::gbm::Buffer buffer) noexcept
    : device_(device), backing_(std::move(buffer)) {}

Surface::Surface(drm::Device* device, drm::dumb::Buffer buffer) noexcept
    : device_(device), backing_(std::move(buffer)) {}

drm::expected<Surface, std::error_code> Surface::create(drm::Device& device,
                                                        drm::gbm::GbmDevice* gbm_device,
                                                        const SurfaceConfig& config) {
  if (config.width == 0 || config.height == 0) {
    return make_error(std::errc::invalid_argument);
  }

  if (gbm_device != nullptr) {
    auto gbm_buf = try_gbm(*gbm_device, config);
    if (gbm_buf.has_value()) {
      return Surface{&device, std::move(*gbm_buf)};
    }
    // Log + continue to the dumb fallback. GBM unavailability on a
    // legitimate KMS device is common (vgem, virgl headless, some
    // distro mesa builds without scanout-capable formats); failing
    // hard here would punish callers running on those targets.
    drm::log_warn("csd::Surface: GBM allocation failed ({}); falling back to dumb",
                  gbm_buf.error().message());
  }

  auto dumb_buf = try_dumb(device, config);
  if (!dumb_buf.has_value()) {
    return drm::unexpected<std::error_code>(dumb_buf.error());
  }
  return Surface{&device, std::move(*dumb_buf)};
}

drm::expected<Surface, std::error_code> Surface::create(drm::Device& device,
                                                        const SurfaceConfig& config) {
  return Surface::create(device, nullptr, config);
}

// Helpers below use std::get_if rather than std::visit. std::visit on a
// two-alternative variant compiles to the same dispatch, but its return
// path can throw std::bad_variant_access on a moved-from variant, so
// every accessor below would need a NOLINT for bugprone-exception-escape.
// The if/else-if chain provably can't throw, which is cleaner under
// noexcept and matches the project's preference for explicit branches.

bool Surface::empty() const noexcept {
  if (const auto* g = std::get_if<drm::gbm::Buffer>(&backing_)) {
    return g->empty();
  }
  if (const auto* d = std::get_if<drm::dumb::Buffer>(&backing_)) {
    return d->empty();
  }
  return true;
}

std::uint32_t Surface::fb_id() const noexcept {
  if (const auto* g = std::get_if<drm::gbm::Buffer>(&backing_)) {
    return g->fb_id();
  }
  if (const auto* d = std::get_if<drm::dumb::Buffer>(&backing_)) {
    return d->fb_id();
  }
  return 0;
}

std::uint32_t Surface::width() const noexcept {
  if (const auto* g = std::get_if<drm::gbm::Buffer>(&backing_)) {
    return g->width();
  }
  if (const auto* d = std::get_if<drm::dumb::Buffer>(&backing_)) {
    return d->width();
  }
  return 0;
}

std::uint32_t Surface::height() const noexcept {
  if (const auto* g = std::get_if<drm::gbm::Buffer>(&backing_)) {
    return g->height();
  }
  if (const auto* d = std::get_if<drm::dumb::Buffer>(&backing_)) {
    return d->height();
  }
  return 0;
}

std::uint32_t Surface::stride() const noexcept {
  if (const auto* g = std::get_if<drm::gbm::Buffer>(&backing_)) {
    return g->stride();
  }
  if (const auto* d = std::get_if<drm::dumb::Buffer>(&backing_)) {
    return d->stride();
  }
  return 0;
}

// V1 is ARGB8888-only; both backings allocate that format
// unconditionally. Static would let the analyzer simplify this call,
// but keeping it as a member matches the rest of the accessor surface
// and leaves room for a future per-buffer format.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::uint32_t Surface::format() const noexcept {
  return DRM_FORMAT_ARGB8888;
}

SurfaceBacking Surface::backing() const noexcept {
  return std::holds_alternative<drm::gbm::Buffer>(backing_) ? SurfaceBacking::Gbm
                                                            : SurfaceBacking::Dumb;
}

drm::expected<drm::BufferMapping, std::error_code> Surface::paint(drm::MapAccess access) {
  if (empty()) {
    return make_error(std::errc::bad_file_descriptor);
  }
  if (auto* g = std::get_if<drm::gbm::Buffer>(&backing_)) {
    return g->map(access);
  }
  return std::get<drm::dumb::Buffer>(backing_).map(access);
}

drm::expected<int, std::error_code> Surface::dma_buf_fd() const {
  if (empty()) {
    return make_error(std::errc::bad_file_descriptor);
  }
  if (const auto* gbm_buf = std::get_if<drm::gbm::Buffer>(&backing_)) {
    return gbm_buf->fd();
  }
  // Dumb path: PRIME-export the GEM handle. The kernel's prime export
  // ioctl needs the originating fd; Surface kept a Device pointer for
  // exactly this reason.
  if (device_ == nullptr) {
    return make_error(std::errc::bad_file_descriptor);
  }
  const auto& dumb_buf = std::get<drm::dumb::Buffer>(backing_);
  int dmabuf_fd = -1;
  errno = 0;
  const int rc = drmPrimeHandleToFD(device_->fd(), dumb_buf.handle(), O_CLOEXEC, &dmabuf_fd);
  if (rc != 0 || dmabuf_fd < 0) {
    return errno_or(EIO);
  }
  return dmabuf_fd;
}

void Surface::forget() noexcept {
  if (auto* g = std::get_if<drm::gbm::Buffer>(&backing_)) {
    g->forget();
    return;
  }
  if (auto* d = std::get_if<drm::dumb::Buffer>(&backing_)) {
    d->forget();
  }
}

}  // namespace drm::csd