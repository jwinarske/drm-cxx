// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "composite_canvas.hpp"

#include "buffer_source.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/dumb/buffer.hpp>

#include <drm_fourcc.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <system_error>
#include <utility>

namespace drm::scene {

namespace {

// ARGB8888 layout in memory (little-endian byte order):
//   byte 0 = B, byte 1 = G, byte 2 = R, byte 3 = A.
// Pack helpers operate on the 32-bit word view; the blender writes
// through a uint32_t* row pointer so endianness matches naturally.
constexpr std::uint32_t pack_argb(std::uint8_t a, std::uint8_t r, std::uint8_t g,
                                  std::uint8_t b) noexcept {
  return (static_cast<std::uint32_t>(a) << 24U) | (static_cast<std::uint32_t>(r) << 16U) |
         (static_cast<std::uint32_t>(g) << 8U) | static_cast<std::uint32_t>(b);
}

constexpr std::uint8_t component_a(std::uint32_t pixel) noexcept {
  return static_cast<std::uint8_t>((pixel >> 24U) & 0xFFU);
}
constexpr std::uint8_t component_r(std::uint32_t pixel) noexcept {
  return static_cast<std::uint8_t>((pixel >> 16U) & 0xFFU);
}
constexpr std::uint8_t component_g(std::uint32_t pixel) noexcept {
  return static_cast<std::uint8_t>((pixel >> 8U) & 0xFFU);
}
constexpr std::uint8_t component_b(std::uint32_t pixel) noexcept {
  return static_cast<std::uint8_t>(pixel & 0xFFU);
}

// Premultiplied SRC_OVER:
//   out = src + dst * (1 - src_a)
// Both operands assumed premultiplied (rgb <= a). The (+127)/255
// rounding makes a fully-opaque source reproduce the source pixel
// exactly so identity blends incur no drift. Straight-alpha sources
// will saturate visibly because rgb is treated as already-scaled;
// callers must convert beforehand (or let the kernel scanout-blend
// after we hand them off).
std::uint32_t blend_pixel_over(std::uint32_t src, std::uint32_t dst) noexcept {
  const std::uint32_t sa = component_a(src);
  if (sa == 0U) {
    return dst;
  }
  if (sa == 0xFFU) {
    return src;
  }
  const std::uint32_t inv = 255U - sa;
  const std::uint32_t da = component_a(dst);
  const std::uint32_t out_r = component_r(src) + (((component_r(dst) * inv) + 127U) / 255U);
  const std::uint32_t out_g = component_g(src) + (((component_g(dst) * inv) + 127U) / 255U);
  const std::uint32_t out_b = component_b(src) + (((component_b(dst) * inv) + 127U) / 255U);
  // Coverage alpha so a series of partially-transparent SRC_OVERs
  // accumulates to the correct combined alpha. Saturates at 0xFF.
  const std::uint32_t out_a = sa + (((da * inv) + 127U) / 255U);
  return pack_argb(static_cast<std::uint8_t>(std::min(out_a, std::uint32_t{0xFFU})),
                   static_cast<std::uint8_t>(std::min(out_r, std::uint32_t{0xFFU})),
                   static_cast<std::uint8_t>(std::min(out_g, std::uint32_t{0xFFU})),
                   static_cast<std::uint8_t>(std::min(out_b, std::uint32_t{0xFFU})));
}

// Compute clipped source / dest spans for one axis. Returns false when
// the rect degenerates after clipping. dst_extent is the canvas
// dimension; src_extent is the source-buffer dimension.
struct AxisRange {
  std::uint32_t dst_start{0};  // first dst pixel touched
  std::uint32_t dst_end{0};    // one-past-last dst pixel (exclusive)
  std::uint32_t src_start{0};  // first src pixel sampled
  std::uint32_t src_span{0};   // number of src pixels covering [dst_start, dst_end)
};

bool clip_axis(std::int32_t dst_pos, std::uint32_t dst_size, std::uint32_t dst_extent,
               std::int32_t src_pos, std::uint32_t src_size, std::uint32_t src_extent,
               AxisRange& out) noexcept {
  if (dst_size == 0U || src_size == 0U) {
    return false;
  }
  // Clip src_rect against the source buffer first.
  const std::int32_t src_clip_left = std::max<std::int32_t>(0, -src_pos);
  const std::int32_t src_pos_clipped = src_pos + src_clip_left;
  if (src_pos_clipped >= static_cast<std::int32_t>(src_extent)) {
    return false;
  }
  const std::uint32_t src_remaining_after_left =
      (src_size > static_cast<std::uint32_t>(src_clip_left))
          ? (src_size - static_cast<std::uint32_t>(src_clip_left))
          : 0U;
  if (src_remaining_after_left == 0U) {
    return false;
  }
  const std::uint32_t src_extent_remaining =
      src_extent - static_cast<std::uint32_t>(src_pos_clipped);
  const std::uint32_t src_visible = std::min(src_remaining_after_left, src_extent_remaining);
  if (src_visible == 0U) {
    return false;
  }
  // Same clipping for dst against the canvas.
  const std::int32_t dst_clip_left = std::max<std::int32_t>(0, -dst_pos);
  const std::int32_t dst_pos_clipped = dst_pos + dst_clip_left;
  if (dst_pos_clipped >= static_cast<std::int32_t>(dst_extent)) {
    return false;
  }
  const std::uint32_t dst_remaining_after_left =
      (dst_size > static_cast<std::uint32_t>(dst_clip_left))
          ? (dst_size - static_cast<std::uint32_t>(dst_clip_left))
          : 0U;
  if (dst_remaining_after_left == 0U) {
    return false;
  }
  const std::uint32_t dst_extent_remaining =
      dst_extent - static_cast<std::uint32_t>(dst_pos_clipped);
  const std::uint32_t dst_visible = std::min(dst_remaining_after_left, dst_extent_remaining);
  if (dst_visible == 0U) {
    return false;
  }
  out.dst_start = static_cast<std::uint32_t>(dst_pos_clipped);
  out.dst_end = out.dst_start + dst_visible;
  out.src_start = static_cast<std::uint32_t>(src_pos_clipped);
  out.src_span = src_visible;
  return true;
}

// Map a destination index in [0, dst_visible) back to a source index
// in [src_start, src_start + src_span) via nearest-neighbour. Source
// rect can be smaller, equal, or larger than dst rect — same mapping
// works in every direction.
std::uint32_t map_src_index(std::uint32_t dst_idx, std::uint32_t dst_visible,
                            std::uint32_t src_span) noexcept {
  if (dst_visible <= 1U) {
    return 0U;
  }
  // Centre-aligned NN: pixel k of dst lands at (k + 0.5) * src/dst.
  return (dst_idx * src_span) / dst_visible;
}

bool format_supported(std::uint32_t fourcc) noexcept {
  return fourcc == DRM_FORMAT_ARGB8888 || fourcc == DRM_FORMAT_XRGB8888;
}

bool source_is_opaque(std::uint32_t fourcc) noexcept {
  return fourcc == DRM_FORMAT_XRGB8888;
}

// reinterpret_cast precludes constexpr evaluation, so this stays a
// runtime helper. The body is small enough that any optimizer inlines
// it; the cost of dropping constexpr is purely informational.
bool aligned4(const void* p) noexcept {
  return (reinterpret_cast<std::uintptr_t>(p) & 3U) == 0U;
}

// height * stride_bytes without overflow. Returns false (and out left
// untouched) on wrap. Both operands are uint32; the product can exceed
// 2^32 even on a 64-bit host for a deliberately-malformed caller, but
// std::size_t is 64-bit there so the math itself is safe — this guard
// matters for 32-bit builds where size_t is 32-bit and the product
// silently truncates.
bool checked_byte_extent(std::uint32_t height, std::uint32_t stride_bytes,
                         std::size_t& out) noexcept {
  std::size_t product = 0;
  if (__builtin_mul_overflow(static_cast<std::size_t>(height),
                             static_cast<std::size_t>(stride_bytes), &product)) {
    return false;
  }
  out = product;
  return true;
}

}  // namespace

drm::expected<std::unique_ptr<CompositeCanvas>, std::error_code> CompositeCanvas::create(
    const drm::Device& dev, const CompositeCanvasConfig& cfg) {
  if (cfg.canvas_width == 0U || cfg.canvas_height == 0U) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  drm::dumb::Config dumb_cfg;
  dumb_cfg.width = cfg.canvas_width;
  dumb_cfg.height = cfg.canvas_height;
  dumb_cfg.drm_format = DRM_FORMAT_ARGB8888;
  dumb_cfg.bpp = 32;
  dumb_cfg.add_fb = true;

  auto back = drm::dumb::Buffer::create(dev, dumb_cfg);
  if (!back) {
    return drm::unexpected<std::error_code>(back.error());
  }
  auto front = drm::dumb::Buffer::create(dev, dumb_cfg);
  if (!front) {
    return drm::unexpected<std::error_code>(front.error());
  }

  return std::unique_ptr<CompositeCanvas>(new CompositeCanvas(std::move(*back), std::move(*front),
                                                              cfg.canvas_width, cfg.canvas_height));
}

void CompositeCanvas::begin_frame() noexcept {
  back_index_ = 1U - back_index_;
}

void CompositeCanvas::clear() noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
  auto& buf = buffers_[back_index_];
  if (buf.empty() || buf.data() == nullptr) {
    return;
  }
  std::memset(buf.data(), 0, buf.size_bytes());
}

void CompositeCanvas::clear_into(drm::span<std::uint8_t> dst, std::uint32_t dst_stride_bytes,
                                 std::uint32_t dst_width, std::uint32_t dst_height, std::int32_t x,
                                 std::int32_t y, std::int32_t w, std::int32_t h) noexcept {
  if (dst.data() == nullptr || dst_width == 0U || dst_height == 0U || w <= 0 || h <= 0) {
    return;
  }
  // dst_width * 4 itself overflows for absurd widths; do the comparison
  // in 64-bit so a u32 wrap can't bypass the guard.
  if (static_cast<std::uint64_t>(dst_stride_bytes) < static_cast<std::uint64_t>(dst_width) * 4U) {
    return;
  }
  if (!aligned4(dst.data())) {
    return;
  }
  std::size_t dst_required = 0;
  if (!checked_byte_extent(dst_height, dst_stride_bytes, dst_required)) {
    return;
  }
  if (dst.size() < dst_required) {
    return;
  }
  // Promote to int64 for clip math so width + x can't overflow on
  // pathological dst rects.
  const std::int64_t x0 = std::max<std::int64_t>(0, x);
  const std::int64_t y0 = std::max<std::int64_t>(0, y);
  const std::int64_t x1 = std::min<std::int64_t>(dst_width, std::int64_t{x} + w);
  const std::int64_t y1 = std::min<std::int64_t>(dst_height, std::int64_t{y} + h);
  if (x1 <= x0 || y1 <= y0) {
    return;
  }
  const std::uint32_t stride_px = dst_stride_bytes / 4U;
  auto* base = reinterpret_cast<std::uint32_t*>(dst.data());
  const auto span = static_cast<std::size_t>(x1 - x0);
  for (std::int64_t row = y0; row < y1; ++row) {
    auto* dst_row =
        base + (static_cast<std::size_t>(row) * stride_px) + static_cast<std::size_t>(x0);
    std::fill_n(dst_row, span, 0U);
  }
}

void CompositeCanvas::clear_rect(std::int32_t x, std::int32_t y, std::int32_t w,
                                 std::int32_t h) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
  auto& buf = buffers_[back_index_];
  if (buf.empty() || buf.data() == nullptr) {
    return;
  }
  clear_into(drm::span<std::uint8_t>(buf.data(), buf.size_bytes()), buf.stride(), width_, height_,
             x, y, w, h);
}

void CompositeCanvas::blend_into(drm::span<std::uint8_t> dst, std::uint32_t dst_stride_bytes,
                                 std::uint32_t dst_width, std::uint32_t dst_height,
                                 const CompositeSrc& src, const CompositeRect& src_rect,
                                 const CompositeRect& dst_rect) noexcept {
  if (dst.data() == nullptr || dst_width == 0U || dst_height == 0U) {
    return;
  }
  // 64-bit comparisons on stride/extent so a u32 wrap can't bypass the
  // guard on pathological inputs (width near UINT32_MAX).
  if (static_cast<std::uint64_t>(dst_stride_bytes) < static_cast<std::uint64_t>(dst_width) * 4U) {
    return;
  }
  std::size_t dst_required = 0;
  if (!checked_byte_extent(dst_height, dst_stride_bytes, dst_required)) {
    return;
  }
  if (dst.size() < dst_required) {
    return;
  }
  if (!format_supported(src.drm_fourcc)) {
    return;
  }
  if (src.pixels.data() == nullptr) {
    return;
  }
  if (static_cast<std::uint64_t>(src.src_stride_bytes) <
      static_cast<std::uint64_t>(src.src_width) * 4U) {
    return;
  }
  std::size_t src_required = 0;
  if (!checked_byte_extent(src.src_height, src.src_stride_bytes, src_required)) {
    return;
  }
  if (src.pixels.size() < src_required) {
    return;
  }
  // The inner loop reinterpret_casts both spans to uint32_t*; UB on
  // unaligned bases. Real allocators (kernel dumb, GBM, libstdc++
  // std::vector) all alignof >= 4, but a third-party caller could pass
  // anything in. Bail rather than read undefined.
  if (!aligned4(dst.data()) || !aligned4(src.pixels.data())) {
    return;
  }

  AxisRange xr;
  AxisRange yr;
  if (!clip_axis(dst_rect.x, dst_rect.w, dst_width, src_rect.x, src_rect.w, src.src_width, xr)) {
    return;
  }
  if (!clip_axis(dst_rect.y, dst_rect.h, dst_height, src_rect.y, src_rect.h, src.src_height, yr)) {
    return;
  }

  const bool opaque_src = source_is_opaque(src.drm_fourcc);
  const std::uint32_t dst_visible_x = xr.dst_end - xr.dst_start;
  const std::uint32_t dst_visible_y = yr.dst_end - yr.dst_start;
  const std::uint32_t dst_stride_px = dst_stride_bytes / 4U;
  const std::uint32_t src_stride_px = src.src_stride_bytes / 4U;
  // No-scaling fast path: when src and dst have equal extents, we can
  // skip the per-pixel `(dst_idx * src_span) / dst_visible` mapping —
  // every (dx, dy) lands on exactly (xr.src_start + dx, yr.src_start + dy).
  // This is the case for every shipped consumer today (Blend2D / thorvg
  // emit at the layer's natural size, and the scene's dst_rect matches);
  // signage_player's clock + ticker hit it on every frame.
  const bool no_scale = (xr.src_span == dst_visible_x) && (yr.src_span == dst_visible_y);

  auto* dst_base = reinterpret_cast<std::uint32_t*>(dst.data());
  const auto* src_base = reinterpret_cast<const std::uint32_t*>(src.pixels.data());

  for (std::uint32_t dy = 0; dy < dst_visible_y; ++dy) {
    const std::uint32_t sy =
        yr.src_start + (no_scale ? dy : map_src_index(dy, dst_visible_y, yr.src_span));
    auto* dst_row = dst_base + (static_cast<std::size_t>(yr.dst_start + dy) * dst_stride_px);
    const auto* src_row = src_base + (static_cast<std::size_t>(sy) * src_stride_px);
    if (no_scale) {
      const auto* src_px = src_row + xr.src_start;
      auto* dst_px = dst_row + xr.dst_start;
      for (std::uint32_t dx = 0; dx < dst_visible_x; ++dx) {
        std::uint32_t s = src_px[dx];
        if (opaque_src) {
          s |= 0xFF000000U;
        }
        dst_px[dx] = blend_pixel_over(s, dst_px[dx]);
      }
    } else {
      for (std::uint32_t dx = 0; dx < dst_visible_x; ++dx) {
        const std::uint32_t sx = xr.src_start + map_src_index(dx, dst_visible_x, xr.src_span);
        std::uint32_t s = src_row[sx];
        if (opaque_src) {
          s |= 0xFF000000U;
        }
        auto* dst_px = dst_row + xr.dst_start + dx;
        *dst_px = blend_pixel_over(s, *dst_px);
      }
    }
  }
}

void CompositeCanvas::blend(const CompositeSrc& src, const CompositeRect& src_rect,
                            const CompositeRect& dst_rect) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
  auto& buf = buffers_[back_index_];
  if (buf.empty() || buf.data() == nullptr) {
    return;
  }
  blend_into(drm::span<std::uint8_t>(buf.data(), buf.size_bytes()), buf.stride(), width_, height_,
             src, src_rect, dst_rect);
}

std::uint32_t CompositeCanvas::drm_fourcc() noexcept {
  return DRM_FORMAT_ARGB8888;
}

std::uint64_t CompositeCanvas::modifier() noexcept {
  // Dumb buffers are linear by construction.
  return DRM_FORMAT_MOD_LINEAR;
}

void CompositeCanvas::on_session_paused() noexcept {
  // Drop fb_id / GEM handles without ioctls on either buffer — the
  // libseat-revoked fd can't service them. Both buffers need
  // forgetting; on_session_resumed re-allocates both.
  for (auto& buf : buffers_) {
    buf.forget();
  }
}

drm::expected<void, std::error_code> CompositeCanvas::on_session_resumed(
    const drm::Device& new_dev) {
  drm::dumb::Config dumb_cfg;
  dumb_cfg.width = width_;
  dumb_cfg.height = height_;
  dumb_cfg.drm_format = DRM_FORMAT_ARGB8888;
  dumb_cfg.bpp = 32;
  dumb_cfg.add_fb = true;

  for (auto& buf : buffers_) {
    auto fresh = drm::dumb::Buffer::create(new_dev, dumb_cfg);
    if (!fresh) {
      return drm::unexpected<std::error_code>(fresh.error());
    }
    buf = std::move(*fresh);
  }
  // Reset the ping-pong cursor so the first post-resume `begin_frame`
  // settles on a deterministic back. Either index is fine — both
  // buffers are freshly kernel-zeroed — but starting from a known
  // state keeps test traces predictable.
  back_index_ = 1;
  return {};
}

}  // namespace drm::scene
