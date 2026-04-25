// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "gbm_buffer_source.hpp"

#include "buffer_source.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/gbm/buffer.hpp>
#include <drm-cxx/gbm/device.hpp>

#include <gbm.h>

#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>

namespace drm::scene {

namespace {

drm::gbm::Config buffer_config(std::uint32_t width, std::uint32_t height,
                               std::uint32_t drm_format) noexcept {
  drm::gbm::Config cfg;
  cfg.width = width;
  cfg.height = height;
  cfg.drm_format = drm_format;
  cfg.usage = GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE;
  cfg.add_fb = true;
  cfg.map_cpu = true;
  return cfg;
}

}  // namespace

drm::expected<std::unique_ptr<GbmBufferSource>, std::error_code> GbmBufferSource::create(
    const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format) {
  auto gbm_dev = drm::gbm::GbmDevice::create(dev.fd());
  if (!gbm_dev) {
    return drm::unexpected<std::error_code>(gbm_dev.error());
  }

  auto buf = drm::gbm::Buffer::create(*gbm_dev, buffer_config(width, height, drm_format));
  if (!buf) {
    return drm::unexpected<std::error_code>(buf.error());
  }

  SourceFormat fmt;
  fmt.drm_fourcc = drm_format;
  fmt.modifier = buf->modifier();
  fmt.width = width;
  fmt.height = height;

  return std::unique_ptr<GbmBufferSource>(
      new GbmBufferSource(std::move(*gbm_dev), std::move(*buf), fmt));
}

drm::expected<AcquiredBuffer, std::error_code> GbmBufferSource::acquire() {
  if (buffer_.empty() || buffer_.fb_id() == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  AcquiredBuffer acq;
  acq.fb_id = buffer_.fb_id();
  acq.acquire_fence_fd = -1;
  acq.opaque = nullptr;
  return acq;
}

void GbmBufferSource::release(AcquiredBuffer /*acquired*/) noexcept {
  // Single-buffer: the BO is permanently owned by this source and
  // handed out again on the next acquire. Nothing to return.
}

void GbmBufferSource::on_session_paused() noexcept {
  // No outstanding ioctls; acquire() is driven by the scene's commit
  // path, which won't run while the scene is paused. State is safe
  // as-is until on_session_resumed re-allocates against a live fd.
}

drm::expected<void, std::error_code> GbmBufferSource::on_session_resumed(
    const drm::Device& new_dev) {
  // Old DRM fd is dead. Both the BO (via its backing gbm_device) and
  // the gbm_device itself are bound to that fd; issuing gbm_bo_unmap /
  // gbm_bo_destroy / gbm_device_destroy now would go through the
  // revoked fd. forget() drops the BO's handles without touching the
  // kernel; the gbm_device is then replaced wholesale below.
  const auto prev_width = format_.width;
  const auto prev_height = format_.height;
  const auto prev_format = format_.drm_fourcc;

  buffer_.forget();

  // Replace the gbm_device wholesale — the old one holds the now-dead
  // DRM fd. Move-assignment on GbmDevice calls gbm_device_destroy on
  // its current device; that's a library call against a cached fd
  // that the kernel has already reclaimed. In practice mesa's
  // gbm_device_destroy only closes internal state (no DRM ioctls), so
  // this is safe; revisit if a future mesa version issues ioctls from
  // destroy.
  auto new_gbm_dev = drm::gbm::GbmDevice::create(new_dev.fd());
  if (!new_gbm_dev) {
    return drm::unexpected<std::error_code>(new_gbm_dev.error());
  }
  gbm_dev_ = std::move(*new_gbm_dev);

  auto new_buf =
      drm::gbm::Buffer::create(gbm_dev_, buffer_config(prev_width, prev_height, prev_format));
  if (!new_buf) {
    return drm::unexpected<std::error_code>(new_buf.error());
  }
  buffer_ = std::move(*new_buf);
  // format_ is re-affirmed rather than reassigned; dimensions and
  // fourcc are unchanged, and the resolved modifier may legitimately
  // differ across fds (driver reconsiders allocation layout).
  format_.modifier = buffer_.modifier();
  return {};
}

}  // namespace drm::scene
