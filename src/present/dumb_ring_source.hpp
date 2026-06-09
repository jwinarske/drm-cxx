// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// dumb_ring_source.hpp — a multi-buffered software LayerBufferSource backed by
// a BufferRing. It is the canonical BufferRing consumer: it turns the ring's
// buffer-age + damage-union bookkeeping into per-frame FB_DAMAGE_CLIPS.
//
// Lives in present/ (not scene/) because it depends on present::BufferRing, and
// scene/ must not depend on present/ (the ScanoutProducer seam points the other
// way). It is still a scene::LayerBufferSource, so a LayerScene layer can hold
// one directly.
//
// Contract (the EGL_EXT_buffer_age pattern, for CPU producers):
//   * The app calls paint() once per frame with a callback. The callback gets
//     the leased buffer's CPU mapping and the *stale region* it must repaint
//     (Repaint: full for a fresh/too-stale slot, else the union of damage since
//     this slot was last scanned out), and returns what it changed this frame.
//   * The next LayerScene commit's acquire() scans out the painted slot and
//     reports the repainted region as the buffer's damage, so FB_DAMAGE_CLIPS
//     carries exactly what differs from this buffer's previous scanout.
//
// Single-threaded: drive paint() + the scene commit from one loop.

#pragma once

#include "buffer_ring.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/dumb/buffer.hpp>
#include <drm-cxx/scene/buffer_source.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <vector>

namespace drm {
class BufferMapping;
class Device;
}  // namespace drm

namespace drm::present {

class DumbRingSource : public scene::LayerBufferSource {
 public:
  /// Allocate a ring of up to `max_slots` linear dumb buffers (>=1). Slots are
  /// allocated lazily as the ring grows under contention; the first paint()
  /// allocates slot 0. `drm_format` must be a 32-bpp packed format (XRGB/ARGB).
  [[nodiscard]] static drm::expected<std::unique_ptr<DumbRingSource>, std::error_code> create(
      const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format,
      std::size_t max_slots = 3);

  DumbRingSource(const DumbRingSource&) = delete;
  DumbRingSource& operator=(const DumbRingSource&) = delete;
  DumbRingSource(DumbRingSource&&) = delete;
  DumbRingSource& operator=(DumbRingSource&&) = delete;
  ~DumbRingSource() override = default;

  /// Paint the next frame. `fn` receives the leased buffer's writable mapping
  /// and the stale region it must repaint, and returns the region it changed
  /// this frame (in buffer pixels). Returns
  /// `errc::resource_unavailable_try_again` when every slot is busy (retry next
  /// vblank) and propagates allocation/map failures. On success the painted
  /// slot becomes the next acquire()'s buffer.
  using PaintFn = std::function<std::vector<Rect>(drm::BufferMapping&, const Repaint&)>;
  [[nodiscard]] drm::expected<void, std::error_code> paint(const PaintFn& fn);

  // ── LayerBufferSource ──────────────────────────────────────────────
  [[nodiscard]] drm::expected<scene::AcquiredBuffer, std::error_code> acquire() override;
  void release(scene::AcquiredBuffer acquired) noexcept override;
  [[nodiscard]] scene::BindingModel binding_model() const noexcept override {
    return scene::BindingModel::SceneSubmitsFbId;
  }
  [[nodiscard]] scene::SourceFormat format() const noexcept override { return format_; }
  void on_session_paused() noexcept override;
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override;

 private:
  DumbRingSource(const drm::Device& dev, scene::SourceFormat format, std::size_t max_slots)
      : dev_(&dev), format_(format), ring_(max_slots) {}

  [[nodiscard]] drm::expected<void, std::error_code> ensure_slot(std::size_t slot);

  const drm::Device* dev_{nullptr};
  scene::SourceFormat format_{};
  BufferRing ring_;
  std::vector<std::optional<drm::dumb::Buffer>> buffers_;
  // The slot paint() just committed, awaiting the scene's acquire(). Reset on
  // acquire so each commit requires a fresh paint().
  std::optional<std::size_t> pending_slot_;
  std::vector<scene::DamageRect> pending_damage_;
};

}  // namespace drm::present
