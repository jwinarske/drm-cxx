// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "dumb_buffer_source.hpp"

#include "buffer_source.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/dumb/buffer.hpp>

#include <drm_fourcc.h>

#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>

namespace drm::scene {

namespace {

// dispatch the right `dumb::Buffer` factory for a given
// fourcc. Single-plane RGB / packed YUV go through the plain
// `create()`; semi-planar 4:2:0 (NV12 / NV21 / P010 / P012 / P016)
// goes through `create_planar()` which handles the over-allocation
// for the UV plane plus the multi-plane AddFB2 wiring.
[[nodiscard]] bool is_semiplanar_yuv(std::uint32_t fourcc) noexcept {
  return fourcc == DRM_FORMAT_NV12 || fourcc == DRM_FORMAT_NV21 || fourcc == DRM_FORMAT_P010 ||
         fourcc == DRM_FORMAT_P012 || fourcc == DRM_FORMAT_P016;
}

[[nodiscard]] drm::expected<drm::dumb::Buffer, std::error_code> allocate_for_format(
    const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format) {
  if (is_semiplanar_yuv(drm_format)) {
    return drm::dumb::Buffer::create_planar(dev, drm_format, width, height);
  }
  drm::dumb::Config cfg;
  cfg.width = width;
  cfg.height = height;
  cfg.drm_format = drm_format;
  cfg.bpp = 32;  // ARGB/XRGB packed; the only single-plane shape this source ships today.
  cfg.add_fb = true;
  return drm::dumb::Buffer::create(dev, cfg);
}

}  // namespace

drm::expected<std::unique_ptr<DumbBufferSource>, std::error_code> DumbBufferSource::create(
    const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format) {
  auto r = allocate_for_format(dev, width, height, drm_format);
  if (!r) {
    return drm::unexpected<std::error_code>(r.error());
  }

  SourceFormat fmt;
  fmt.drm_fourcc = drm_format;
  fmt.modifier = 0;  // DRM_FORMAT_MOD_LINEAR == 0; dumb buffers are always linear
  fmt.width = width;
  fmt.height = height;

  // Private constructor; wrap in unique_ptr manually (make_unique can't
  // reach the private ctor without a friend declaration, and polymorphic
  // LayerBufferSource consumers need heap lifetime anyway).
  return std::unique_ptr<DumbBufferSource>(new DumbBufferSource(std::move(*r), fmt));
}

drm::expected<AcquiredBuffer, std::error_code> DumbBufferSource::acquire() {
  if (buffer_.empty() || buffer_.fb_id() == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  AcquiredBuffer acq;
  acq.fb_id = buffer_.fb_id();
  acq.acquire_fence_fd = -1;
  acq.opaque = nullptr;
  return acq;
}

drm::expected<drm::BufferMapping, std::error_code> DumbBufferSource::map(drm::MapAccess access) {
  if (buffer_.empty() || buffer_.data() == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  // Dumb buffers are always linear by kernel construction, so this is
  // really a belt-and-braces check — `format_.modifier` is set to
  // DRM_FORMAT_MOD_LINEAR (0) at create() time. Kept symmetric with
  // GbmBufferSource so a future buffer source that returns a tiled
  // mapping by mistake gets caught here uniformly.
  if (format_.modifier != 0U /* DRM_FORMAT_MOD_LINEAR */ &&
      format_.modifier != ((1ULL << 56U) - 1U) /* DRM_FORMAT_MOD_INVALID */) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::function_not_supported));
  }
  return buffer_.map(access);
}

void DumbBufferSource::release(AcquiredBuffer /*acquired*/) noexcept {
  // Single-buffer source: the buffer is permanently owned by this
  // object and handed out again on the next acquire(). Nothing to
  // release.
}

void DumbBufferSource::on_session_paused() noexcept {
  // No outstanding ioctls; acquire() is driven by the scene's commit
  // path, which won't run while the scene is paused. State is safe
  // as-is until on_session_resumed re-allocates against a live fd.
}

drm::expected<void, std::error_code> DumbBufferSource::on_session_resumed(
    const drm::Device& new_dev) {
  // Old fd is dead: forget() drops GEM + FB handles without ioctls
  // (the kernel reclaims them on fd close) and munmaps the CPU
  // mapping. Snapshot the prior dimensions first — format_ is the
  // source of truth post-forget.
  const auto prev_width = format_.width;
  const auto prev_height = format_.height;
  const auto prev_format = format_.drm_fourcc;

  buffer_.forget();

  auto r = allocate_for_format(new_dev, prev_width, prev_height, prev_format);
  if (!r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  buffer_ = std::move(*r);
  // format_ is re-affirmed rather than reassigned; modifier stays 0
  // (linear), dimensions unchanged. Consumers depend on format()
  // returning the same SourceFormat post-resume.
  return {};
}

}  // namespace drm::scene
