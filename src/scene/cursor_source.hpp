// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// cursor_source.hpp — LayerBufferSource that scans a drm::cursor::Cursor out as
// a scene layer.
//
// The legacy path drives a HW cursor plane directly (drm::cursor::Renderer).
// This source instead presents the cursor sprite as an ordinary scene layer:
// the sprite is blitted into a CPU-writable dumb ARGB8888 buffer that the scene
// places on a plane like any other layer. That is required on hardware with no
// CURSOR-type plane — e.g. rockchip VOP2, where the cursor must ride an OVERLAY
// plane and compositing it into the composition buffer does not work. Reserve
// the plane up front with LayerScene::set_external_reserved_planes, and position
// the layer's dst_rect at (pointer_x - hotspot_x(), pointer_y - hotspot_y()).
//
// Static cursors blit once at creation. Animated cursors advance per a monotonic
// clock: acquire() picks the current frame and re-blits (reporting full-frame
// damage) when it changes.

#pragma once

#include "buffer_source.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/cursor/cursor.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/dumb/buffer.hpp>
#include <drm-cxx/time/clock.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>

namespace drm {
class Device;
}  // namespace drm

namespace drm::cursor {
class Theme;
}  // namespace drm::cursor

namespace drm::scene {

/// `LayerBufferSource` backed by a `drm::cursor::Cursor` blitted into a dumb
/// ARGB8888 buffer. See the file header for the VOP2 / reserved-plane rationale.
class CursorSource : public LayerBufferSource {
 public:
  /// Wrap an already-loaded cursor. The buffer is sized to the cursor's largest
  /// frame; the first frame is blitted immediately.
  [[nodiscard]] static drm::expected<std::unique_ptr<CursorSource>, std::error_code> create(
      const drm::Device& dev, drm::cursor::Cursor cursor);

  /// Convenience: build a static cursor from raw ARGB8888 pixels (the
  /// themeless fallback) and wrap it. `pixels` is tightly packed, row-major,
  /// exactly `width * height` entries.
  [[nodiscard]] static drm::expected<std::unique_ptr<CursorSource>, std::error_code> create_argb(
      const drm::Device& dev, drm::span<const std::uint32_t> pixels, std::uint32_t width,
      std::uint32_t height, int xhot, int yhot);

  /// Convenience: resolve `cursor_name` in `theme` (honoring `preferred_theme`,
  /// else $XCURSOR_THEME) at `requested_size` and wrap the result.
  [[nodiscard]] static drm::expected<std::unique_ptr<CursorSource>, std::error_code>
  create_from_theme(const drm::Device& dev, const drm::cursor::Theme& theme,
                    std::string_view cursor_name, std::string_view preferred_theme,
                    std::uint32_t requested_size);

  CursorSource(const CursorSource&) = delete;
  CursorSource& operator=(const CursorSource&) = delete;
  CursorSource(CursorSource&&) = delete;
  CursorSource& operator=(CursorSource&&) = delete;
  ~CursorSource() override = default;

  /// Cursor hotspot, in buffer pixels. The caller positions the layer's
  /// dst_rect at (pointer - hotspot) so the active point tracks the pointer.
  [[nodiscard]] int hotspot_x() const noexcept { return xhot_; }
  [[nodiscard]] int hotspot_y() const noexcept { return yhot_; }
  [[nodiscard]] std::uint32_t width() const noexcept { return format_.width; }
  [[nodiscard]] std::uint32_t height() const noexcept { return format_.height; }

  // ── LayerBufferSource ──────────────────────────────────────────────
  [[nodiscard]] drm::expected<AcquiredBuffer, std::error_code> acquire() override;
  void release(AcquiredBuffer acquired) noexcept override;
  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return BindingModel::SceneSubmitsFbId;
  }
  [[nodiscard]] SourceFormat format() const noexcept override { return format_; }
  [[nodiscard]] drm::expected<drm::BufferMapping, std::error_code> map(
      drm::MapAccess access) override;
  void on_session_paused() noexcept override;
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override;

 private:
  CursorSource(drm::cursor::Cursor cursor, drm::dumb::Buffer buffer, SourceFormat format, int xhot,
               int yhot) noexcept
      : cursor_(std::move(cursor)),
        buffer_(std::move(buffer)),
        format_(format),
        xhot_(xhot),
        yhot_(yhot) {}

  // Blit `frame`'s pixels into the (transparent-cleared) dumb buffer.
  [[nodiscard]] drm::expected<void, std::error_code> blit(const drm::cursor::Frame& frame);

  drm::cursor::Cursor cursor_;
  drm::dumb::Buffer buffer_;
  SourceFormat format_{};
  int xhot_{0};
  int yhot_{0};

  drm::SteadyClock clock_{};
  std::optional<drm::Clock::time_point> start_;
  const std::uint32_t* last_frame_pixels_{nullptr};
};

}  // namespace drm::scene
