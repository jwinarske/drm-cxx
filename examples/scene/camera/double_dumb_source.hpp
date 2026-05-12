// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// double_dumb_source.hpp — example-local double-buffered scanout source.
//
// `drm::scene::DumbBufferSource` is single-buffered: the producer writes
// pixels into the same mapped region the scene scans out from. At
// webcam rates (30 fps into a 60 Hz display) the two never collide,
// but a libyuv tier converting at the display's vblank rate (1600x896
// YUY2 → XRGB at 60 fps on the bare Brio, --no-vaapi single cam) can
// overwrite the buffer mid-scanout and tear a horizontal seam.
//
// DoubleDumbSource owns two `drm::dumb::Buffer`s. `acquire()` returns
// the *front* FB (the one most recently published for scanout);
// `map(Write|ReadWrite)` returns the *back* mapping (the one the
// producer is allowed to clobber). After the producer finishes a frame
// it calls `publish()` to swap front and back. The producer is now
// writing into what was the front buffer; the scene is scanning out
// what was the back. No mid-frame overwrite is possible because the
// kernel page-flip semantics already guarantee the front buffer is
// stable until vblank+1.
//
// Scope: a private camera-example primitive, deliberately kept out of
// `src/scene/` so the in-tree library stays focused on widely-applicable
// sources. The library's planned `GbmRingSource` will eventually subsume
// this with a generalised N-buffer ring; until then this is the
// camera-side workaround.

#pragma once

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/dumb/buffer.hpp>
#include <drm-cxx/scene/buffer_source.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>

namespace drm {
class Device;
}  // namespace drm

namespace drm::examples::camera {

/// Two-buffer scanout source backed by a pair of `drm::dumb::Buffer`s.
/// CPU writes go to the back buffer via `map()`; `publish()` promotes
/// it to the front, where the next `acquire()` picks it up. Identical
/// `LayerBufferSource` contract as `drm::scene::DumbBufferSource` — the
/// scene doesn't know there's a second buffer.
class DoubleDumbSource : public drm::scene::LayerBufferSource {
 public:
  /// Allocate both dumb buffers of the given size and format. Both are
  /// zero-filled at creation, so `acquire()` before any `publish()`
  /// returns a fully-transparent buffer (same behaviour as
  /// DumbBufferSource).
  [[nodiscard]] static drm::expected<std::unique_ptr<DoubleDumbSource>, std::error_code> create(
      const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format);

  ~DoubleDumbSource() override = default;
  DoubleDumbSource(const DoubleDumbSource&) = delete;
  DoubleDumbSource& operator=(const DoubleDumbSource&) = delete;
  DoubleDumbSource(DoubleDumbSource&&) = delete;
  DoubleDumbSource& operator=(DoubleDumbSource&&) = delete;

  /// Promote the back buffer (most recently written via `map`) to the
  /// front. The caller-visible effect is that the next `acquire()`
  /// returns the just-published FB. The previous front buffer becomes
  /// the new back buffer (overwritable by the next `map`).
  void publish() noexcept;

  // ── LayerBufferSource overrides ────────────────────────────────────

  [[nodiscard]] drm::expected<drm::scene::AcquiredBuffer, std::error_code> acquire() override;
  void release(drm::scene::AcquiredBuffer acquired) noexcept override;
  [[nodiscard]] drm::scene::BindingModel binding_model() const noexcept override {
    return drm::scene::BindingModel::SceneSubmitsFbId;
  }
  [[nodiscard]] drm::scene::SourceFormat format() const noexcept override { return format_; }
  [[nodiscard]] drm::expected<drm::BufferMapping, std::error_code> map(
      drm::MapAccess access) override;
  void on_session_paused() noexcept override;
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override;

 private:
  DoubleDumbSource(drm::dumb::Buffer a, drm::dumb::Buffer b, drm::scene::SourceFormat fmt) noexcept
      : buffers_{std::move(a), std::move(b)}, format_(fmt) {}

  std::array<drm::dumb::Buffer, 2> buffers_;
  drm::scene::SourceFormat format_{};
  std::size_t front_idx_{0};  // buffer index acquire() returns
};

}  // namespace drm::examples::camera
