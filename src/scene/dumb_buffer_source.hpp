// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// dumb_buffer_source.hpp — single-buffer LayerBufferSource backed by a
// drm::dumb::Buffer.
//
// The simplest possible source: one CPU-writable linear ARGB8888 buffer
// (or any format drm::dumb::Buffer accepts), created once at startup,
// reused every frame. acquire() returns its cached FB ID; release() is
// a no-op because there is nothing to hand back to. Suitable for:
//
//   * software-rendered cursors and CSD frames,
//   * test patterns and unit-test rigs,
//   * any layer whose content lives in CPU memory and doesn't need
//     double- or triple-buffering (single-producer, scanned out
//     asynchronously — torn writes are not a problem at the rate a CSD
//     frame is redrawn).
//
// For multi-buffered scanout where the producer is racing the display,
// reach for `GbmBufferSource` (N-slot ring) instead.

#pragma once

#include "buffer_source.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/dumb/buffer.hpp>

#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

/// `LayerBufferSource` backed by a single `drm::dumb::Buffer`. CPU
/// writes pixels directly into `pixels()`; the scene returns the same
/// cached FB ID on every `acquire()`. Suitable for software-rendered
/// cursors / CSDs / test patterns / signage layers — anything where
/// the producer is not racing scanout. For multi-buffered scanout,
/// reach for a ring source (planned: `GbmRingSource`).
class DumbBufferSource : public LayerBufferSource {
 public:
  /// Allocate a single dumb buffer of the given size and format. The
  /// buffer is zero-filled at creation (drm::dumb::Buffer guarantees
  /// this), so the first `acquire()` returns a fully-transparent image.
  [[nodiscard]] static drm::expected<std::unique_ptr<DumbBufferSource>, std::error_code> create(
      const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format);

  DumbBufferSource(const DumbBufferSource&) = delete;
  DumbBufferSource& operator=(const DumbBufferSource&) = delete;
  DumbBufferSource(DumbBufferSource&&) = delete;
  DumbBufferSource& operator=(DumbBufferSource&&) = delete;
  ~DumbBufferSource() override = default;

  // ── LayerBufferSource ──────────────────────────────────────────────
  [[nodiscard]] drm::expected<AcquiredBuffer, std::error_code> acquire() override;
  void release(AcquiredBuffer acquired) noexcept override;
  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return BindingModel::SceneSubmitsFbId;
  }
  [[nodiscard]] SourceFormat format() const noexcept override { return format_; }
  [[nodiscard]] std::optional<CpuMapping> cpu_mapping() const noexcept override;
  void on_session_paused() noexcept override;
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override;

  // ── Pixel access for CPU-side renderers ────────────────────────────
  /// Mutable view over the buffer's linear pixel storage. `stride()`
  /// bytes per row — usually `width() * 4` for ARGB8888 but padded on
  /// some drivers.
  [[nodiscard]] drm::span<std::uint8_t> pixels() noexcept {
    return {buffer_.data(), buffer_.size_bytes()};
  }

  /// Bytes per row in the buffer. Usually `width * 4` for ARGB/XRGB
  /// formats, but the kernel may pad it for alignment.
  [[nodiscard]] std::uint32_t stride() const noexcept { return buffer_.stride(); }

 private:
  DumbBufferSource(drm::dumb::Buffer buffer, SourceFormat format) noexcept
      : buffer_(std::move(buffer)), format_(format) {}

  drm::dumb::Buffer buffer_;
  SourceFormat format_{};
};

}  // namespace drm::scene
