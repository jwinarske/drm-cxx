// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// composition_target.hpp — abstract composition surface for layers the
// allocator could not place on a hardware plane.
//
// LayerScene's composition fallback blends the unassigned layers into a
// composition target and arms its framebuffer onto a free plane. Two
// implementations exist behind this interface:
//
//   * CompositeCanvas (composite_canvas.hpp) — CPU SRC_OVER blend into a
//     dumb buffer. Always available; the only path on GPU-less builds.
//   * GlCompositor (gl_compositor.hpp) — GLES SRC_OVER blend into a
//     gbm_surface, used automatically when an EGL/GLES stack is present.
//     Compiled only under DRM_CXX_HAS_EGL.
//
// The per-frame drive sequence is identical for both:
//   begin_frame() -> clear() -> blend(...) per layer (zpos order) ->
//   flush() -> arm fb_id() onto the canvas plane.
//
// EGL-free and always compiled, so callers and the CPU path never depend
// on the GPU stack.

#pragma once

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <array>
#include <cstdint>
#include <system_error>

namespace drm {
class Device;
}  // namespace drm

namespace drm::display {
class ToneMapper;
}  // namespace drm::display

namespace drm::scene {

/// Source-pixel descriptor for blend operations. Packs the coordinates
/// the blender needs into one struct so the entry-point signature stays
/// manageable.
struct CompositeSrc {
  /// CPU-mapped source pixels. Must be at least
  /// `src_height * src_stride_bytes` bytes.
  drm::span<const std::uint8_t> pixels;
  std::uint32_t src_stride_bytes{0};
  /// Source-buffer dimensions; the source rect is clamped against these.
  std::uint32_t src_width{0};
  std::uint32_t src_height{0};
  /// DRM FourCC of the source pixels. ARGB8888 (straight alpha) and
  /// XRGB8888 (opaque, alpha ignored) are supported; other formats are
  /// no-ops.
  std::uint32_t drm_fourcc{0};
  /// Per-layer alpha modulation in KMS plane units (0 = transparent,
  /// 0xFFFF = opaque). Mirrors the per-plane "alpha" property so a
  /// composited layer matches a hardware-placed one. Default 0xFFFF.
  std::uint16_t plane_alpha{0xFFFF};
  /// Optional CPU tone-map applied per pixel before the SRC_OVER blend
  /// (CPU path only; the GPU path ignores it for now). nullptr leaves the
  /// source pixels untouched.
  const drm::display::ToneMapper* tone_mapper{nullptr};

  /// Optional direct DMA-BUF import path. When `dma_n_planes > 0`, the GPU
  /// compositor imports these planes as an EGLImage and samples them
  /// directly, ignoring `pixels` (which may be empty for a source with no
  /// CPU mapping — a camera / V4L2 layer that would otherwise blank). The
  /// CPU `CompositeCanvas` cannot import DMA-BUFs and skips such a source.
  /// The fds are borrowed (owned by the layer's source; not closed here);
  /// see `DmaBufDesc` in buffer_source.hpp. `dma_fourcc` is `drm_fourcc`.
  std::array<int, 4> dma_fds{{-1, -1, -1, -1}};
  std::array<std::uint32_t, 4> dma_offsets{};
  std::array<std::uint32_t, 4> dma_pitches{};
  std::uint32_t dma_n_planes{0};
  std::uint64_t dma_modifier{0};
};

/// Rectangles passed to blend(). Both are signed because dst_rect can
/// land partially off-screen on either edge — the blender clips.
struct CompositeRect {
  std::int32_t x{0};
  std::int32_t y{0};
  std::uint32_t w{0};
  std::uint32_t h{0};
};

/// Interface LayerScene's composition fallback drives. See file header for
/// the per-frame sequence. Implementations own a double-buffered scanout
/// target and expose its current framebuffer via fb_id().
class CompositionTarget {
 public:
  CompositionTarget() = default;
  virtual ~CompositionTarget() = default;
  CompositionTarget(const CompositionTarget&) = delete;
  CompositionTarget& operator=(const CompositionTarget&) = delete;
  CompositionTarget(CompositionTarget&&) = delete;
  CompositionTarget& operator=(CompositionTarget&&) = delete;

  /// Swap to the back buffer before painting this frame's content.
  virtual void begin_frame() noexcept = 0;

  /// Clear the back buffer to transparent black.
  virtual void clear() noexcept = 0;

  /// SRC_OVER blend `src[src_rect]` into the back buffer at `dst_rect`
  /// (premultiplied; per-layer alpha applied). No-op on unsupported
  /// source formats or degenerate rects.
  virtual void blend(const CompositeSrc& src, const CompositeRect& src_rect,
                     const CompositeRect& dst_rect) noexcept = 0;

  /// Publish this frame's back buffer so fb_id() points at it. The CPU
  /// path copies its shadow into the dumb buffer; the GPU path swaps and
  /// locks the front buffer. Returns an error if the publish failed
  /// (e.g. a GPU swap/acquire error) so the caller can drop the frame.
  [[nodiscard]] virtual drm::expected<void, std::error_code> flush() noexcept = 0;

  /// FB id of the buffer to arm onto the canvas plane this frame.
  [[nodiscard]] virtual std::uint32_t fb_id() const noexcept = 0;
  /// Canvas dimensions in pixels.
  [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;
  /// DRM FourCC the canvas / scanout FB is allocated in.
  [[nodiscard]] virtual std::uint32_t drm_fourcc() const noexcept = 0;
  /// True when the target can be armed (allocated, not session-paused).
  [[nodiscard]] virtual bool armable() const noexcept = 0;

  /// True when this target can composite a source directly from its
  /// DMA-BUF of `drm_fourcc` (the GPU EGLImage path) instead of reading
  /// CPU pixels — which lets the scene rescue a `map()`-less source (a
  /// camera / V4L2 layer) rather than dropping it. The CPU
  /// `CompositeCanvas` can't import DMA-BUFs, so it uses the default
  /// `false`. A `GlCompositor` returns true only for the formats its
  /// blend path handles.
  [[nodiscard]] virtual bool supports_dma_buf_import(std::uint32_t /*drm_fourcc*/) const noexcept {
    return false;
  }

  /// Drop GEM/FB state without ioctls — the revoked fd can't service them.
  virtual void on_session_paused() noexcept = 0;
  /// Re-allocate against `new_dev` after a session resume.
  [[nodiscard]] virtual drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) = 0;
};

}  // namespace drm::scene
