// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// composite_canvas.hpp — software composition target for layers the
// allocator could not place on a hardware plane.
//
// Phase 2.3 implementation: a single full-screen ARGB8888 dumb buffer
// owned by the scene. When `LayerScene::do_commit` finds that the
// allocator left one or more layers unassigned, it clears the canvas,
// CPU-blends each unassigned layer's source pixels into it (in zpos
// order, SRC_OVER), and then arms the canvas onto a free plane via
// direct atomic property writes — without re-running the allocator.
//
// The canvas itself is fd-backed (drm::dumb::Buffer). It participates
// in the scene's session lifecycle: on_session_paused() forgets the
// fb_id / GEM handle without ioctls (the libseat-revoked fd can't
// service them), and on_session_resumed() re-allocates against the
// new fd.
//
// Single-canvas v1: the entire composition bucket is one full-screen
// surface. Multi-canvas pooling (Phase 2.3 follow-up) would be needed
// only if a scene wants to keep multiple non-contiguous z-runs on
// distinct planes — the current shape handles every test_patterns /
// signage_player / thorvg_janitor configuration.

#pragma once

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/dumb/buffer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

struct CompositeCanvasConfig {
  /// How many ARGB8888 dumb buffers the canvas pool may allocate.
  /// V1 honours up to 1; reserved for Phase 2.3 follow-up that
  /// supports multiple non-contiguous z-runs on distinct planes.
  std::uint32_t max_canvases{1};

  /// Dimensions of the canvas. Normally the scene's CRTC mode size;
  /// exposed here so headless / offscreen scenes can size it
  /// explicitly.
  std::uint32_t canvas_width{0};
  std::uint32_t canvas_height{0};
};

/// Source-pixel descriptor for blend operations. Packs the four
/// coordinates the blender needs into one struct so the entry-point
/// signature stays manageable.
struct CompositeSrc {
  /// CPU-mapped source pixels. Must be at least
  /// `src_height * src_stride_bytes` bytes.
  drm::span<const std::uint8_t> pixels;
  std::uint32_t src_stride_bytes{0};
  /// Source-buffer dimensions; the source rect is clamped against
  /// these.
  std::uint32_t src_width{0};
  std::uint32_t src_height{0};
  /// DRM FourCC of the source pixels. V1 supports DRM_FORMAT_ARGB8888
  /// (treated as straight alpha) and DRM_FORMAT_XRGB8888 (treated as
  /// fully opaque, alpha byte ignored). Other formats are no-ops.
  std::uint32_t drm_fourcc{0};
};

/// Rectangles passed to blend(). Both are signed because dst_rect can
/// land partially off-screen on either edge — the blender clips.
struct CompositeRect {
  std::int32_t x{0};
  std::int32_t y{0};
  std::uint32_t w{0};
  std::uint32_t h{0};
};

class CompositeCanvas {
 public:
  /// Allocate a single ARGB8888 dumb buffer at the requested size.
  /// Width / height come from `cfg.canvas_width / canvas_height`;
  /// `max_canvases` is honoured up to 1 for v1.
  [[nodiscard]] static drm::expected<std::unique_ptr<CompositeCanvas>, std::error_code> create(
      const drm::Device& dev, const CompositeCanvasConfig& cfg);

  CompositeCanvas(const CompositeCanvas&) = delete;
  CompositeCanvas& operator=(const CompositeCanvas&) = delete;
  CompositeCanvas(CompositeCanvas&&) = delete;
  CompositeCanvas& operator=(CompositeCanvas&&) = delete;
  ~CompositeCanvas() = default;

  /// Zero-fill the canvas (transparent black, all bytes = 0). Cheap;
  /// the caller invokes this once per frame before blending the
  /// per-layer sources on top.
  void clear() noexcept;

  /// Zero-fill only the rectangle `rect` (canvas coordinates,
  /// half-open extents). Bounds-clipped to the canvas; degenerate
  /// rects are no-ops. Used by `LayerScene` to scrub only the union
  /// of last-frame's and this-frame's blend region instead of the
  /// whole buffer — drops the per-frame memory-write cost from
  /// `width * height * 4` bytes to `dirty_w * dirty_h * 4` bytes.
  void clear_rect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) noexcept;

  // ── Pure-CPU helpers (testable without a live DRM fd) ────────────────
  // The buffer-bound methods above forward here. Exposed so unit tests
  // can exercise the blend / clear math against stack buffers, no DRM
  // device required.

  /// Premultiplied SRC_OVER blend into `dst` at `dst_rect`, sampling
  /// from `src` at `src_rect`. Both `dst` and ARGB8888 `src` pixels are
  /// assumed premultiplied — the formula is `out = src + dst*(1-src_a)`,
  /// which matches the kernel's default `pixel blend mode` of
  /// "Pre-multiplied" and the output convention of Blend2D / thorvg /
  /// Cairo / Skia. Straight-alpha sources will produce visibly wrong
  /// colours (rgb saturates instead of mixing); convert before passing
  /// in. XRGB8888 sources have their alpha byte forced to 0xFF (fully
  /// opaque) before blending. Both spans must be 4-byte-aligned;
  /// unaligned pointers cause the blend to bail without writes.
  static void blend_into(drm::span<std::uint8_t> dst, std::uint32_t dst_stride_bytes,
                         std::uint32_t dst_width, std::uint32_t dst_height, const CompositeSrc& src,
                         const CompositeRect& src_rect, const CompositeRect& dst_rect) noexcept;

  /// Zero-fill `dst[rect]`. Bounds-clipped to the dst extents.
  static void clear_into(drm::span<std::uint8_t> dst, std::uint32_t dst_stride_bytes,
                         std::uint32_t dst_width, std::uint32_t dst_height, std::int32_t x,
                         std::int32_t y, std::int32_t w, std::int32_t h) noexcept;

  /// CPU SRC_OVER blend: dst.argb = src.argb OVER dst.argb at
  /// `dst_rect`, sampling from `src.pixels` at `src_rect`. Both rects
  /// are clipped against their respective buffer extents; mismatched
  /// sizes use nearest-neighbour scaling. No-op when the source format
  /// isn't supported (XRGB / ARGB8888 only in v1) or either rect
  /// degenerates to zero area after clipping.
  void blend(const CompositeSrc& src, const CompositeRect& src_rect,
             const CompositeRect& dst_rect) noexcept;

  // ── Frame lifecycle ─────────────────────────────────────────────────
  //
  // The canvas owns two ARGB8888 dumb buffers and ping-pongs between
  // them so the CPU isn't painting into the same memory the display
  // is scanning out of. Every commit cycle:
  //
  //   1. begin_frame() — swap roles, future paint ops target the new
  //      back. The old back is now front and is what the kernel will
  //      scan out (after we arm it on a plane and the next vblank
  //      lands).
  //   2. clear() / clear_rect() / blend() — paint into the new back.
  //   3. fb_id() — returns the back's fb_id; the scene writes that to
  //      the canvas plane's FB_ID property in `arm_composition_canvas`.
  //
  // Single-buffer canvases tear visibly because the scanout reads
  // from the same memory the CPU is writing into; a horizontal seam
  // appears wherever the scanout caught us mid-paint. Double
  // buffering eliminates the race for the cost of a second
  // canvas-sized dumb buffer (3 MB at 1024×768, 33 MB at 4K).

  /// Swap the back/front roles before painting this frame's content.
  /// All subsequent paint operations (clear / clear_rect / blend)
  /// write into the new back. Idempotent within a single frame: each
  /// `compose_unassigned` call should `begin_frame` exactly once.
  void begin_frame() noexcept;

  /// Returns the fb_id of the buffer we're currently painting into
  /// (i.e. the back). After `begin_frame`, this is what the scene
  /// arms onto the canvas plane — once committed, the kernel's next
  /// vblank flips scanout to that buffer and `begin_frame` will swap
  /// roles again on the next frame.
  [[nodiscard]] std::uint32_t fb_id() const noexcept {
    // back_index_ is bounded to {0, 1} by construction (begin_frame
    // is the only mutator and just toggles between the two values),
    // so .at()'s bounds check is always-true overhead. NOLINT
    // confined to this single read so cppcoreguidelines doesn't have
    // to special-case the invariant.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    return buffers_[back_index_].fb_id();
  }
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] std::uint32_t stride_bytes() const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    return buffers_[back_index_].stride();
  }
  // Canvas pixel format and modifier are fixed for v1 (ARGB8888 linear),
  // so these are static — exposed via the canvas type rather than an
  // instance for clarity at the call site.
  [[nodiscard]] static std::uint32_t drm_fourcc() noexcept;
  [[nodiscard]] static std::uint64_t modifier() noexcept;

  /// True when the canvas can be armed — both buffers were allocated
  /// and neither has been forgotten via `on_session_paused`.
  [[nodiscard]] bool armable() const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    return !buffers_[0].empty() && !buffers_[1].empty() && buffers_[back_index_].fb_id() != 0;
  }

  // ── Session hooks ────────────────────────────────────────────────────
  void on_session_paused() noexcept;
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(const drm::Device& new_dev);

 private:
  CompositeCanvas(drm::dumb::Buffer back, drm::dumb::Buffer front, std::uint32_t width,
                  std::uint32_t height) noexcept
      : buffers_{std::move(back), std::move(front)}, width_(width), height_(height) {}

  std::array<drm::dumb::Buffer, 2> buffers_;
  // Index of the back buffer (the one currently being painted).
  // `begin_frame` flips this between 0 and 1; the front is
  // `1 - back_index_`. Initialized to 1 so the very first
  // `begin_frame` swaps to 0 — the order itself doesn't matter, only
  // that consecutive frames target opposite buffers.
  std::size_t back_index_{1};
  std::uint32_t width_{0};
  std::uint32_t height_{0};
};

}  // namespace drm::scene
