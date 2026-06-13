// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// composite_canvas.hpp — software composition target for layers the
// allocator could not place on a hardware plane.
//
// implementation: a single full-screen ARGB8888 dumb buffer
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
// surface. Multi-canvas pooling would be needed
// only if a scene wants to keep multiple non-contiguous z-runs on
// distinct planes — the current shape handles every test_patterns /
// signage_player / thorvg_janitor configuration.

#pragma once

#include "composition_target.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/dumb/buffer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace drm {
class Device;
}  // namespace drm

namespace drm::display {
class ToneMapper;
}  // namespace drm::display

namespace drm::scene {

struct CompositeCanvasConfig {
  /// How many ARGB8888 dumb buffers the canvas pool may allocate.
  /// V1 honors up to 1; reserved for follow-up that
  /// supports multiple non-contiguous z-runs on distinct planes.
  std::uint32_t max_canvases{1};

  /// Dimensions of the canvas. Normally the scene's CRTC mode size;
  /// exposed here so headless / offscreen scenes can size it
  /// explicitly.
  std::uint32_t canvas_width{0};
  std::uint32_t canvas_height{0};

  /// DRM FourCC the canvas dumb buffers (and thus the armed scanout FB)
  /// are allocated in. 0 => DRM_FORMAT_ARGB8888 (the default, byte-for-
  /// byte the internal blend format). Set this to a format the target
  /// plane actually scans out when ARGB8888 isn't advertised — minimal
  /// controllers such as tilcdc expose only XBGR8888 / RGB565. The
  /// internal blend always runs in ARGB8888; flush() converts to this
  /// output format per row. Supported: ARGB/XRGB/XBGR/ABGR8888 and
  /// RGB565 / BGR565; anything else fails create() with invalid_argument.
  std::uint32_t output_fourcc{0};
};

// CompositeSrc / CompositeRect are defined in composition_target.hpp (shared
// with the GPU compositor) and visible via the include above.

/// Software composition target for layers the allocator could not
/// place on a hardware plane. Owns a pair of ARGB8888 dumb buffers and
/// ping-pongs between them per frame; the CPU paints into the back
/// while the kernel scans the front. The static `blend_into` /
/// `clear_into` helpers are exposed so unit tests can exercise the
/// blend math against stack buffers without a live DRM device.
class CompositeCanvas : public CompositionTarget {
 public:
  /// Allocate a pair of ARGB8888 dumb buffers at the requested size.
  /// Width / height come from `cfg.canvas_width / canvas_height`;
  /// `max_canvases` is honored up to 1 for v1 (controls how many
  /// distinct canvas surfaces, not the per-canvas buffer count — the
  /// double-buffering above is unconditional).
  [[nodiscard]] static drm::expected<std::unique_ptr<CompositeCanvas>, std::error_code> create(
      const drm::Device& dev, const CompositeCanvasConfig& cfg);

  CompositeCanvas(const CompositeCanvas&) = delete;
  CompositeCanvas& operator=(const CompositeCanvas&) = delete;
  CompositeCanvas(CompositeCanvas&&) = delete;
  CompositeCanvas& operator=(CompositeCanvas&&) = delete;
  ~CompositeCanvas() override = default;

  /// Zero-fill the canvas (transparent black, all bytes = 0). Cheap;
  /// the caller invokes this once per frame before blending the
  /// per-layer sources on top.
  void clear() noexcept override;

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
  /// colors (rgb saturates instead of mixing); convert before passing
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

  /// Convert one row of `pixel_count` ARGB8888 pixels (`src`, memory
  /// order B,G,R,A) into `out_fourcc`, writing to `dst`. This is the
  /// shadow→scanout step flush() applies per dirty row so the canvas can
  /// land on a plane that doesn't advertise ARGB8888. Supported
  /// `out_fourcc`: ARGB/XRGB8888 (straight copy), XBGR/ABGR8888 (R↔B
  /// swap), RGB565 / BGR565 (16bpp pack). Any other value falls back to a
  /// 32bpp copy. Exposed static so unit tests can verify the packing
  /// without a live DRM device.
  static void convert_row(std::uint8_t* dst, const std::uint8_t* src, std::int32_t pixel_count,
                          std::uint32_t out_fourcc) noexcept;

  /// CPU SRC_OVER blend: dst.argb = src.argb OVER dst.argb at
  /// `dst_rect`, sampling from `src.pixels` at `src_rect`. Both rects
  /// are clipped against their respective buffer extents; mismatched
  /// sizes use nearest-neighbour scaling. No-op when the source format
  /// isn't supported (XRGB / ARGB8888 only in v1) or either rect
  /// degenerates to zero area after clipping.
  void blend(const CompositeSrc& src, const CompositeRect& src_rect,
             const CompositeRect& dst_rect) noexcept override;

  /// Copy this frame's painted shadow buffer into the back dumb buffer
  /// in one bulk memcpy. clear() / blend() write into a cached
  /// userspace shadow; the dumb buffer is typically mapped
  /// write-combined by the kernel, so per-pixel writes inside the blend
  /// loop are slow but a single linear memcpy hits the WC write
  /// streaming bandwidth ceiling. Call once per frame after the last
  /// blend(), before arming the canvas plane. Always succeeds (returns
  /// {}); the expected return exists to satisfy the CompositionTarget
  /// interface, whose GPU implementation can fail on swap/acquire.
  [[nodiscard]] drm::expected<void, std::error_code> flush() noexcept override;

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
  void begin_frame() noexcept override;

  /// Returns the fb_id of the buffer we're currently painting into
  /// (i.e. the back). After `begin_frame`, this is what the scene
  /// arms onto the canvas plane — once committed, the kernel's next
  /// vblank flips scanout to that buffer and `begin_frame` will swap
  /// roles again on the next frame.
  [[nodiscard]] std::uint32_t fb_id() const noexcept override {
    // back_index_ is bounded to {0, 1} by construction (begin_frame
    // is the only mutator and just toggles between the two values),
    // so .at()'s bounds check is always-true overhead. NOLINT
    // confined to this single read so cppcoreguidelines doesn't have
    // to special-case the invariant.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    return buffers_[back_index_].fb_id();
  }
  /// Canvas width in pixels (CRTC mode size by default; explicit for
  /// headless / offscreen scenes).
  [[nodiscard]] std::uint32_t width() const noexcept override { return width_; }
  /// Canvas height in pixels.
  [[nodiscard]] std::uint32_t height() const noexcept override { return height_; }
  /// Bytes per row of the back buffer. Both buffers were allocated to
  /// the same size; the kernel may pad above `width * 4`.
  [[nodiscard]] std::uint32_t stride_bytes() const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    return buffers_[back_index_].stride();
  }

  /// DRM FourCC the canvas dumb buffers / scanout FB are allocated in —
  /// the negotiated `CompositeCanvasConfig::output_fourcc` (ARGB8888 by
  /// default). The internal blend is always ARGB8888 regardless; this is
  /// what the armed plane scans out.
  [[nodiscard]] std::uint32_t drm_fourcc() const noexcept override { return output_fourcc_; }
  /// DRM modifier for canvas pixels (`DRM_FORMAT_MOD_LINEAR`; dumb
  /// buffers are linear by construction).
  [[nodiscard]] static std::uint64_t modifier() noexcept;

  /// True when the canvas can be armed — both buffers were allocated
  /// and neither has been forgotten via `on_session_paused`.
  [[nodiscard]] bool armable() const noexcept override {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    return !buffers_[0].empty() && !buffers_[1].empty() && buffers_[back_index_].fb_id() != 0;
  }

  // ── Session hooks ────────────────────────────────────────────────────

  /// Drop both dumb buffers' GEM/FB state without ioctls — the
  /// libseat-revoked fd cannot service them. Pairs with
  /// `on_session_resumed`. After this call `armable()` is false until
  /// the resume completes.
  void on_session_paused() noexcept override;

  /// Re-allocate both ARGB8888 dumb buffers against `new_dev`.
  /// Dimensions and back/front orientation are preserved; pixel
  /// content is not. Caller's responsibility to repaint.
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override;

  /// Half-open axis-aligned rect, in canvas pixels. Used internally by
  /// clear/blend/flush to amortize the per-frame WC memcpy down to just
  /// the touched region; declared public because the cpp-local helpers
  /// (dirty_union, clip_to_canvas) live in an anonymous namespace and
  /// reach the type via fully-qualified name.
  struct DirtyRect {
    std::int32_t x{0};
    std::int32_t y{0};
    std::int32_t w{0};
    std::int32_t h{0};
    [[nodiscard]] bool empty() const noexcept { return w <= 0 || h <= 0; }
  };

 private:
  CompositeCanvas(drm::dumb::Buffer back, drm::dumb::Buffer front, std::uint32_t width,
                  std::uint32_t height, std::uint32_t output_fourcc,
                  std::uint32_t output_bpp) noexcept
      : buffers_{std::move(back), std::move(front)},
        width_(width),
        height_(height),
        output_fourcc_(output_fourcc),
        output_bpp_(output_bpp) {}

  std::array<drm::dumb::Buffer, 2> buffers_;
  // Cached userspace shadow buffer. clear() / blend() write here
  // instead of into the WC dumb buffer; flush() memcpys it out in one
  // sequential write per frame. Allocated lazily on first paint op so
  // we don't pay for it before composition is needed; stride matches
  // the dumb buffer's stride so the memcpy lines up byte-for-byte.
  std::vector<std::uint8_t> shadow_;
  std::uint32_t shadow_stride_bytes_{0};

  // Accumulates blend() dst_rects (and full canvas on clear()) for the
  // frame currently being painted. flush() reads + clears this.
  DirtyRect current_dirty_{};
  // Per-back-buffer record of what the previous flush wrote into each
  // dumb buffer. flush memcpys union(current_dirty_, prev_flush_[idx])
  // so stale pixels left over from the previous use of this same back
  // are overwritten with the shadow's now-zero content.
  std::array<DirtyRect, 2> prev_flush_{};
  // Index of the back buffer (the one currently being painted).
  // `begin_frame` flips this between 0 and 1; the front is
  // `1 - back_index_`. Initialized to 1 so the very first
  // `begin_frame` swaps to 0 — the order itself doesn't matter, only
  // that consecutive frames target opposite buffers.
  std::size_t back_index_{1};
  std::uint32_t width_{0};
  std::uint32_t height_{0};
  // Output (scanout) pixel format + its bytes-per-pixel. The dumb
  // buffers are allocated in this format; flush() converts the always-
  // ARGB8888 shadow into it per row. Defaults to ARGB8888 / 4.
  std::uint32_t output_fourcc_{0};
  std::uint32_t output_bpp_{4};

  // Ensure the shadow buffer is allocated and sized to match the
  // current back buffer's stride. Returns false if the buffer pair
  // isn't usable (e.g. post-session-pause); callers degrade to a no-op.
  [[nodiscard]] bool ensure_shadow() noexcept;
};

}  // namespace drm::scene
