// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "composite_canvas.hpp"

#include "buffer_source.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/display/tone_mapper.hpp>
#include <drm-cxx/dumb/buffer.hpp>

#include <drm_fourcc.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <system_error>
#include <utility>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) — gates #if blocks; constexpr won't work.
#define DRM_CXX_HAS_NEON 1
#else
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) — gates #if blocks; constexpr won't work.
#define DRM_CXX_HAS_NEON 0
#endif

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
// apply a ToneMapper to a single u32 ARGB8888 pixel.
// The mapper operates on u64 (4 × u16 channels), so we expand
// each u8 channel to u16 by multiplying by 257 (which makes
// `0x00 → 0x0000` and `0xFF → 0xFFFF` exactly), invoke the
// mapper, then compress back via `(c + 128) / 257`.
std::uint32_t apply_tone_mapper_argb(std::uint32_t argb,
                                     const drm::display::ToneMapper& mapper) noexcept {
  const std::uint16_t a = component_a(argb);
  const std::uint16_t r = component_r(argb);
  const std::uint16_t g = component_g(argb);
  const std::uint16_t b = component_b(argb);
  const std::uint64_t packed =
      static_cast<std::uint64_t>(static_cast<std::uint16_t>(r * 257)) |
      (static_cast<std::uint64_t>(static_cast<std::uint16_t>(g * 257)) << 16U) |
      (static_cast<std::uint64_t>(static_cast<std::uint16_t>(b * 257)) << 32U) |
      (static_cast<std::uint64_t>(static_cast<std::uint16_t>(a * 257)) << 48U);
  const std::uint64_t out = mapper(packed);
  const auto out_r = static_cast<std::uint16_t>(out & 0xFFFFU);
  const auto out_g = static_cast<std::uint16_t>((out >> 16U) & 0xFFFFU);
  const auto out_b = static_cast<std::uint16_t>((out >> 32U) & 0xFFFFU);
  const auto out_a = static_cast<std::uint16_t>((out >> 48U) & 0xFFFFU);
  return pack_argb(static_cast<std::uint8_t>((out_a + 128U) / 257U),
                   static_cast<std::uint8_t>((out_r + 128U) / 257U),
                   static_cast<std::uint8_t>((out_g + 128U) / 257U),
                   static_cast<std::uint8_t>((out_b + 128U) / 257U));
}

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
  // Center-aligned NN: pixel k of dst lands at (k + 0.5) * src/dst.
  return (dst_idx * src_span) / dst_visible;
}

bool format_supported(std::uint32_t fourcc) noexcept {
  return fourcc == DRM_FORMAT_ARGB8888 || fourcc == DRM_FORMAT_XRGB8888;
}

bool source_is_opaque(std::uint32_t fourcc) noexcept {
  return fourcc == DRM_FORMAT_XRGB8888;
}

// Bytes per pixel of a canvas *output* (scanout) format, or 0 if the
// canvas can't emit it. The internal blend is always ARGB8888; these
// are the formats flush() knows how to convert that shadow into so the
// canvas can land on a plane that doesn't advertise ARGB8888.
std::uint32_t canvas_output_bpp(std::uint32_t fourcc) noexcept {
  switch (fourcc) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
      return 4U;
    case DRM_FORMAT_RGB565:
    case DRM_FORMAT_BGR565:
      return 2U;
    default:
      return 0U;
  }
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

#if DRM_CXX_HAS_NEON
// Pure-opaque copy: dst[i] = src[i] | 0xFF000000. 16 pixels per outer
// iteration to amortize per-cache-line WC write setup; the no-vector
// scalar tail handles 0..15 leftover pixels. The dst on this codebase's
// hot path is a write-combined dumb buffer mapping, where vector stores
// are dramatically faster than the same byte count via scalar `stp`.
inline void neon_row_pure_opaque(std::uint32_t* dst, const std::uint32_t* src,
                                 std::uint32_t n) noexcept {
  const uint32x4_t alpha = vdupq_n_u32(0xFF000000U);
  std::uint32_t i = 0;
  for (; i + 16U <= n; i += 16U) {
    uint32x4_t s0 = vld1q_u32(src + i);
    uint32x4_t s1 = vld1q_u32(src + i + 4U);
    uint32x4_t s2 = vld1q_u32(src + i + 8U);
    uint32x4_t s3 = vld1q_u32(src + i + 12U);
    vst1q_u32(dst + i, vorrq_u32(s0, alpha));
    vst1q_u32(dst + i + 4U, vorrq_u32(s1, alpha));
    vst1q_u32(dst + i + 8U, vorrq_u32(s2, alpha));
    vst1q_u32(dst + i + 12U, vorrq_u32(s3, alpha));
  }
  for (; i + 4U <= n; i += 4U) {
    vst1q_u32(dst + i, vorrq_u32(vld1q_u32(src + i), alpha));
  }
  for (; i < n; ++i) {
    dst[i] = src[i] | 0xFF000000U;
  }
}

// SRC_OVER blend, 8 pixels per iteration. `force_opaque_src` mirrors the
// scalar `opaque_src` branch that OR's 0xFF000000 onto the source before
// blending. `blend_pixel_over`'s premultiplied formula:
//   out_c = src_c + (dst_c * (255 - src_a) + 127) / 255
//   out_a = src_a + (dst_a * (255 - src_a) + 127) / 255  (saturated)
// implemented per-channel on widened u16 lanes. Saturation falls out of
// vqmovn_u16 (which saturates to 0xFF on overflow).
inline void neon_row_blend_over(std::uint32_t* dst, const std::uint32_t* src, std::uint32_t n,
                                bool force_opaque_src) noexcept {
  const uint8x8_t opaque_a = vdup_n_u8(0xFFU);
  const uint16x8_t round_127 = vdupq_n_u16(127U);
  std::uint32_t i = 0;
  for (; i + 8U <= n; i += 8U) {
    // De-interleave src and dst into planar B/G/R/A vectors of 8 bytes.
    uint8x8x4_t s = vld4_u8(reinterpret_cast<const std::uint8_t*>(src + i));
    uint8x8x4_t d = vld4_u8(reinterpret_cast<const std::uint8_t*>(dst + i));
    if (force_opaque_src) {
      s.val[3] = opaque_a;
    }
    // inv = 255 - src_a, widened to u16 for the multiply.
    const uint8x8_t inv8 = vsub_u8(opaque_a, s.val[3]);
    const uint16x8_t inv = vmovl_u8(inv8);
    // For each channel c: dst_scaled = (d_c * inv + 127) / 255.
    // The /255 approximation `(x + (x>>8) + 257>>1) >> 8` is exact for
    // x in [0, 65279] (the only range we hit: 255 * 255 = 65025).
    // Use vrshrq_n_u16(vmlal+... ) sequence with a careful constant.
    // Simpler: do a multiply then shift right by 8 with round.
    auto div255 = [](uint16x8_t x) noexcept -> uint16x8_t {
      // (x + (x >> 8) + 1) >> 8 — common /255 trick, exact for u8*u8.
      const uint16x8_t shifted = vshrq_n_u16(x, 8);
      return vshrq_n_u16(vaddq_u16(vaddq_u16(x, shifted), vdupq_n_u16(1U)), 8);
    };
    uint16x8_t bp = vmlal_u8(round_127, d.val[0], inv8);
    uint16x8_t gp = vmlal_u8(round_127, d.val[1], inv8);
    uint16x8_t rp = vmlal_u8(round_127, d.val[2], inv8);
    uint16x8_t ap = vmlal_u8(round_127, d.val[3], inv8);
    (void)inv;  // computed above; vmlal_u8 took inv8 directly.
    bp = div255(bp);
    gp = div255(gp);
    rp = div255(rp);
    ap = div255(ap);
    // Add src on top.
    bp = vaddq_u16(bp, vmovl_u8(s.val[0]));
    gp = vaddq_u16(gp, vmovl_u8(s.val[1]));
    rp = vaddq_u16(rp, vmovl_u8(s.val[2]));
    ap = vaddq_u16(ap, vmovl_u8(s.val[3]));
    // Saturate-narrow back to u8.
    uint8x8x4_t out;
    out.val[0] = vqmovn_u16(bp);
    out.val[1] = vqmovn_u16(gp);
    out.val[2] = vqmovn_u16(rp);
    out.val[3] = vqmovn_u16(ap);
    vst4_u8(reinterpret_cast<std::uint8_t*>(dst + i), out);
  }
  // Scalar tail for the last 0..7 pixels. `blend_pixel_over` is in the
  // anonymous-namespace context above.
  for (; i < n; ++i) {
    std::uint32_t s32 = src[i];
    if (force_opaque_src) {
      s32 |= 0xFF000000U;
    }
    dst[i] = blend_pixel_over(s32, dst[i]);
  }
}
#endif  // DRM_CXX_HAS_NEON

// R↔B channel swap: ARGB8888 (memory B,G,R,A) → XBGR/ABGR8888 (memory
// R,G,B,A). On NEON, vld4_u8 de-interleaves 8 pixels into planar B/G/R/A
// 8-byte lanes and vst4_u8 re-interleaves them with R and B lanes
// exchanged — the swap is free (just a different store order), no shifts
// or masks. The scalar tail (and the whole body on non-NEON builds)
// matches CompositeCanvas::convert_row's reference exactly. `__restrict`
// asserts the shadow (src) and dumb buffer (dst) never alias — they're
// always distinct allocations — so x86_64 / riscv64 autovectorize the
// scalar loop (pshufb / RVV) instead of serializing on a phantom hazard.
inline void row_swap_rb(std::uint8_t* __restrict dst, const std::uint8_t* __restrict src,
                        std::uint32_t n) noexcept {
  std::uint32_t i = 0;
#if DRM_CXX_HAS_NEON
  for (; i + 8U <= n; i += 8U) {
    const uint8x8x4_t s = vld4_u8(src + (i * 4U));
    uint8x8x4_t o;
    o.val[0] = s.val[2];  // R
    o.val[1] = s.val[1];  // G
    o.val[2] = s.val[0];  // B
    o.val[3] = s.val[3];  // A / X
    vst4_u8(dst + (i * 4U), o);
  }
#endif
  for (; i < n; ++i) {
    const std::uint32_t o = i * 4U;
    dst[o + 0U] = src[o + 2U];
    dst[o + 1U] = src[o + 1U];
    dst[o + 2U] = src[o + 0U];
    dst[o + 3U] = src[o + 3U];
  }
}

// ARGB8888 (memory B,G,R,A) → RGB565 / BGR565 pack. `swap_rb` true emits
// BGR565 (B in the high 5-bit field, R in the low). On NEON, vld4_u8
// gives planar channels; each is widened to u16, shifted to its field,
// and OR-combined, then 8 packed u16 are stored in one vst1q_u16. The
// scalar tail mirrors convert_row's reference bit-for-bit. `__restrict`
// (src/dst never alias) lets x86_64 / riscv64 autovectorize the fallback.
inline void row_pack_565(std::uint8_t* __restrict dst, const std::uint8_t* __restrict src,
                         std::uint32_t n, bool swap_rb) noexcept {
  std::uint32_t i = 0;
#if DRM_CXX_HAS_NEON
  for (; i + 8U <= n; i += 8U) {
    const uint8x8x4_t s = vld4_u8(src + (i * 4U));
    const uint8x8_t hi8 = swap_rb ? s.val[0] : s.val[2];  // B or R → top field
    const uint8x8_t lo8 = swap_rb ? s.val[2] : s.val[0];  // R or B → low field
    const uint16x8_t hi = vshlq_n_u16(vshrq_n_u16(vmovl_u8(hi8), 3), 11);
    const uint16x8_t grn = vshlq_n_u16(vshrq_n_u16(vmovl_u8(s.val[1]), 2), 5);
    const uint16x8_t lo = vshrq_n_u16(vmovl_u8(lo8), 3);
    vst1q_u16(reinterpret_cast<std::uint16_t*>(dst + (i * 2U)), vorrq_u16(vorrq_u16(hi, grn), lo));
  }
#endif
  for (; i < n; ++i) {
    const std::uint32_t s = i * 4U;
    const std::uint32_t b = src[s + 0U];
    const std::uint32_t g = src[s + 1U];
    const std::uint32_t r = src[s + 2U];
    const std::uint32_t hi5 = swap_rb ? b : r;
    const std::uint32_t lo5 = swap_rb ? r : b;
    const auto v =
        static_cast<std::uint16_t>(((hi5 >> 3U) << 11U) | ((g >> 2U) << 5U) | (lo5 >> 3U));
    const std::uint32_t d = i * 2U;
    dst[d + 0U] = static_cast<std::uint8_t>(v & 0xFFU);
    dst[d + 1U] = static_cast<std::uint8_t>(v >> 8U);
  }
}

}  // namespace

// Convert one row of `n` ARGB8888 shadow pixels (memory order B,G,R,A)
// into `out_fourcc`, writing to `dst`. Called per dirty row by flush();
// `out_fourcc` is one of the canvas_output_bpp()-supported formats (the
// create()-validated invariant), so the default arm never fires in
// practice — it falls back to a straight 32bpp copy for safety.
void CompositeCanvas::convert_row(std::uint8_t* dst, const std::uint8_t* src,
                                  std::int32_t pixel_count, std::uint32_t out_fourcc) noexcept {
  const auto count = static_cast<std::size_t>(pixel_count);
  const auto n = static_cast<std::uint32_t>(pixel_count);
  switch (out_fourcc) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
      std::memcpy(dst, src, count * 4U);
      return;
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
      row_swap_rb(dst, src, n);
      return;
    case DRM_FORMAT_RGB565:
    case DRM_FORMAT_BGR565:
      row_pack_565(dst, src, n, out_fourcc == DRM_FORMAT_BGR565);
      return;
    default:
      std::memcpy(dst, src, count * 4U);
      return;
  }
}

drm::expected<std::unique_ptr<CompositeCanvas>, std::error_code> CompositeCanvas::create(
    const drm::Device& dev, const CompositeCanvasConfig& cfg) {
  if (cfg.canvas_width == 0U || cfg.canvas_height == 0U) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  const std::uint32_t out_fourcc =
      (cfg.output_fourcc == 0U) ? DRM_FORMAT_ARGB8888 : cfg.output_fourcc;
  const std::uint32_t out_bpp = canvas_output_bpp(out_fourcc);
  if (out_bpp == 0U) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  drm::dumb::Config dumb_cfg;
  dumb_cfg.width = cfg.canvas_width;
  dumb_cfg.height = cfg.canvas_height;
  dumb_cfg.drm_format = out_fourcc;
  dumb_cfg.bpp = out_bpp * 8U;
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
                                                              cfg.canvas_width, cfg.canvas_height,
                                                              out_fourcc, out_bpp));
}

void CompositeCanvas::begin_frame() noexcept {
  back_index_ = 1U - back_index_;
}

bool CompositeCanvas::ensure_shadow() noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
  auto& buf = buffers_[back_index_];
  if (buf.empty() || buf.data() == nullptr) {
    return false;
  }
  // The shadow is always ARGB8888 (4 bpp) regardless of the output
  // format the dumb buffers were allocated in — flush() converts. Its
  // stride is its own tight width*4, decoupled from buf.stride() (which
  // may be a different bpp entirely for an RGB565 output).
  const std::uint32_t stride = width_ * 4U;
  const std::size_t bytes = static_cast<std::size_t>(stride) * height_;
  if (stride != shadow_stride_bytes_ || shadow_.size() != bytes) {
    shadow_.assign(bytes, 0U);
    shadow_stride_bytes_ = stride;
    // Stride / size changed → existing prev_flush_ rects are stale.
    prev_flush_[0] = {};
    prev_flush_[1] = {};
    current_dirty_ = {};
  }
  return true;
}

namespace {

// Clip a half-open rect to [0, width) × [0, height). Returns false if
// the clipped rect is degenerate. Used by the shadow clear / flush
// paths.
bool clip_to_canvas(const CompositeCanvas::DirtyRect& r, std::int32_t width, std::int32_t height,
                    CompositeCanvas::DirtyRect& out) noexcept {
  if (r.w <= 0 || r.h <= 0 || width <= 0 || height <= 0) {
    return false;
  }
  const std::int32_t x0 = std::max<std::int32_t>(0, r.x);
  const std::int32_t y0 = std::max<std::int32_t>(0, r.y);
  // r.x + r.w can overflow if r.x is near INT32_MAX and r.w is large.
  // Promote to int64 for the bound math.
  const std::int32_t x1 = static_cast<std::int32_t>(std::min<std::int64_t>(
      static_cast<std::int64_t>(width), static_cast<std::int64_t>(r.x) + r.w));
  const std::int32_t y1 = static_cast<std::int32_t>(std::min<std::int64_t>(
      static_cast<std::int64_t>(height), static_cast<std::int64_t>(r.y) + r.h));
  if (x1 <= x0 || y1 <= y0) {
    return false;
  }
  out.x = x0;
  out.y = y0;
  out.w = x1 - x0;
  out.h = y1 - y0;
  return true;
}

// Union two half-open rects. Empty operand short-circuits to the other.
CompositeCanvas::DirtyRect dirty_union(const CompositeCanvas::DirtyRect& a,
                                       const CompositeCanvas::DirtyRect& b) noexcept {
  if (a.empty()) {
    return b;
  }
  if (b.empty()) {
    return a;
  }
  const std::int32_t x0 = std::min(a.x, b.x);
  const std::int32_t y0 = std::min(a.y, b.y);
  const std::int32_t x1 = std::max(a.x + a.w, b.x + b.w);
  const std::int32_t y1 = std::max(a.y + a.h, b.y + b.h);
  return {x0, y0, x1 - x0, y1 - y0};
}

}  // namespace

void CompositeCanvas::clear() noexcept {
  if (!ensure_shadow()) {
    return;
  }
  // Zero only what we wrote last frame. The shadow is zero everywhere
  // else by invariant — the very first frame after `ensure_shadow`
  // reallocated the vector inherited zero-init from `assign`, and
  // subsequent frames stay zero outside `current_dirty_` because we
  // never wrote there.
  DirtyRect to_clear;
  if (!clip_to_canvas(current_dirty_, static_cast<std::int32_t>(width_),
                      static_cast<std::int32_t>(height_), to_clear)) {
    current_dirty_ = {};
    return;
  }
  // Row-by-row memset of the dirty band, in shadow_stride_bytes_-pixel
  // strides. Inner span is `to_clear.w * 4` bytes.
  const std::size_t row_bytes = static_cast<std::size_t>(to_clear.w) * 4U;
  for (std::int32_t row = to_clear.y; row < to_clear.y + to_clear.h; ++row) {
    auto* p = shadow_.data() + (static_cast<std::size_t>(row) * shadow_stride_bytes_) +
              (static_cast<std::size_t>(to_clear.x) * 4U);
    std::memset(p, 0, row_bytes);
  }
  current_dirty_ = {};
}

void CompositeCanvas::flush() noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
  auto& buf = buffers_[back_index_];
  if (buf.empty() || buf.data() == nullptr) {
    return;
  }
  const std::size_t shadow_bytes = static_cast<std::size_t>(shadow_stride_bytes_) * height_;
  if (shadow_.empty() || shadow_.size() != shadow_bytes) {
    return;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
  const DirtyRect flush_rect_unclipped = dirty_union(current_dirty_, prev_flush_[back_index_]);
  DirtyRect flush_rect;
  if (clip_to_canvas(flush_rect_unclipped, static_cast<std::int32_t>(width_),
                     static_cast<std::int32_t>(height_), flush_rect)) {
    for (std::int32_t row = flush_rect.y; row < flush_rect.y + flush_rect.h; ++row) {
      const auto src_off = (static_cast<std::size_t>(row) * shadow_stride_bytes_) +
                           (static_cast<std::size_t>(flush_rect.x) * 4U);
      const auto dst_off = (static_cast<std::size_t>(row) * buf.stride()) +
                           (static_cast<std::size_t>(flush_rect.x) * output_bpp_);
      convert_row(buf.data() + dst_off, shadow_.data() + src_off, flush_rect.w, output_fourcc_);
    }
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
  prev_flush_[back_index_] = current_dirty_;
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
  if (!ensure_shadow()) {
    return;
  }
  clear_into(drm::span<std::uint8_t>(shadow_.data(), shadow_.size()), shadow_stride_bytes_, width_,
             height_, x, y, w, h);
  // Track the cleared rect as dirty so flush() copies zeros over any
  // formerly-non-zero pixels in the back buffer that this clear
  // overwrote in the shadow.
  current_dirty_ = dirty_union(current_dirty_, DirtyRect{x, y, w, h});
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
  // KMS plane alpha is u16; collapse to the 0..255 range we blend in.
  // Round-to-nearest so the boundaries 0x0000 / 0xFFFF map exactly to
  // 0x00 / 0xFF and intermediate values stay symmetric.
  const auto plane_alpha8 = static_cast<std::uint32_t>(
      (static_cast<std::uint32_t>(src.plane_alpha) * 255U + 32767U) / 65535U);
  const bool modulate_alpha = plane_alpha8 < 0xFFU;
  const std::uint32_t dst_visible_x = xr.dst_end - xr.dst_start;
  const std::uint32_t dst_visible_y = yr.dst_end - yr.dst_start;
  const std::uint32_t dst_stride_px = dst_stride_bytes / 4U;
  const std::uint32_t src_stride_px = src.src_stride_bytes / 4U;
  // Premultiplied per-plane alpha modulation: scale every src channel
  // by plane_alpha8/255 in u32 math so rgb stays <= a (the SRC_OVER
  // formula in `blend_pixel_over` assumes premultiplied input).
  // (+127)/255 rounding matches `blend_pixel_over`'s identity-blend
  // behavior so plane_alpha=0xFFFF reproduces the un-modulated source
  // exactly.
  auto modulate_pixel = [plane_alpha8](std::uint32_t s) -> std::uint32_t {
    const std::uint32_t a = ((((s >> 24U) & 0xFFU) * plane_alpha8) + 127U) / 255U;
    const std::uint32_t r = ((((s >> 16U) & 0xFFU) * plane_alpha8) + 127U) / 255U;
    const std::uint32_t g = ((((s >> 8U) & 0xFFU) * plane_alpha8) + 127U) / 255U;
    const std::uint32_t b = ((s & 0xFFU) * plane_alpha8 + 127U) / 255U;
    return (a << 24U) | (r << 16U) | (g << 8U) | b;
  };
  // No-scaling fast path: when src and dst have equal extents, we can
  // skip the per-pixel `(dst_idx * src_span) / dst_visible` mapping —
  // every (dx, dy) lands on exactly (xr.src_start + dx, yr.src_start + dy).
  // This is the case for every shipped consumer today (Blend2D / thorvg
  // emit at the layer's natural size, and the scene's dst_rect matches);
  // signage_player's clock + ticker hit it on every frame.
  const bool no_scale = (xr.src_span == dst_visible_x) && (yr.src_span == dst_visible_y);
  // Pure-copy fast path: opaque source, no plane-alpha modulation, no
  // tone-mapping → every dst pixel is just `src | 0xFF000000`. Skips the
  // write-combined dst read in `blend_pixel_over` that dominates the
  // composition budget (~240 ns/px → ~5 ns/px). XRGB layers with no
  // tone-mapper hit this every frame on the signage / mdi demos.
  const auto* tm = src.tone_mapper;
  const bool pure_opaque_copy = opaque_src && !modulate_alpha && (tm == nullptr);

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
      if (pure_opaque_copy) {
#if DRM_CXX_HAS_NEON
        neon_row_pure_opaque(dst_px, src_px, dst_visible_x);
#else
        for (std::uint32_t dx = 0; dx < dst_visible_x; ++dx) {
          dst_px[dx] = src_px[dx] | 0xFF000000U;
        }
#endif
        continue;
      }
#if DRM_CXX_HAS_NEON
      // NEON fast path covers the common SRC_OVER case (no tone mapper,
      // no plane-alpha modulation). The premultiplied SRC_OVER recipe
      // matches `blend_pixel_over` per-channel; the tail is scalar.
      if (tm == nullptr && !modulate_alpha) {
        neon_row_blend_over(dst_px, src_px, dst_visible_x, opaque_src);
        continue;
      }
#endif
      for (std::uint32_t dx = 0; dx < dst_visible_x; ++dx) {
        std::uint32_t s = src_px[dx];
        if (opaque_src) {
          s |= 0xFF000000U;
        }
        if (tm != nullptr) {
          s = apply_tone_mapper_argb(s, *tm);
        }
        if (modulate_alpha) {
          s = modulate_pixel(s);
        }
        dst_px[dx] = blend_pixel_over(s, dst_px[dx]);
      }
    } else {
      if (pure_opaque_copy) {
        for (std::uint32_t dx = 0; dx < dst_visible_x; ++dx) {
          const std::uint32_t sx = xr.src_start + map_src_index(dx, dst_visible_x, xr.src_span);
          dst_row[xr.dst_start + dx] = src_row[sx] | 0xFF000000U;
        }
        continue;
      }
      for (std::uint32_t dx = 0; dx < dst_visible_x; ++dx) {
        const std::uint32_t sx = xr.src_start + map_src_index(dx, dst_visible_x, xr.src_span);
        std::uint32_t s = src_row[sx];
        if (opaque_src) {
          s |= 0xFF000000U;
        }
        if (tm != nullptr) {
          s = apply_tone_mapper_argb(s, *tm);
        }
        if (modulate_alpha) {
          s = modulate_pixel(s);
        }
        auto* dst_px = dst_row + xr.dst_start + dx;
        *dst_px = blend_pixel_over(s, *dst_px);
      }
    }
  }
}

void CompositeCanvas::blend(const CompositeSrc& src, const CompositeRect& src_rect,
                            const CompositeRect& dst_rect) noexcept {
  if (!ensure_shadow()) {
    return;
  }
  blend_into(drm::span<std::uint8_t>(shadow_.data(), shadow_.size()), shadow_stride_bytes_, width_,
             height_, src, src_rect, dst_rect);
  // Conservative dirty-rect accumulation: extend by the caller's
  // dst_rect (clipped against the canvas in flush()/clear()). blend_into
  // does its own clip and may degrade to a no-op, but tracking the
  // unclipped rect is fine — the flush path clips before memcpying and
  // an over-estimate just costs a slightly larger memcpy.
  current_dirty_ = dirty_union(
      current_dirty_, DirtyRect{dst_rect.x, dst_rect.y, static_cast<std::int32_t>(dst_rect.w),
                                static_cast<std::int32_t>(dst_rect.h)});
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
  dumb_cfg.drm_format = output_fourcc_;
  dumb_cfg.bpp = output_bpp_ * 8U;
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
