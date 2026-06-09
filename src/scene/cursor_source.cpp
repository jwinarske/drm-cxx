// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "cursor_source.hpp"

#include "buffer_source.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/cursor/cursor.hpp>
#include <drm-cxx/cursor/theme.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/dumb/buffer.hpp>

#include <drm_fourcc.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

namespace drm::scene {

namespace {

[[nodiscard]] drm::expected<drm::dumb::Buffer, std::error_code> allocate_argb(
    const drm::Device& dev, std::uint32_t width, std::uint32_t height) {
  drm::dumb::Config cfg;
  cfg.width = width;
  cfg.height = height;
  cfg.drm_format = DRM_FORMAT_ARGB8888;
  cfg.bpp = 32;
  cfg.add_fb = true;
  return drm::dumb::Buffer::create(dev, cfg);
}

}  // namespace

drm::expected<void, std::error_code> CursorSource::blit(const drm::cursor::Frame& frame) {
  if (buffer_.empty() || buffer_.data() == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  auto m = buffer_.map(drm::MapAccess::Write);
  const auto px = m.pixels();
  const auto stride = m.stride();
  std::memset(px.data(), 0, px.size());  // transparent everywhere first

  const std::uint32_t copy_w = std::min(frame.width, format_.width);
  const std::uint32_t copy_h = std::min(frame.height, format_.height);
  for (std::uint32_t y = 0; y < copy_h; ++y) {
    std::uint8_t* dst = px.data() + (static_cast<std::size_t>(y) * stride);
    const std::uint32_t* src_row =
        frame.pixels.data() + (static_cast<std::size_t>(y) * frame.width);
    std::memcpy(dst, src_row, static_cast<std::size_t>(copy_w) * 4U);
  }
  last_frame_pixels_ = frame.pixels.data();
  return {};
}

drm::expected<std::unique_ptr<CursorSource>, std::error_code> CursorSource::create(
    const drm::Device& dev, drm::cursor::Cursor cursor) {
  // Size the buffer to the largest frame (xcursor frames are usually uniform,
  // but be defensive) and capture the hotspot before we move the cursor in.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  for (std::size_t i = 0; i < cursor.frame_count(); ++i) {
    const auto& f = cursor.at(i);
    width = std::max(width, f.width);
    height = std::max(height, f.height);
  }
  if (width == 0 || height == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  const int xhot = cursor.first().xhot;
  const int yhot = cursor.first().yhot;

  auto buf = allocate_argb(dev, width, height);
  if (!buf) {
    return drm::unexpected<std::error_code>(buf.error());
  }

  SourceFormat fmt;
  fmt.drm_fourcc = DRM_FORMAT_ARGB8888;
  fmt.modifier = 0;  // DRM_FORMAT_MOD_LINEAR
  fmt.width = width;
  fmt.height = height;

  auto src = std::unique_ptr<CursorSource>(
      new CursorSource(std::move(cursor), std::move(*buf), fmt, xhot, yhot));
  if (auto r = src->blit(src->cursor_.first()); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  return src;
}

drm::expected<std::unique_ptr<CursorSource>, std::error_code> CursorSource::create_argb(
    const drm::Device& dev, drm::span<const std::uint32_t> pixels, std::uint32_t width,
    std::uint32_t height, int xhot, int yhot) {
  auto cursor = drm::cursor::Cursor::from_argb(pixels, width, height, xhot, yhot);
  if (!cursor) {
    return drm::unexpected<std::error_code>(cursor.error());
  }
  return create(dev, std::move(*cursor));
}

drm::expected<std::unique_ptr<CursorSource>, std::error_code> CursorSource::create_from_theme(
    const drm::Device& dev, const drm::cursor::Theme& theme, std::string_view cursor_name,
    std::string_view preferred_theme, std::uint32_t requested_size) {
  auto cursor = drm::cursor::Cursor::load(theme, cursor_name, preferred_theme, requested_size);
  if (!cursor) {
    return drm::unexpected<std::error_code>(cursor.error());
  }
  return create(dev, std::move(*cursor));
}

drm::expected<AcquiredBuffer, std::error_code> CursorSource::acquire() {
  if (buffer_.empty() || buffer_.fb_id() == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  // Animated cursors advance with the monotonic clock; static cursors keep the
  // frame blitted at creation. Re-blit only when the frame actually changes.
  if (cursor_.animated()) {
    if (!start_.has_value()) {
      start_ = clock_.now();
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock_.now() - *start_);
    const auto& frame = cursor_.frame_at(elapsed);
    if (frame.pixels.data() != last_frame_pixels_) {
      if (auto r = blit(frame); !r) {
        return drm::unexpected<std::error_code>(r.error());
      }
    }
  }
  AcquiredBuffer acq;
  acq.fb_id = buffer_.fb_id();
  acq.opaque = nullptr;
  // damage left empty == full-frame; cursor buffers are tiny, so a full upload
  // is cheap and avoids tracking sub-rect dirty regions across frames.
  return acq;
}

drm::expected<drm::BufferMapping, std::error_code> CursorSource::map(drm::MapAccess access) {
  if (buffer_.empty() || buffer_.data() == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  return buffer_.map(access);
}

void CursorSource::release(AcquiredBuffer /*acquired*/) noexcept {
  // Single-buffer source: the buffer is permanently owned and re-handed on the
  // next acquire(). Nothing to release.
}

void CursorSource::on_session_paused() noexcept {
  // No outstanding ioctls; acquire() runs only on the scene's commit path,
  // which is quiesced while paused.
}

drm::expected<void, std::error_code> CursorSource::on_session_resumed(const drm::Device& new_dev) {
  // Old fd is dead: forget() drops GEM + FB handles (kernel reclaims on close)
  // and munmaps. Re-allocate against the live fd and re-blit the first frame;
  // the animation clock restarts on the next acquire().
  buffer_.forget();
  auto buf = allocate_argb(new_dev, format_.width, format_.height);
  if (!buf) {
    return drm::unexpected<std::error_code>(buf.error());
  }
  buffer_ = std::move(*buf);
  start_.reset();
  last_frame_pixels_ = nullptr;
  return blit(cursor_.first());
}

}  // namespace drm::scene
