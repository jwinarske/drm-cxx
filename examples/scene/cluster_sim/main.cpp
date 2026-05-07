// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// cluster_sim — automotive instrument-cluster showcase. Currently:
// a Blend2D-painted radial-gradient backdrop plus animated speedometer
// and tachometer dial layers, a digital speed readout between them,
// a row of warning indicators below, and an optional rear-view camera
// layer driven by drm::scene::V4l2DecoderSource against vicodec
// (toggled with R).
//
// Layer stack (bottom up):
//   * Bg: full-screen XRGB8888 dumb buffer, painted once at startup
//     (radial gradient on a dark-blue backdrop).
//   * Speedometer dial: ARGB8888 dumb buffer, repainted each frame
//     with an animated needle. Positioned in the screen's left third.
//   * Tachometer dial: same shape, screen's right third, slightly
//     out-of-phase animation so the two dials don't sweep in lockstep.
//   * Center info: ARGB8888 readout between the dials. Big speed
//     number derived from the speedometer's animation phase, "km/h"
//     label below.
//   * Warning indicators: ARGB8888 strip below the dials. Four icons
//     (left turn, check engine, high beam, right turn) each blinking
//     on independent periods so the demo shows them in different lit
//     states. zpos >= 4 so the allocator pins them on a hardware
//     plane even when the dials had to composite -- mirrors how a
//     real cluster never lets a warning indicator drop below 60 fps.
//
// The dials, center info, and warnings all demonstrate per-frame
// Blend2D paint into a dumb buffer the LayerScene scans out of
// directly. There's no double-buffering -- the buffer is mapped,
// painted, unmapped, and committed each frame; brief tearing on the
// needle is acceptable for an idle demo and would be addressed by
// alternating two DumbBufferSources per layer if a real cluster
// needed tear-free animation.
//
// Key bindings:
//   Esc / q / Ctrl-C — quit.
//   R                — toggle the rear-view camera layer (no-op when
//                      vicodec isn't loaded; cluster_sim emits a one-
//                      shot log line at startup explaining the skip).
//   Ctrl+Alt+F<n>    — VT switch (forwarded to libseat).
//
// The rear-view layer demonstrates V4l2DecoderSource end to end. At
// startup, cluster_sim probes /dev/video* for a vicodec encoder +
// stateful decoder pair; if both are present, it drives the encoder
// once to compress a short looping FWHT clip of an animated test
// pattern, holds the encoded bytes in memory, and feeds them through
// V4l2DecoderSource each time the rear-view is toggled on. This
// avoids bundling a binary FWHT asset in the repo while still
// exercising the full V4L2 stateful-decoder + KMS prime + addFB2
// path against a real /dev/video device.

#include "../../common/open_output.hpp"
#include "../../common/vt_switch.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/input/seat.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_handle.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/scene/v4l2_decoder_source.hpp>
#include <drm-cxx/session/seat.hpp>

#if CLUSTER_SIM_HAS_LIBYUV
// libyuv ships its umbrella header at <libyuv.h>; the convert_argb
// component is what we need for YUY2->ARGB8888 (== DRM_FORMAT_XRGB8888
// byte-for-byte; the alpha lane is ignored at scanout).
#include <libyuv/convert_argb.h>  // NOLINT(misc-include-cleaner)
#endif

// Blend2D ships its umbrella header at <blend2d/blend2d.h> on the
// upstream source install + Fedora; older Debian/Ubuntu packages put
// it at <blend2d.h>. Cover both via __has_include, mirroring the
// project's existing pattern in capture/snapshot.cpp.
#if __has_include(<blend2d/blend2d.h>)
#include <blend2d/blend2d.h>  // NOLINT(misc-include-cleaner)
#elif __has_include(<blend2d.h>)
#include <blend2d.h>  // NOLINT(misc-include-cleaner)
#else
#error "Blend2D header not found despite the build's blend2d gate being on"
#endif

#include <drm_fourcc.h>
#include <drm_mode.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <linux/input-event-codes.h>
#include <linux/videodev2.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {

// Cluster-aesthetic palette. Center is near-black with a subtle blue
// cast (matches the "deep instrument" look most automotive clusters
// settle on); edges fade darker so the dial layers have somewhere to
// read against.
constexpr std::uint32_t k_bg_center_argb = 0xFF101626U;
constexpr std::uint32_t k_bg_edge_argb = 0xFF02040AU;

// Dial palette. Face is darker than the bg center so the dial reads
// as separate from the backdrop; the rim is a neutral metallic gray.
constexpr std::uint32_t k_dial_rim_argb = 0xFF606870U;
constexpr std::uint32_t k_dial_face_argb = 0xFF080C18U;
constexpr std::uint32_t k_dial_tick_argb = 0xFFC0C8D0U;
constexpr std::uint32_t k_dial_hub_argb = 0xFF1A1F2CU;
constexpr std::uint32_t k_speedo_needle_argb = 0xFFFF3B30U;  // red
constexpr std::uint32_t k_tach_needle_argb = 0xFFFFB300U;    // amber

// Idle animation periods (seconds). Out of phase so the two dials
// don't sweep in lockstep -- a real cluster's dials are decorrelated
// (speed and RPM track different physical signals).
constexpr double k_speedo_period_s = 6.0;
constexpr double k_tach_period_s = 4.0;

// Conventional automotive cluster sweep range: 270° starting from
// the bottom-left and going clockwise to the bottom-right. In
// screen-with-Y-down coordinates (which match Blend2D), the bottom-
// left direction is 3π/4 and a clockwise sweep is positive.
constexpr double k_pi = 3.14159265358979323846;
constexpr double k_dial_start_angle = 3.0 * k_pi / 4.0;
constexpr double k_dial_sweep_angle = 3.0 * k_pi / 2.0;
constexpr int k_dial_major_ticks = 12;

// Center info layer dimensions (between the dials). Width covers
// "240 km/h" comfortably; height fits a big number plus the unit
// label below.
constexpr std::uint32_t k_info_w = 320U;
constexpr std::uint32_t k_info_h = 140U;
constexpr std::uint32_t k_info_speed_max_kmh = 240U;
constexpr std::uint32_t k_info_text_argb = 0xFFE6E8EEU;
constexpr std::uint32_t k_info_unit_argb = 0xFF7C8090U;
constexpr float k_info_speed_font_px = 96.0F;
constexpr float k_info_unit_font_px = 22.0F;

// Warning indicator strip. Four cells, each ~80 px wide; total width
// 4*100 = 400 with internal padding.
constexpr std::uint32_t k_warn_w = 400U;
constexpr std::uint32_t k_warn_h = 80U;
constexpr int k_warn_count = 4;
constexpr float k_warn_glyph_font_px = 36.0F;

// Each indicator has its own color for the lit state and its own
// blink period. Decorrelated periods so the strip looks organic
// rather than metronomic.
struct WarningSpec {
  std::uint32_t lit_argb;
  double period_s;
  std::string_view glyph;
};
constexpr std::array<WarningSpec, k_warn_count> k_warn_specs{{
    {0xFF34C759U, 0.55, "<"},  // left turn — green arrow stand-in
    {0xFFFFB300U, 0.83, "!"},  // check engine — amber bang
    {0xFF4DA8FFU, 1.20, "^"},  // high beam — blue chevron
    {0xFF34C759U, 0.62, ">"},  // right turn — green arrow stand-in
}};
constexpr std::uint32_t k_warn_dim_argb = 0xFF14181FU;  // unlit cell fill
constexpr std::uint32_t k_warn_glyph_dim_argb = 0xFF38404CU;
constexpr std::uint32_t k_warn_cell_radius = 12U;

// Rear-view layer dimensions. 320x240 keeps the layer reading as a
// thumbnail in the corner of the screen.
constexpr std::uint32_t k_rear_w = 320U;
constexpr std::uint32_t k_rear_h = 240U;

// Coded dimensions handed to vicodec (encoder + decoder). Decoupled
// from the on-screen rect because vicodec has a 640x360 minimum and
// snaps the requested dimensions up. NV12 capture buffers come back
// with bytesperline == width, and amdgpu DC requires LINEAR FB pitch
// to be 256-aligned -- 640 % 256 == 128, so 640-wide vicodec output
// fails drmModeAddFB2 on amdgpu. 768 is the smallest width vicodec
// honors that produces a 256-aligned NV12 pitch (768 % 256 == 0).
// The plane scales the 768x432 buffer down to k_rear_w x k_rear_h on
// screen via src_rect / dst_rect, so this only affects buffer size,
// not the visible thumbnail.
constexpr std::uint32_t k_rear_coded_w = 768U;
constexpr std::uint32_t k_rear_coded_h = 432U;

constexpr std::size_t k_rear_clip_frames = 8;
constexpr std::uint32_t k_rear_codec_fourcc = 0x54485746U;    // V4L2_PIX_FMT_FWHT
constexpr std::uint32_t k_rear_capture_fourcc = 0x3231564EU;  // V4L2_PIX_FMT_NV12
constexpr int k_rear_zpos = 5;                                // above warning strip
constexpr std::uint32_t k_rear_buffer_count = 4U;
constexpr std::uint32_t k_rear_output_buffer_size = 512U * 1024U;  // ample for FWHT @ 768x432

// UVC source pixel format. YUYV (V4L2_PIX_FMT_YUYV / 'YUYV') is the
// universal UVC fallback: every UVC class device must expose at least
// one YUYV resolution. libyuv::YUY2ToARGB then converts each captured
// frame into the rear-view DumbBufferSource (XRGB8888) on the CPU --
// sidesteps amdgpu DC's refusal to scan out PRIME-imports whose
// dma_buf->ops aren't amdgpu_dmabuf_ops (a provenance check that
// rejects ALL foreign V4L2 dmabufs regardless of backing storage,
// in place since kernel commit 3e339465a836 in 2017).
constexpr std::uint32_t k_uvc_capture_fourcc = 0x56595559U;  // V4L2_PIX_FMT_YUYV
constexpr std::uint32_t k_uvc_buffer_count = 4U;

// Synthetic rear-view tier. Used when neither a UVC camera nor a
// viable vicodec V4l2DecoderSource path is available -- specifically,
// the no-camera case on amdgpu where the kernel rejects foreign
// PRIME-imported dmabufs as FBs (see comment above k_uvc_capture_fourcc).
// We paint a moving Blend2D pattern into a DumbBufferSource (XRGB8888)
// each frame so R-toggle still demonstrates a layer-add path.
constexpr std::uint32_t k_rear_synth_bg_argb = 0xFF0A1018U;     // dark slate
constexpr std::uint32_t k_rear_synth_bar_argb = 0xFF1F4D80U;    // muted blue scan
constexpr std::uint32_t k_rear_synth_label_argb = 0xFFE6E8EEU;  // off-white
constexpr std::uint32_t k_rear_synth_subtle_argb = 0xFF38404CU;
constexpr float k_rear_synth_label_font_px = 22.0F;
constexpr float k_rear_synth_sublabel_font_px = 14.0F;
constexpr double k_rear_synth_period_s = 2.4;

// EINTR retry budget for ad-hoc V4L2 ioctls in the encoder driver
// below. Same idiom as V4l2DecoderSource's xioctl, just inline here
// so the cluster_sim example doesn't reach into library internals.
constexpr int k_max_ioctl_retries = 8;

// Convert an idle-animation phase in [0, 1] to a needle-position
// norm in [0, 1] via a (1 - cos)/2 sweep. Smooth at the endpoints,
// peaks in the middle, mirrors back -- the cosine sweep gives a
// slow-fast-slow visual that reads more "dial easing" than a
// triangle wave.
[[nodiscard]] double dial_norm_from_phase(double phase01) noexcept {
  return 0.5 * (1.0 - std::cos(2.0 * k_pi * phase01));
}

// Paint a radial gradient into an XRGB8888 dumb buffer. Blend2D
// initializes a BLImage view over the existing pixel buffer (no copy)
// and the BLContext flushes on `end()` before we return.
//
// NOLINTBEGIN(misc-include-cleaner) — Blend2D types reach this TU
// through the <blend2d/blend2d.h> umbrella; include-cleaner can't
// resolve the per-symbol header given the umbrella's macro guards.
// Same suppression as signage_player/overlay_renderer.cpp.
void paint_bg_gradient(drm::BufferMapping& mapping, std::uint32_t width,
                       std::uint32_t height) noexcept {
  if (width == 0U || height == 0U) {
    return;
  }
  drm::span<std::uint8_t> const pixels = mapping.pixels();
  std::uint32_t const stride = mapping.stride();
  if (pixels.size() < static_cast<std::size_t>(height) * stride) {
    return;
  }

  BLImage canvas;
  if (canvas.create_from_data(static_cast<int>(width), static_cast<int>(height), BL_FORMAT_XRGB32,
                              pixels.data(), static_cast<intptr_t>(stride), BL_DATA_ACCESS_RW,
                              nullptr, nullptr) != BL_SUCCESS) {
    return;
  }

  BLContext ctx(canvas);
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);

  double const cx = static_cast<double>(width) / 2.0;
  double const cy = static_cast<double>(height) / 2.0;
  double const radius = static_cast<double>(width > height ? width : height) * 0.6;
  BLGradient grad(BLRadialGradientValues(cx, cy, cx, cy, radius));
  grad.add_stop(0.0, BLRgba32(k_bg_center_argb));
  grad.add_stop(1.0, BLRgba32(k_bg_edge_argb));
  ctx.fill_all(grad);
  ctx.end();
}

// Paint a dial face + animated needle into an ARGB8888 dumb buffer.
// `norm` in [0, 1] maps onto the dial's 270° sweep range; `needle_argb`
// distinguishes speedo (red) from tach (amber). Painting clears the
// buffer to transparent first so the dial's circular face renders
// against the bg layer beneath.
void paint_dial(drm::BufferMapping& mapping, std::uint32_t size, double norm,
                std::uint32_t needle_argb) noexcept {
  if (size == 0U) {
    return;
  }
  drm::span<std::uint8_t> const pixels = mapping.pixels();
  std::uint32_t const stride = mapping.stride();
  if (pixels.size() < static_cast<std::size_t>(size) * stride) {
    return;
  }

  BLImage canvas;
  if (canvas.create_from_data(static_cast<int>(size), static_cast<int>(size), BL_FORMAT_PRGB32,
                              pixels.data(), static_cast<intptr_t>(stride), BL_DATA_ACCESS_RW,
                              nullptr, nullptr) != BL_SUCCESS) {
    return;
  }

  BLContext ctx(canvas);
  // Clear to transparent first so the dial reads as a circular cut-out
  // against the bg layer underneath. Subsequent paint ops use SRC_OVER
  // for proper alpha blending of the rim, face, ticks, and needle.
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.fill_all(BLRgba32(0x00000000U));
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);

  double const cx = static_cast<double>(size) / 2.0;
  double const cy = static_cast<double>(size) / 2.0;
  double const r_outer = static_cast<double>(size) * 0.48;
  double const r_inner = static_cast<double>(size) * 0.42;
  double const r_tick_outer = static_cast<double>(size) * 0.46;
  double const r_tick_inner = static_cast<double>(size) * 0.38;
  double const r_needle = static_cast<double>(size) * 0.40;
  double const r_hub = static_cast<double>(size) * 0.06;

  // Rim (filled circle in metallic gray) and face (slightly inset
  // dark fill) — fill_circle layers correctly because of SRC_OVER.
  ctx.fill_circle(BLCircle(cx, cy, r_outer), BLRgba32(k_dial_rim_argb));
  ctx.fill_circle(BLCircle(cx, cy, r_inner), BLRgba32(k_dial_face_argb));

  // Major ticks (12 + 1 to close the sweep range) — light gray lines
  // running radially outward across the rim. Using set_stroke_width
  // ahead of stroke_line because Blend2D's per-call stroke API doesn't
  // take a width inline.
  double const a_norm = std::clamp(norm, 0.0, 1.0);
  ctx.set_stroke_width(3.0);
  for (int i = 0; i <= k_dial_major_ticks; ++i) {
    double const t = static_cast<double>(i) / static_cast<double>(k_dial_major_ticks);
    double const a = k_dial_start_angle + (t * k_dial_sweep_angle);
    double const x1 = cx + (r_tick_inner * std::cos(a));
    double const y1 = cy + (r_tick_inner * std::sin(a));
    double const x2 = cx + (r_tick_outer * std::cos(a));
    double const y2 = cy + (r_tick_outer * std::sin(a));
    ctx.stroke_line(BLPoint(x1, y1), BLPoint(x2, y2), BLRgba32(k_dial_tick_argb));
  }

  // Needle: thick rounded line from center to the tip computed from
  // the normalized position. Stroke cap is round so the tip reads as
  // a smooth point rather than a chopped rectangle.
  double const needle_a = k_dial_start_angle + (a_norm * k_dial_sweep_angle);
  double const nx = cx + (r_needle * std::cos(needle_a));
  double const ny = cy + (r_needle * std::sin(needle_a));
  ctx.set_stroke_width(5.0);
  ctx.set_stroke_caps(BL_STROKE_CAP_ROUND);
  ctx.stroke_line(BLPoint(cx, cy), BLPoint(nx, ny), BLRgba32(needle_argb));

  // Hub cap covering the needle's pivot point.
  ctx.fill_circle(BLCircle(cx, cy, r_hub), BLRgba32(k_dial_hub_argb));

  ctx.end();
}

// Try a handful of well-known Linux font paths in priority order.
// fc-match would be the principled answer but pulls fontconfig as a
// hard dep for a scaffold-tier example; mirror signage_player's
// approach and probe the most common Fedora / Debian / Arch / Alpine
// layouts. Returning BL_ERROR_FONT_NOT_INITIALIZED is non-fatal
// upstream -- the center-info / warning paint paths fall back to
// shape-only output when the font face is empty.
[[nodiscard]] BLResult load_default_font_face(BLFontFace& face) noexcept {
  static constexpr std::array<const char*, 10> k_candidates = {
      "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Bold.ttf",
      "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
      "/usr/share/fonts/google-noto/NotoSans-Bold.ttf",
      "/usr/share/fonts/noto/NotoSans-Bold.ttf",
      "/usr/share/fonts/TTF/Vera.ttf",
  };
  for (const char* path : k_candidates) {
    if (face.create_from_file(path) == BL_SUCCESS) {
      return BL_SUCCESS;
    }
  }
  return BL_ERROR_FONT_NOT_INITIALIZED;
}

// Paint a centered UTF-8 string at the requested baseline-y. Helper
// shared by the center-info readout and the warning glyph cells.
// Caller has already prepared the BLContext + the font face; this
// just handles glyph shaping + centering math.
void paint_centered_text(BLContext& ctx, BLFont& font, std::string_view text, double cx,
                         double cy_baseline, std::uint32_t argb) noexcept {
  if (text.empty()) {
    return;
  }
  BLGlyphBuffer gb;
  gb.set_utf8_text(text.data(), text.size());
  if (font.shape(gb) != BL_SUCCESS) {
    return;
  }
  BLTextMetrics tm{};
  if (font.get_text_metrics(gb, tm) != BL_SUCCESS) {
    return;
  }
  double const text_w = tm.bounding_box.x1 - tm.bounding_box.x0;
  double const x = cx - (text_w * 0.5) - tm.bounding_box.x0;
  ctx.fill_utf8_text(BLPoint(x, cy_baseline), font, text.data(), text.size(), BLRgba32(argb));
}

// Paint the speed readout: one big numeric value (0-240) with a
// "km/h" label beneath. `font_face` may be empty when the host had no
// usable system font -- in that case the layer is left transparent
// rather than emitting a placeholder, since a wrong-looking text
// fallback reads worse than a missing one.
void paint_center_info(drm::BufferMapping& mapping, std::uint32_t speed_kmh,
                       const BLFontFace& font_face) noexcept {
  drm::span<std::uint8_t> const pixels = mapping.pixels();
  std::uint32_t const stride = mapping.stride();
  if (pixels.size() < static_cast<std::size_t>(k_info_h) * stride) {
    return;
  }
  BLImage canvas;
  if (canvas.create_from_data(static_cast<int>(k_info_w), static_cast<int>(k_info_h),
                              BL_FORMAT_PRGB32, pixels.data(), static_cast<intptr_t>(stride),
                              BL_DATA_ACCESS_RW, nullptr, nullptr) != BL_SUCCESS) {
    return;
  }
  BLContext ctx(canvas);
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.fill_all(BLRgba32(0x00000000U));
  if (!font_face.is_valid()) {
    ctx.end();
    return;
  }
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);

  // Speed digits centered horizontally at ~70% height; "km/h" label
  // sits directly below at ~95% height. Baseline-y values picked by
  // eye to match the typical instrument-cluster layout.
  std::string const speed_text = std::to_string(std::clamp<std::uint32_t>(speed_kmh, 0, 999));
  BLFont speed_font;
  if (speed_font.create_from_face(font_face, k_info_speed_font_px) == BL_SUCCESS) {
    paint_centered_text(ctx, speed_font, speed_text, static_cast<double>(k_info_w) * 0.5,
                        static_cast<double>(k_info_h) * 0.72, k_info_text_argb);
  }
  BLFont unit_font;
  if (unit_font.create_from_face(font_face, k_info_unit_font_px) == BL_SUCCESS) {
    paint_centered_text(ctx, unit_font, "km/h", static_cast<double>(k_info_w) * 0.5,
                        static_cast<double>(k_info_h) * 0.95, k_info_unit_argb);
  }
  ctx.end();
}

// Paint the four-cell warning strip. `phases[i]` in [0, 1) is the
// per-cell blink phase; lit when phase < 0.5, dim otherwise. Each
// cell is a rounded rectangle filled with the lit/dim color and a
// single glyph centered inside.
void paint_warning_indicators(drm::BufferMapping& mapping,
                              const std::array<double, k_warn_count>& phases,
                              const BLFontFace& font_face) noexcept {
  drm::span<std::uint8_t> const pixels = mapping.pixels();
  std::uint32_t const stride = mapping.stride();
  if (pixels.size() < static_cast<std::size_t>(k_warn_h) * stride) {
    return;
  }
  BLImage canvas;
  if (canvas.create_from_data(static_cast<int>(k_warn_w), static_cast<int>(k_warn_h),
                              BL_FORMAT_PRGB32, pixels.data(), static_cast<intptr_t>(stride),
                              BL_DATA_ACCESS_RW, nullptr, nullptr) != BL_SUCCESS) {
    return;
  }

  BLContext ctx(canvas);
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.fill_all(BLRgba32(0x00000000U));
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);

  BLFont glyph_font;
  bool const have_font = font_face.is_valid() &&
                         glyph_font.create_from_face(font_face, k_warn_glyph_font_px) == BL_SUCCESS;

  double const cell_w = static_cast<double>(k_warn_w) / static_cast<double>(k_warn_count);
  double const inner_pad = 6.0;
  for (int i = 0; i < k_warn_count; ++i) {
    bool const lit = phases.at(static_cast<std::size_t>(i)) < 0.5;
    auto const& spec = k_warn_specs.at(static_cast<std::size_t>(i));
    std::uint32_t const cell_argb = lit ? spec.lit_argb : k_warn_dim_argb;
    std::uint32_t const glyph_argb = lit ? 0xFF080C18U : k_warn_glyph_dim_argb;

    double const x0 = (static_cast<double>(i) * cell_w) + inner_pad;
    double const y0 = inner_pad;
    double const w = cell_w - (2.0 * inner_pad);
    double const h = static_cast<double>(k_warn_h) - (2.0 * inner_pad);
    ctx.fill_round_rect(BLRoundRect(x0, y0, w, h, k_warn_cell_radius), BLRgba32(cell_argb));

    if (have_font) {
      double const cx = x0 + (w * 0.5);
      double const cy_baseline = y0 + (h * 0.7);
      paint_centered_text(ctx, glyph_font, spec.glyph, cx, cy_baseline, glyph_argb);
    }
  }
  ctx.end();
}

// Synthetic rear-view paint used when neither UVC nor a viable
// vicodec V4l2DecoderSource path is available. Draws into a 320x240
// XRGB8888 dumb buffer: dark slate background, a horizontal scan bar
// sweeping vertically with a sine-decoupled phase, and a centered
// "REAR-VIEW (SIM)" label so the demo always has visible content on
// R press even when amdgpu's foreign-dmabuf rejection blocks both
// V4L2 paths.
void paint_rearview_synthetic(drm::BufferMapping& mapping, double elapsed_s,
                              const BLFontFace& font_face) noexcept {
  drm::span<std::uint8_t> const pixels = mapping.pixels();
  std::uint32_t const stride = mapping.stride();
  if (pixels.size() < static_cast<std::size_t>(k_rear_h) * stride) {
    return;
  }
  BLImage canvas;
  if (canvas.create_from_data(static_cast<int>(k_rear_w), static_cast<int>(k_rear_h),
                              BL_FORMAT_XRGB32, pixels.data(), static_cast<intptr_t>(stride),
                              BL_DATA_ACCESS_RW, nullptr, nullptr) != BL_SUCCESS) {
    return;
  }
  BLContext ctx(canvas);
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.fill_all(BLRgba32(k_rear_synth_bg_argb));
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);

  // Vertical scan bar -- a soft horizontal band whose y-position
  // sweeps via (1 - cos)/2 over k_rear_synth_period_s. The bar bleeds
  // beyond the canvas at the endpoints to avoid edge-case artefacting.
  double const phase = std::fmod(elapsed_s, k_rear_synth_period_s) / k_rear_synth_period_s;
  double const sweep = (1.0 - std::cos(phase * 2.0 * k_pi)) * 0.5;
  double const bar_h = static_cast<double>(k_rear_h) * 0.18;
  double const bar_cy = sweep * static_cast<double>(k_rear_h);
  ctx.fill_rect(BLRect(0.0, bar_cy - (bar_h * 0.5), static_cast<double>(k_rear_w), bar_h),
                BLRgba32(k_rear_synth_bar_argb));

  // Subtle horizontal tick marks at quarter heights so the layer reads
  // as "scanning" rather than just a moving block.
  for (int i = 1; i < 4; ++i) {
    double const y = (static_cast<double>(i) * static_cast<double>(k_rear_h)) / 4.0;
    ctx.fill_rect(BLRect(0.0, y - 0.5, static_cast<double>(k_rear_w), 1.0),
                  BLRgba32(k_rear_synth_subtle_argb));
  }

  // Centered "REAR-VIEW (SIM)" label so the user can tell at a glance
  // that this is the synthetic fallback, not a stalled real feed.
  if (font_face.is_valid()) {
    BLFont label_font;
    if (label_font.create_from_face(font_face, k_rear_synth_label_font_px) == BL_SUCCESS) {
      paint_centered_text(ctx, label_font, "REAR-VIEW", static_cast<double>(k_rear_w) * 0.5,
                          static_cast<double>(k_rear_h) * 0.45, k_rear_synth_label_argb);
    }
    BLFont sub_font;
    if (sub_font.create_from_face(font_face, k_rear_synth_sublabel_font_px) == BL_SUCCESS) {
      paint_centered_text(ctx, sub_font, "(SIM)", static_cast<double>(k_rear_w) * 0.5,
                          static_cast<double>(k_rear_h) * 0.6, k_rear_synth_subtle_argb);
    }
  }
  ctx.end();
}
// NOLINTEND(misc-include-cleaner)

// EINTR-retrying ioctl wrapper. Returns 0 on success, the errno on
// failure. Mirrors V4l2DecoderSource's internal helper; kept inline
// here so the example doesn't reach into library internals.
[[nodiscard]] int xioctl(int fd, unsigned long request, void* arg) noexcept {
  for (int attempt = 0; attempt < k_max_ioctl_retries; ++attempt) {
    int const r = ::ioctl(fd, request, arg);
    if (r >= 0) {
      return 0;
    }
    if (errno != EINTR) {
      return errno;
    }
  }
  return EINTR;
}

// Walk /dev/video* and find a vicodec endpoint that advertises FWHT on
// the requested side: OUTPUT for the stateful decoder (FWHT in,
// raw out), CAPTURE for the encoder (raw in, FWHT out). Same probe
// shape as tests/integration/test_v4l2_decoder_source_vicodec.cpp.
[[nodiscard]] std::optional<std::string> find_vicodec_endpoint(bool want_encoder) {
  std::error_code ec;
  for (auto const& entry : std::filesystem::directory_iterator("/dev", ec)) {
    auto const& p = entry.path();
    std::string const name = p.filename().string();
    if (name.rfind("video", 0) != 0) {
      continue;
    }
    int const fd = ::open(p.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
      continue;
    }
    v4l2_capability cap{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
      ::close(fd);
      continue;
    }
    std::string const card(reinterpret_cast<const char*>(cap.card));  // NOLINT
    if (card.find("vicodec") == std::string::npos) {
      ::close(fd);
      continue;
    }
    std::uint32_t const caps =
        ((cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U) ? cap.device_caps : cap.capabilities;
    bool const is_mplane = (caps & V4L2_CAP_VIDEO_M2M_MPLANE) != 0U;
    bool const is_single = (caps & V4L2_CAP_VIDEO_M2M) != 0U;
    if (!is_mplane && !is_single) {
      ::close(fd);
      continue;
    }
    std::uint32_t probe_type = 0;
    if (want_encoder) {
      probe_type = is_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    } else {
      probe_type = is_mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
    }
    bool advertises_fwht = false;
    for (std::uint32_t i = 0; i < 64; ++i) {
      v4l2_fmtdesc desc{};
      desc.index = i;
      desc.type = probe_type;
      if (xioctl(fd, VIDIOC_ENUM_FMT, &desc) != 0) {
        break;
      }
      if (desc.pixelformat == k_rear_codec_fourcc) {
        advertises_fwht = true;
        break;
      }
    }
    ::close(fd);
    if (advertises_fwht) {
      return p.string();
    }
  }
  return std::nullopt;
}

#if CLUSTER_SIM_HAS_LIBYUV
// UVC capture state. Lives for as long as the rear-view layer is
// engaged: open the /dev/video node, S_FMT YUYV at the negotiated
// dimensions, REQBUFS+QUERYBUF+MMAP a small ring of CPU-mappable
// buffers, then dequeue/libyuv-convert/queue per frame from the main
// loop. The buffers are V4L2_MEMORY_MMAP so the conversion source is
// the kernel's own page-aligned mapping; we never PRIME-import the
// dmabuf and so dodge amdgpu DC's foreign-vmalloc rejection entirely.
struct UvcCapture {
  int fd{-1};
  std::array<void*, k_uvc_buffer_count> mapped{};
  std::array<std::size_t, k_uvc_buffer_count> mapped_len{};
  std::uint32_t bytesperline{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
  bool streaming{false};
};

// Walk /dev/video* for a single-plane CAPTURE-only device that
// advertises YUYV. Skip M2M / OUTPUT-only devices (vicodec) and
// metadata / radio / SDR nodes. The first match wins; that's
// good enough for the cluster_sim demo where there's typically
// at most one camera attached.
[[nodiscard]] std::optional<std::string> find_uvc_endpoint() {
  std::error_code ec;
  for (auto const& entry : std::filesystem::directory_iterator("/dev", ec)) {
    auto const& p = entry.path();
    std::string const name = p.filename().string();
    if (name.rfind("video", 0) != 0) {
      continue;
    }
    int const fd = ::open(p.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
      continue;
    }
    v4l2_capability cap{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
      ::close(fd);
      continue;
    }
    std::uint32_t const caps =
        ((cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U) ? cap.device_caps : cap.capabilities;
    bool const is_capture = (caps & V4L2_CAP_VIDEO_CAPTURE) != 0U;
    bool const is_m2m = (caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)) != 0U;
    if (!is_capture || is_m2m || (caps & V4L2_CAP_STREAMING) == 0U) {
      ::close(fd);
      continue;
    }
    bool advertises_yuyv = false;
    for (std::uint32_t i = 0; i < 64; ++i) {
      v4l2_fmtdesc desc{};
      desc.index = i;
      desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      if (xioctl(fd, VIDIOC_ENUM_FMT, &desc) != 0) {
        break;
      }
      if (desc.pixelformat == k_uvc_capture_fourcc) {
        advertises_yuyv = true;
        break;
      }
    }
    ::close(fd);
    if (advertises_yuyv) {
      return p.string();
    }
  }
  return std::nullopt;
}

// REQBUFS=0 + munmap whatever's mapped + close the fd. Idempotent so
// the failure path in setup_uvc_capture and the toggle-off path can
// share it.
void teardown_uvc_capture(UvcCapture& uvc) noexcept {
  if (uvc.fd >= 0 && uvc.streaming) {
    int t = static_cast<int>(V4L2_BUF_TYPE_VIDEO_CAPTURE);
    (void)xioctl(uvc.fd, VIDIOC_STREAMOFF, &t);
    uvc.streaming = false;
  }
  for (std::size_t i = 0; i < k_uvc_buffer_count; ++i) {
    if (uvc.mapped.at(i) != nullptr) {
      ::munmap(uvc.mapped.at(i), uvc.mapped_len.at(i));
      uvc.mapped.at(i) = nullptr;
      uvc.mapped_len.at(i) = 0;
    }
  }
  if (uvc.fd >= 0) {
    v4l2_requestbuffers zero{};
    zero.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    zero.memory = V4L2_MEMORY_MMAP;
    (void)xioctl(uvc.fd, VIDIOC_REQBUFS, &zero);
    ::close(uvc.fd);
    uvc.fd = -1;
  }
  uvc.bytesperline = 0;
  uvc.width = 0;
  uvc.height = 0;
}

// Open the named UVC node, request YUYV at `want_w x want_h`, allocate
// + mmap the buffer ring, queue every buffer, and STREAMON. The kernel
// may snap requested dimensions; whatever it gives us is recorded in
// `uvc.width / uvc.height / uvc.bytesperline`. Returns true on
// success; on failure the partial state is torn down and `uvc.fd`
// is restored to -1.
[[nodiscard]] bool setup_uvc_capture(UvcCapture& uvc, const std::string& path, std::uint32_t want_w,
                                     std::uint32_t want_h) {
  uvc.fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if (uvc.fd < 0) {
    return false;
  }
  v4l2_format fmt{};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.pixelformat = k_uvc_capture_fourcc;
  fmt.fmt.pix.width = want_w;
  fmt.fmt.pix.height = want_h;
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  if (xioctl(uvc.fd, VIDIOC_S_FMT, &fmt) != 0) {
    teardown_uvc_capture(uvc);
    return false;
  }
  if (fmt.fmt.pix.pixelformat != k_uvc_capture_fourcc) {
    teardown_uvc_capture(uvc);
    return false;
  }
  uvc.width = fmt.fmt.pix.width;
  uvc.height = fmt.fmt.pix.height;
  uvc.bytesperline = fmt.fmt.pix.bytesperline;

  v4l2_requestbuffers req{};
  req.count = k_uvc_buffer_count;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(uvc.fd, VIDIOC_REQBUFS, &req) != 0 || req.count == 0) {
    teardown_uvc_capture(uvc);
    return false;
  }
  std::uint32_t const granted = std::min<std::uint32_t>(req.count, k_uvc_buffer_count);
  for (std::uint32_t i = 0; i < granted; ++i) {
    v4l2_buffer qb{};
    qb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    qb.memory = V4L2_MEMORY_MMAP;
    qb.index = i;
    if (xioctl(uvc.fd, VIDIOC_QUERYBUF, &qb) != 0) {
      teardown_uvc_capture(uvc);
      return false;
    }
    void* const m =
        ::mmap(nullptr, qb.length, PROT_READ | PROT_WRITE, MAP_SHARED, uvc.fd, qb.m.offset);
    if (m == MAP_FAILED) {
      teardown_uvc_capture(uvc);
      return false;
    }
    uvc.mapped.at(i) = m;
    uvc.mapped_len.at(i) = qb.length;
    if (xioctl(uvc.fd, VIDIOC_QBUF, &qb) != 0) {
      teardown_uvc_capture(uvc);
      return false;
    }
  }
  int t = static_cast<int>(V4L2_BUF_TYPE_VIDEO_CAPTURE);
  if (xioctl(uvc.fd, VIDIOC_STREAMON, &t) != 0) {
    teardown_uvc_capture(uvc);
    return false;
  }
  uvc.streaming = true;
  return true;
}
#endif  // CLUSTER_SIM_HAS_LIBYUV

// Paint a moving NV12 test pattern into a single-plane buffer. Used
// as the encoder's input each frame. The pattern is a simple gradient
// shifted by `frame_idx` columns so the encoded clip shows visible
// motion when looped.
void paint_test_pattern_nv12(std::uint8_t* nv12, std::uint32_t w, std::uint32_t h,
                             std::size_t frame_idx) noexcept {
  // Y plane: vertical gradient horizontally shifted by frame.
  auto const shift = static_cast<std::uint32_t>(frame_idx) * 16U;
  auto const w_sz = static_cast<std::size_t>(w);
  auto const h_sz = static_cast<std::size_t>(h);
  for (std::uint32_t y = 0; y < h; ++y) {
    auto* row = nv12 + (static_cast<std::size_t>(y) * w_sz);
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint32_t const v = (x + shift) ^ y;
      row[x] = static_cast<std::uint8_t>(0x40U + (v & 0x7FU));
    }
  }
  // UV plane (interleaved Cb,Cr): mid-gray (128, 128) gives a near-
  // monochrome image -- chroma motion isn't necessary for the demo.
  std::uint8_t* uv = nv12 + (w_sz * h_sz);
  std::memset(uv, 0x80, w_sz * (h_sz / 2U));
}

// Drive vicodec's encoder once at startup to produce
// `k_rear_clip_frames` frames of FWHT-encoded bitstream. Returns the
// per-frame encoded byte vectors; an empty result signals "encoder
// path didn't yield a usable clip" and the caller should disable the
// rear-view feature.
//
// The flow mirrors the V4L2 stateful M2M state machine but inverted:
// raw NV12 frames go in on OUTPUT, FWHT bytes come out on CAPTURE.
// All ioctls are ad-hoc here because cluster_sim only encodes once
// at startup -- no need to factor the encoder side into a library
// module.
[[nodiscard]] std::vector<std::vector<std::uint8_t>> encode_fwht_clip(
    const std::string& encoder_path) {
  std::vector<std::vector<std::uint8_t>> result;

  int const fd = ::open(encoder_path.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    return result;
  }

  v4l2_capability cap{};
  if (xioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
    ::close(fd);
    return result;
  }
  std::uint32_t const dev_caps =
      ((cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U) ? cap.device_caps : cap.capabilities;
  bool const is_mplane = (dev_caps & V4L2_CAP_VIDEO_M2M_MPLANE) != 0U;
  std::uint32_t const out_type =
      is_mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
  std::uint32_t const cap_type =
      is_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;

  // Source format on OUTPUT (raw NV12 input to the encoder).
  v4l2_format src_fmt{};
  src_fmt.type = out_type;
  if (is_mplane) {
    src_fmt.fmt.pix_mp.pixelformat = k_rear_capture_fourcc;
    src_fmt.fmt.pix_mp.width = k_rear_coded_w;
    src_fmt.fmt.pix_mp.height = k_rear_coded_h;
    src_fmt.fmt.pix_mp.num_planes = 1;
  } else {
    src_fmt.fmt.pix.pixelformat = k_rear_capture_fourcc;
    src_fmt.fmt.pix.width = k_rear_coded_w;
    src_fmt.fmt.pix.height = k_rear_coded_h;
  }
  if (xioctl(fd, VIDIOC_S_FMT, &src_fmt) != 0) {
    ::close(fd);
    return result;
  }

  // Coded format on CAPTURE (FWHT output).
  v4l2_format coded_fmt{};
  coded_fmt.type = cap_type;
  if (is_mplane) {
    coded_fmt.fmt.pix_mp.pixelformat = k_rear_codec_fourcc;
    coded_fmt.fmt.pix_mp.width = k_rear_coded_w;
    coded_fmt.fmt.pix_mp.height = k_rear_coded_h;
  } else {
    coded_fmt.fmt.pix.pixelformat = k_rear_codec_fourcc;
    coded_fmt.fmt.pix.width = k_rear_coded_w;
    coded_fmt.fmt.pix.height = k_rear_coded_h;
  }
  if (xioctl(fd, VIDIOC_S_FMT, &coded_fmt) != 0) {
    ::close(fd);
    return result;
  }

  auto cleanup_queues = [&]() {
    int t = static_cast<int>(out_type);
    (void)xioctl(fd, VIDIOC_STREAMOFF, &t);
    t = static_cast<int>(cap_type);
    (void)xioctl(fd, VIDIOC_STREAMOFF, &t);
    v4l2_requestbuffers zero{};
    zero.type = out_type;
    zero.memory = V4L2_MEMORY_MMAP;
    (void)xioctl(fd, VIDIOC_REQBUFS, &zero);
    zero.type = cap_type;
    (void)xioctl(fd, VIDIOC_REQBUFS, &zero);
  };

  // REQBUFS + MMAP both queues. Single buffer per side is enough --
  // we synchronously feed one frame at a time.
  v4l2_requestbuffers req{};
  req.count = 1;
  req.type = out_type;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd, VIDIOC_REQBUFS, &req) != 0 || req.count == 0) {
    cleanup_queues();
    ::close(fd);
    return result;
  }
  req = {};
  req.count = 1;
  req.type = cap_type;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd, VIDIOC_REQBUFS, &req) != 0 || req.count == 0) {
    cleanup_queues();
    ::close(fd);
    return result;
  }

  auto query_and_map = [&](std::uint32_t type, std::size_t& out_len, void*& out_ptr) -> bool {
    v4l2_buffer qb{};
    std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
    qb.type = type;
    qb.memory = V4L2_MEMORY_MMAP;
    qb.index = 0;
    if (is_mplane) {
      qb.length = VIDEO_MAX_PLANES;
      qb.m.planes = planes.data();
    }
    if (xioctl(fd, VIDIOC_QUERYBUF, &qb) != 0) {
      return false;
    }
    std::size_t const length = is_mplane ? planes.at(0).length : qb.length;
    std::uint32_t const offset = is_mplane ? planes.at(0).m.mem_offset : qb.m.offset;
    void* const mapped = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
    if (mapped == MAP_FAILED) {
      return false;
    }
    out_len = length;
    out_ptr = mapped;
    return true;
  };

  std::size_t out_len = 0;
  void* out_ptr = nullptr;
  std::size_t cap_len = 0;
  void* cap_ptr = nullptr;
  if (!query_and_map(out_type, out_len, out_ptr) || !query_and_map(cap_type, cap_len, cap_ptr)) {
    if (out_ptr != nullptr) {
      ::munmap(out_ptr, out_len);
    }
    cleanup_queues();
    ::close(fd);
    return result;
  }

  auto release = [&]() {
    if (out_ptr != nullptr) {
      ::munmap(out_ptr, out_len);
    }
    if (cap_ptr != nullptr) {
      ::munmap(cap_ptr, cap_len);
    }
    cleanup_queues();
    ::close(fd);
  };

  // STREAMON both queues so the encoder is ready to consume the
  // first OUTPUT QBUF as soon as it lands.
  int t = static_cast<int>(out_type);
  if (xioctl(fd, VIDIOC_STREAMON, &t) != 0) {
    release();
    return result;
  }
  t = static_cast<int>(cap_type);
  if (xioctl(fd, VIDIOC_STREAMON, &t) != 0) {
    release();
    return result;
  }

  std::size_t const nv12_size = static_cast<std::size_t>(k_rear_coded_w) * k_rear_coded_h * 3U / 2U;
  result.reserve(k_rear_clip_frames);
  for (std::size_t i = 0; i < k_rear_clip_frames; ++i) {
    if (out_len < nv12_size) {
      break;
    }
    paint_test_pattern_nv12(static_cast<std::uint8_t*>(out_ptr), k_rear_coded_w, k_rear_coded_h, i);

    v4l2_buffer qb{};
    std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
    qb.type = out_type;
    qb.memory = V4L2_MEMORY_MMAP;
    qb.index = 0;
    if (is_mplane) {
      qb.length = 1;
      qb.m.planes = planes.data();
      planes.at(0).bytesused = static_cast<std::uint32_t>(nv12_size);
    } else {
      qb.bytesused = static_cast<std::uint32_t>(nv12_size);
    }
    if (xioctl(fd, VIDIOC_QBUF, &qb) != 0) {
      break;
    }

    // QBUF a CAPTURE buffer so the encoder has somewhere to write.
    v4l2_buffer qbc{};
    std::array<v4l2_plane, VIDEO_MAX_PLANES> planes_c{};
    qbc.type = cap_type;
    qbc.memory = V4L2_MEMORY_MMAP;
    qbc.index = 0;
    if (is_mplane) {
      qbc.length = 1;
      qbc.m.planes = planes_c.data();
    }
    if (xioctl(fd, VIDIOC_QBUF, &qbc) != 0) {
      break;
    }

    // Poll until both queues complete. vicodec is fast so a short
    // bounded poll is sufficient.
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN | POLLOUT;
    if (::poll(&pfd, 1, 1000) <= 0) {
      break;
    }

    // DQBUF OUTPUT (encoder consumed our input).
    v4l2_buffer dqo{};
    std::array<v4l2_plane, VIDEO_MAX_PLANES> dqo_planes{};
    dqo.type = out_type;
    dqo.memory = V4L2_MEMORY_MMAP;
    if (is_mplane) {
      dqo.length = VIDEO_MAX_PLANES;
      dqo.m.planes = dqo_planes.data();
    }
    if (xioctl(fd, VIDIOC_DQBUF, &dqo) != 0) {
      break;
    }

    // DQBUF CAPTURE (encoder produced FWHT bytes).
    v4l2_buffer dqc{};
    std::array<v4l2_plane, VIDEO_MAX_PLANES> dqc_planes{};
    dqc.type = cap_type;
    dqc.memory = V4L2_MEMORY_MMAP;
    if (is_mplane) {
      dqc.length = VIDEO_MAX_PLANES;
      dqc.m.planes = dqc_planes.data();
    }
    if (xioctl(fd, VIDIOC_DQBUF, &dqc) != 0) {
      break;
    }
    std::uint32_t const bytesused = is_mplane ? dqc_planes.at(0).bytesused : dqc.bytesused;
    if (bytesused == 0 || bytesused > cap_len) {
      break;
    }
    std::vector<std::uint8_t> frame(bytesused);
    std::memcpy(frame.data(), cap_ptr, bytesused);
    result.push_back(std::move(frame));
  }

  release();
  return result;
}

// State for the optional rear-view layer. Three source tiers are
// supported and the startup probe picks the highest viable one:
//
//   1. UVC (preferred when libyuv is built in and a UVC camera is
//      attached): YUYV capture from /dev/video* is libyuv-converted
//      to XRGB8888 into a CPU-allocated DumbBufferSource each frame.
//      Works on amdgpu DC (no foreign-dmabuf import involved).
//   2. vicodec (when no UVC but vicodec works on this driver): an
//      FWHT clip encoded once at startup is replayed through
//      V4l2DecoderSource as zero-copy NV12. Works on drivers that
//      accept foreign PRIME imports (vkms / most embedded SoCs);
//      rejected by amdgpu DC, in which case the startup probe
//      tears it down and we fall through to tier 3.
//   3. Synthetic Blend2D (last resort): a moving scan-bar pattern
//      painted into a DumbBufferSource each frame. Always works,
//      ensures the R toggle has visible content even on amdgpu
//      with no camera.
//
// `layer` is engaged whichever tier is active; `uvc.fd >= 0`
// discriminates UVC; `synthetic_armed` distinguishes synthetic from
// vicodec in the toggle dispatcher (vicodec uses `clip`).
struct RearViewState {
  // vicodec tier state
  std::vector<std::vector<std::uint8_t>> clip;
  std::string decoder_path;
  drm::scene::V4l2DecoderSource* source{nullptr};
  std::size_t next_frame{0};

#if CLUSTER_SIM_HAS_LIBYUV
  // UVC tier state
  std::string uvc_path;
  UvcCapture uvc;
  drm::scene::DumbBufferSource* dumb_source{nullptr};
#endif

  // Synthetic-pattern tier state. Populated when neither UVC nor
  // vicodec are viable. `synthetic_source` is non-null while the
  // synthetic layer is engaged so the per-frame paint cycle knows
  // to repaint into it.
  bool synthetic_armed{false};
  drm::scene::DumbBufferSource* synthetic_source{nullptr};

  std::optional<drm::scene::LayerHandle> layer;
};

#if CLUSTER_SIM_HAS_LIBYUV
// Toggle-on path for the UVC source. UVC streaming was already armed
// at startup (STREAMON over USB negotiates ~100-300 ms of bandwidth
// alloc; doing it here would visibly stall the dial-paint loop), so
// this just allocates the destination dumb buffer and adds the layer.
[[nodiscard]] bool toggle_rearview_on_uvc(RearViewState& rv, drm::Device& dev,
                                          drm::scene::LayerScene& scene, std::int32_t x,
                                          std::int32_t y) {
  if (rv.uvc.fd < 0) {
    return false;
  }
  // Match the dumb buffer to the camera's negotiated dimensions so
  // libyuv writes one row per camera row -- avoids a separate scale
  // step. The plane scales the dumb buffer down to k_rear_w x k_rear_h
  // on screen via src_rect / dst_rect just like the vicodec path.
  auto dumb_r =
      drm::scene::DumbBufferSource::create(dev, rv.uvc.width, rv.uvc.height, DRM_FORMAT_XRGB8888);
  if (!dumb_r) {
    drm::println(stderr, "rear-view: DumbBufferSource::create failed: {}",
                 dumb_r.error().message());
    return false;
  }
  auto dumb_holder = std::move(*dumb_r);
  auto* dumb_raw = dumb_holder.get();

  drm::scene::LayerDesc desc;
  desc.source = std::move(dumb_holder);
  desc.display.src_rect = drm::scene::Rect{0, 0, rv.uvc.width, rv.uvc.height};
  desc.display.dst_rect = drm::scene::Rect{x, y, k_rear_w, k_rear_h};
  desc.display.zpos = k_rear_zpos;
  desc.content_type = drm::planes::ContentType::Video;
  auto layer_r = scene.add_layer(std::move(desc));
  if (!layer_r) {
    drm::println(stderr, "rear-view: add_layer failed: {}", layer_r.error().message());
    return false;
  }
  rv.layer = *layer_r;
  rv.dumb_source = dumb_raw;
  return true;
}

// Per-frame pump for the UVC path: dequeue one YUYV frame, libyuv-
// convert it to XRGB8888 inside the dumb buffer's mapping, queue the
// V4L2 buffer back. Caller invokes from the main loop only when the
// UVC fd has POLLIN; EAGAIN here would simply mean the kernel beat
// our poll() to the punch. Any failure (bad ioctl, libyuv error) is
// silently dropped -- the previous frame stays scanned out.
void drive_rearview_uvc(RearViewState& rv) noexcept {
  if (rv.dumb_source == nullptr || rv.uvc.fd < 0) {
    return;
  }
  v4l2_buffer dq{};
  dq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  dq.memory = V4L2_MEMORY_MMAP;
  if (xioctl(rv.uvc.fd, VIDIOC_DQBUF, &dq) != 0) {
    return;
  }
  if (dq.index >= k_uvc_buffer_count || rv.uvc.mapped.at(dq.index) == nullptr) {
    return;
  }
  auto const* src = static_cast<const std::uint8_t*>(rv.uvc.mapped.at(dq.index));
  auto map_r = rv.dumb_source->map(drm::MapAccess::Write);
  if (map_r) {
    auto& mapping = *map_r;
    auto pixels = mapping.pixels();
    if (pixels.data() != nullptr && mapping.stride() != 0U) {
      (void)libyuv::YUY2ToARGB(src, static_cast<int>(rv.uvc.bytesperline), pixels.data(),
                               static_cast<int>(mapping.stride()), static_cast<int>(rv.uvc.width),
                               static_cast<int>(rv.uvc.height));
    }
  }
  v4l2_buffer qb{};
  qb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  qb.memory = V4L2_MEMORY_MMAP;
  qb.index = dq.index;
  (void)xioctl(rv.uvc.fd, VIDIOC_QBUF, &qb);
}
#endif  // CLUSTER_SIM_HAS_LIBYUV

// Toggle-on path for the synthetic-pattern tier. Allocates a single
// XRGB8888 dumb buffer at the on-screen rear-view dimensions and adds
// the layer; the per-frame Blend2D paint runs from the main loop.
[[nodiscard]] bool toggle_rearview_on_synthetic(RearViewState& rv, drm::Device& dev,
                                                drm::scene::LayerScene& scene, std::int32_t x,
                                                std::int32_t y) {
  auto dumb_r = drm::scene::DumbBufferSource::create(dev, k_rear_w, k_rear_h, DRM_FORMAT_XRGB8888);
  if (!dumb_r) {
    drm::println(stderr, "rear-view: synthetic DumbBufferSource::create failed: {}",
                 dumb_r.error().message());
    return false;
  }
  auto dumb_holder = std::move(*dumb_r);
  auto* dumb_raw = dumb_holder.get();

  drm::scene::LayerDesc desc;
  desc.source = std::move(dumb_holder);
  desc.display.src_rect = drm::scene::Rect{0, 0, k_rear_w, k_rear_h};
  desc.display.dst_rect = drm::scene::Rect{x, y, k_rear_w, k_rear_h};
  desc.display.zpos = k_rear_zpos;
  // Synthetic isn't a video stream; default content type is fine and
  // doesn't bias the allocator toward YUV-capable planes.
  auto layer_r = scene.add_layer(std::move(desc));
  if (!layer_r) {
    drm::println(stderr, "rear-view: add_layer failed: {}", layer_r.error().message());
    return false;
  }
  rv.layer = *layer_r;
  rv.synthetic_source = dumb_raw;
  return true;
}

// Toggle-on path for the vicodec tier. Builds a V4l2DecoderSource
// against the cached decoder path, hands it to the scene, then submits
// the first clip frame so the decoder has input to chew on. The
// startup probe already verified V4l2DecoderSource::create succeeds on
// this driver, so failure here is unexpected (still handled, but
// treated as a one-shot not a tier-disabling event since the probe
// proved the tier viable).
[[nodiscard]] bool toggle_rearview_on_vicodec(RearViewState& rv, drm::Device& dev,
                                              drm::scene::LayerScene& scene, std::int32_t x,
                                              std::int32_t y) {
  if (rv.clip.empty()) {
    return false;
  }
  drm::scene::V4l2DecoderConfig cfg;
  cfg.codec_fourcc = k_rear_codec_fourcc;
  cfg.capture_fourcc = k_rear_capture_fourcc;
  cfg.coded_width = k_rear_coded_w;
  cfg.coded_height = k_rear_coded_h;
  cfg.output_buffer_count = k_rear_buffer_count;
  cfg.capture_buffer_count = k_rear_buffer_count;
  cfg.output_buffer_size = k_rear_output_buffer_size;
  auto src_r = drm::scene::V4l2DecoderSource::create(dev, rv.decoder_path.c_str(), cfg);
  if (!src_r) {
    drm::println(stderr, "rear-view: V4l2DecoderSource::create failed: {}",
                 src_r.error().message());
    return false;
  }
  auto src_holder = std::move(*src_r);
  auto* src_raw = src_holder.get();

  drm::scene::LayerDesc desc;
  desc.source = std::move(src_holder);
  auto const fmt = src_raw->format();
  desc.display.src_rect = drm::scene::Rect{0, 0, fmt.width, fmt.height};
  desc.display.dst_rect = drm::scene::Rect{x, y, k_rear_w, k_rear_h};
  desc.display.zpos = k_rear_zpos;
  desc.content_type = drm::planes::ContentType::Video;
  auto layer_r = scene.add_layer(std::move(desc));
  if (!layer_r) {
    drm::println(stderr, "rear-view: add_layer failed: {}", layer_r.error().message());
    return false;
  }
  rv.layer = *layer_r;
  rv.source = src_raw;
  rv.next_frame = 0;

  // Feed the first frame so the decoder has something queued before
  // the scene's next commit hits acquire(). Subsequent frames flow
  // through drive_rearview() each main-loop tick.
  auto const& bytes = rv.clip.at(rv.next_frame);
  if (auto r =
          rv.source->submit_bitstream(drm::span<const std::uint8_t>(bytes.data(), bytes.size()), 0);
      r) {
    rv.next_frame = (rv.next_frame + 1) % rv.clip.size();
  }
  return true;
}

// Public toggle-on dispatcher: pick the highest-priority tier whose
// state was armed by the startup probe. UVC > vicodec > synthetic.
[[nodiscard]] bool toggle_rearview_on(RearViewState& rv, drm::Device& dev,
                                      drm::scene::LayerScene& scene, std::int32_t x,
                                      std::int32_t y) {
  if (rv.layer.has_value()) {
    return false;
  }
#if CLUSTER_SIM_HAS_LIBYUV
  if (!rv.uvc_path.empty()) {
    return toggle_rearview_on_uvc(rv, dev, scene, x, y);
  }
#endif
  if (!rv.clip.empty()) {
    return toggle_rearview_on_vicodec(rv, dev, scene, x, y);
  }
  if (rv.synthetic_armed) {
    return toggle_rearview_on_synthetic(rv, dev, scene, x, y);
  }
  return false;
}

void toggle_rearview_off(RearViewState& rv, drm::scene::LayerScene& scene) noexcept {
  if (!rv.layer.has_value()) {
    return;
  }
  scene.remove_layer(*rv.layer);
  rv.layer.reset();
  rv.source = nullptr;
  rv.next_frame = 0;
  // The synthetic and UVC dumb-source pointers were owned by the
  // layer whose remove_layer just dropped them; clear our raw
  // copies so per-frame paint / drain code stops touching freed
  // memory.
  rv.synthetic_source = nullptr;
#if CLUSTER_SIM_HAS_LIBYUV
  // UVC stays streaming across toggles so the next R press is cheap
  // (no STREAMON / USB renegotiation). Frames pile up in the V4L2
  // ring while the layer is detached; the kernel back-pressures the
  // camera once the ring is full, no leak.
  rv.dumb_source = nullptr;
#endif
}

// Called from the main loop each iteration when the rear-view is
// active: drain the decoder's events + completed buffers, and submit
// the next clip frame when an OUTPUT slot is free. EAGAIN from
// submit_bitstream just means "no free OUTPUT buffer yet" -- we'll
// retry on the next tick. UVC source has its own per-poll path;
// drive_rearview() is a no-op there.
void drive_rearview(RearViewState& rv) noexcept {
  if (rv.source == nullptr || rv.clip.empty()) {
    return;
  }
  if (auto r = rv.source->drive(); !r) {
    // SOURCE_CHANGE / EOS / fatal -- detach the source so the next
    // toggle on rebuilds it from scratch. The layer stays in the
    // scene; it'll just stop advancing until toggled off+on.
    return;
  }
  auto const& bytes = rv.clip.at(rv.next_frame);
  if (auto r =
          rv.source->submit_bitstream(drm::span<const std::uint8_t>(bytes.data(), bytes.size()), 0);
      r) {
    rv.next_frame = (rv.next_frame + 1) % rv.clip.size();
  }
}

}  // namespace

int main(int argc, char** argv) {
  auto ctx_opt = drm::examples::open_and_pick_output(argc, argv);
  if (!ctx_opt.has_value()) {
    return EXIT_FAILURE;
  }
  auto& ctx = *ctx_opt;
  auto& dev = ctx.device;
  auto& seat = ctx.seat;
  std::uint32_t const fb_w = ctx.mode.hdisplay;
  std::uint32_t const fb_h = ctx.mode.vdisplay;

  // Bg layer — painted once, scanned out forever (until step 2 starts
  // mutating per-frame state on the dial layers above it).
  auto bg_src_r = drm::scene::DumbBufferSource::create(dev, fb_w, fb_h, DRM_FORMAT_XRGB8888);
  if (!bg_src_r) {
    drm::println(stderr, "DumbBufferSource::create (bg): {}", bg_src_r.error().message());
    return EXIT_FAILURE;
  }
  auto bg_src = std::move(*bg_src_r);
  auto* bg_src_raw = bg_src.get();
  if (auto m = bg_src->map(drm::MapAccess::Write); m) {
    paint_bg_gradient(*m, fb_w, fb_h);
  } else {
    drm::println(stderr, "bg paint: map failed: {}", m.error().message());
    return EXIT_FAILURE;
  }

  drm::scene::LayerScene::Config scene_cfg;
  scene_cfg.crtc_id = ctx.crtc_id;
  scene_cfg.connector_id = ctx.connector_id;
  scene_cfg.mode = ctx.mode;
  auto scene_r = drm::scene::LayerScene::create(dev, scene_cfg);
  if (!scene_r) {
    drm::println(stderr, "LayerScene::create: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_r);

  drm::scene::LayerDesc bg_desc;
  bg_desc.source = std::move(bg_src);
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  if (auto h = scene->add_layer(std::move(bg_desc)); !h) {
    drm::println(stderr, "add_layer (bg): {}", h.error().message());
    return EXIT_FAILURE;
  }

  // Dial sizing: 4/9 of screen height capped at 400 px gives ~400 on
  // 1080p, 320 on 720p, scales down sensibly for smaller outputs.
  // Centered vertically; speedo at 1/4-screen, tach at 3/4-screen.
  std::uint32_t const dial_size = std::min<std::uint32_t>(fb_h * 4U / 9U, 400U);
  auto const dial_y = static_cast<std::int32_t>((fb_h - dial_size) / 2U);
  auto const speedo_x =
      static_cast<std::int32_t>(fb_w / 4U) - static_cast<std::int32_t>(dial_size / 2U);
  auto const tach_x =
      static_cast<std::int32_t>((fb_w * 3U) / 4U) - static_cast<std::int32_t>(dial_size / 2U);

  auto make_dial_layer =
      [&](std::int32_t x,
          std::int32_t y) -> drm::expected<drm::scene::DumbBufferSource*, std::error_code> {
    auto src_r =
        drm::scene::DumbBufferSource::create(dev, dial_size, dial_size, DRM_FORMAT_ARGB8888);
    if (!src_r) {
      return drm::unexpected<std::error_code>(src_r.error());
    }
    auto src = std::move(*src_r);
    auto* raw = src.get();
    drm::scene::LayerDesc desc;
    desc.source = std::move(src);
    desc.display.src_rect = drm::scene::Rect{0, 0, dial_size, dial_size};
    desc.display.dst_rect = drm::scene::Rect{x, y, dial_size, dial_size};
    // amdgpu pins PRIMARY at zpos=2; the dials need to sit above the
    // bg, so anchor them at zpos>=3 explicitly to avoid silent
    // collision with the primary plane.
    desc.display.zpos = 3;
    if (auto h = scene->add_layer(std::move(desc)); !h) {
      return drm::unexpected<std::error_code>(h.error());
    }
    return raw;
  };

  auto speedo_r = make_dial_layer(speedo_x, dial_y);
  if (!speedo_r) {
    drm::println(stderr, "add_layer (speedo): {}", speedo_r.error().message());
    return EXIT_FAILURE;
  }
  auto tach_r = make_dial_layer(tach_x, dial_y);
  if (!tach_r) {
    drm::println(stderr, "add_layer (tach): {}", tach_r.error().message());
    return EXIT_FAILURE;
  }
  auto* speedo_src_raw = *speedo_r;
  auto* tach_src_raw = *tach_r;

  // Center info layer: ARGB8888 between the dials, vertically
  // centered against the dial midline. Uses the same zpos as the
  // dials -- it's a sibling UI layer, not a priority overlay.
  auto info_src_r =
      drm::scene::DumbBufferSource::create(dev, k_info_w, k_info_h, DRM_FORMAT_ARGB8888);
  if (!info_src_r) {
    drm::println(stderr, "DumbBufferSource::create (info): {}", info_src_r.error().message());
    return EXIT_FAILURE;
  }
  auto* info_src_raw = info_src_r->get();
  drm::scene::LayerDesc info_desc;
  info_desc.source = std::move(*info_src_r);
  info_desc.display.src_rect = drm::scene::Rect{0, 0, k_info_w, k_info_h};
  auto const info_x =
      static_cast<std::int32_t>(fb_w / 2U) - static_cast<std::int32_t>(k_info_w / 2U);
  auto const info_y =
      dial_y + static_cast<std::int32_t>(dial_size / 2U) - static_cast<std::int32_t>(k_info_h / 2U);
  info_desc.display.dst_rect = drm::scene::Rect{info_x, info_y, k_info_w, k_info_h};
  info_desc.display.zpos = 3;
  if (auto h = scene->add_layer(std::move(info_desc)); !h) {
    drm::println(stderr, "add_layer (info): {}", h.error().message());
    return EXIT_FAILURE;
  }

  // Warning indicators: 4-cell ARGB strip below the dials. zpos=4
  // (above the dials) so the allocator pins it on a hardware plane
  // even when the dials had to composite -- a real cluster never
  // lets a warning indicator drop frames.
  auto warn_src_r =
      drm::scene::DumbBufferSource::create(dev, k_warn_w, k_warn_h, DRM_FORMAT_ARGB8888);
  if (!warn_src_r) {
    drm::println(stderr, "DumbBufferSource::create (warn): {}", warn_src_r.error().message());
    return EXIT_FAILURE;
  }
  auto* warn_src_raw = warn_src_r->get();
  drm::scene::LayerDesc warn_desc;
  warn_desc.source = std::move(*warn_src_r);
  warn_desc.display.src_rect = drm::scene::Rect{0, 0, k_warn_w, k_warn_h};
  auto const warn_x =
      static_cast<std::int32_t>(fb_w / 2U) - static_cast<std::int32_t>(k_warn_w / 2U);
  auto const warn_y = dial_y + static_cast<std::int32_t>(dial_size) + 40;
  warn_desc.display.dst_rect = drm::scene::Rect{warn_x, warn_y, k_warn_w, k_warn_h};
  warn_desc.display.zpos = 4;
  if (auto h = scene->add_layer(std::move(warn_desc)); !h) {
    drm::println(stderr, "add_layer (warn): {}", h.error().message());
    return EXIT_FAILURE;
  }

  // Best-effort font load. If no usable font is installed, the info
  // + warning paints fall back to shape-only output (transparent
  // info, glyph-less warning cells). The dials don't need a font.
  BLFontFace font_face;
  if (load_default_font_face(font_face) != BL_SUCCESS) {
    drm::println(stderr,
                 "cluster_sim: no usable system font found -- center info + warning glyphs "
                 "will render shape-only");
  }

  // Paint each animated layer at zero/initial state so the first
  // commit has valid pixel content everywhere. repaint_layers is the
  // shared per-frame paint path called from the main loop and from
  // the session-resume cleanup.
  auto repaint_layers = [&](double speedo_norm, double tach_norm, std::uint32_t speed_kmh,
                            const std::array<double, k_warn_count>& warn_phases) {
    if (auto m = speedo_src_raw->map(drm::MapAccess::Write); m) {
      paint_dial(*m, dial_size, speedo_norm, k_speedo_needle_argb);
    }
    if (auto m = tach_src_raw->map(drm::MapAccess::Write); m) {
      paint_dial(*m, dial_size, tach_norm, k_tach_needle_argb);
    }
    if (auto m = info_src_raw->map(drm::MapAccess::Write); m) {
      paint_center_info(*m, speed_kmh, font_face);
    }
    if (auto m = warn_src_raw->map(drm::MapAccess::Write); m) {
      paint_warning_indicators(*m, warn_phases, font_face);
    }
  };
  std::array<double, k_warn_count> const initial_warn_phases{0.0, 0.0, 0.0, 0.0};
  repaint_layers(0.0, 0.0, 0U, initial_warn_phases);

  // Optional rear-view layer setup. Probe for the vicodec encoder +
  // stateful decoder pair; if both are present, drive the encoder
  // once at startup to produce the looping FWHT clip the rear-view
  // toggle will replay through V4l2DecoderSource. Failing to find
  // either endpoint disables the toggle but doesn't fail startup.
  RearViewState rear_view;
  auto const rear_x = static_cast<std::int32_t>(fb_w) - static_cast<std::int32_t>(k_rear_w) - 32;
  auto const rear_y = static_cast<std::int32_t>(32);

  // Source pickup order: UVC first (when libyuv is built in and a UVC
  // camera is attached) because a real camera reads more authentically
  // as a rear-view than an FWHT test pattern, and the libyuv path
  // bypasses amdgpu DC's refusal to PRIME-import foreign vmalloc
  // dmabufs. Fall back to the vicodec encoder/decoder pair driving
  // V4l2DecoderSource zero-copy if no UVC camera is found.
  bool rear_view_armed = false;
#if CLUSTER_SIM_HAS_LIBYUV
  if (auto uvc_path = find_uvc_endpoint(); uvc_path.has_value()) {
    // STREAMON is the slow step (USB-bandwidth negotiation can take
    // 100-300 ms on a UVC class device); do it once at startup so the
    // R toggle stays cheap and doesn't visibly stall dial animation.
    if (setup_uvc_capture(rear_view.uvc, *uvc_path, k_rear_w, k_rear_h)) {
      rear_view.uvc_path = *uvc_path;
      drm::println("rear-view: ready (UVC at {}, YUY2->XRGB via libyuv); press R to toggle",
                   rear_view.uvc_path);
      rear_view_armed = true;
    } else {
      drm::println(stderr, "rear-view: UVC setup failed at {}; falling back to vicodec", *uvc_path);
    }
  }
#endif
  if (!rear_view_armed) {
    if (auto enc_path = find_vicodec_endpoint(/*want_encoder=*/true); enc_path.has_value()) {
      auto dec_path = find_vicodec_endpoint(/*want_encoder=*/false);
      if (dec_path.has_value()) {
        rear_view.clip = encode_fwht_clip(*enc_path);
        if (!rear_view.clip.empty()) {
          rear_view.decoder_path = *dec_path;
          // Probe vicodec viability against the actual DRM device
          // before announcing the tier ready: V4l2DecoderSource::create
          // does the AddFB2 dance internally, which is exactly what
          // amdgpu DC's foreign-dmabuf provenance check rejects.
          // Constructing once and immediately destroying is cheap and
          // catches the rejection before the user presses R.
          drm::scene::V4l2DecoderConfig probe_cfg;
          probe_cfg.codec_fourcc = k_rear_codec_fourcc;
          probe_cfg.capture_fourcc = k_rear_capture_fourcc;
          probe_cfg.coded_width = k_rear_coded_w;
          probe_cfg.coded_height = k_rear_coded_h;
          probe_cfg.output_buffer_count = k_rear_buffer_count;
          probe_cfg.capture_buffer_count = k_rear_buffer_count;
          probe_cfg.output_buffer_size = k_rear_output_buffer_size;
          auto probe = drm::scene::V4l2DecoderSource::create(dev, dec_path->c_str(), probe_cfg);
          if (probe) {
            // probe destroyed at end of scope; clip stays for the
            // toggle to rebuild a fresh source on R press.
            drm::println(
                "rear-view: ready ({} frames encoded via {}, decoder {}); "
                "press R to toggle",
                rear_view.clip.size(), *enc_path, rear_view.decoder_path);
            rear_view_armed = true;
          } else {
            drm::println(stderr,
                         "rear-view: vicodec import probe failed: {} "
                         "(driver rejects foreign-source PRIME imports as FBs); "
                         "falling back to synthetic Blend2D pattern",
                         probe.error().message());
            rear_view.clip.clear();
            rear_view.decoder_path.clear();
          }
        } else {
          drm::println(stderr,
                       "rear-view: encoder probe at {} produced no frames; "
                       "falling back to synthetic Blend2D pattern",
                       *enc_path);
        }
      } else {
        drm::println(stderr,
                     "rear-view: vicodec encoder found at {} but no stateful "
                     "decoder; falling back to synthetic Blend2D pattern",
                     *enc_path);
      }
    }
  }

  if (!rear_view_armed) {
    rear_view.synthetic_armed = true;
    drm::println("rear-view: ready (synthetic Blend2D test pattern); press R to toggle");
  }

  bool flip_pending = false;
  // need_repaint drives the per-frame dial-paint cycle. Set true on
  // every page-flip-event landing so the next loop iteration repaints
  // and commits, giving us a flip-driven ~vsync-locked animation
  // cadence without a wall-clock timer.
  bool need_repaint = false;
  drm::PageFlip page_flip(dev);
  page_flip.set_handler([&](std::uint32_t /*c*/, std::uint64_t /*s*/, std::uint64_t /*t*/) {
    flip_pending = false;
    need_repaint = true;
  });

  // libinput keyboard + VT-switch chord forwarding. libseat puts the
  // TTY in KD_GRAPHICS where the kernel suppresses Ctrl-C signal
  // generation, so the libinput keyboard is the only reliable in-app
  // quit path on a real VT.
  drm::input::InputDeviceOpener libinput_opener;
  if (seat) {
    libinput_opener = seat->input_opener();
  }
  auto input_seat_r = drm::input::Seat::open({}, std::move(libinput_opener));
  if (!input_seat_r) {
    drm::println(stderr, "drm::input::Seat::open: {} (need root or 'input' group membership)",
                 input_seat_r.error().message());
    return EXIT_FAILURE;
  }
  auto& input_seat = *input_seat_r;
  bool quit = false;
  drm::examples::VtChordTracker vt_chord;
  input_seat.set_event_handler([&](const drm::input::InputEvent& event) {
    const auto* ke = std::get_if<drm::input::KeyboardEvent>(&event);
    if (ke == nullptr) {
      return;
    }
    if (vt_chord.observe(*ke, seat ? &*seat : nullptr)) {
      return;
    }
    if (vt_chord.is_quit_key(*ke)) {
      quit = true;
      return;
    }
    if (ke->pressed && ke->key == KEY_R) {
      bool const have_source =
#if CLUSTER_SIM_HAS_LIBYUV
          !rear_view.uvc_path.empty() ||
#endif
          !rear_view.clip.empty() || rear_view.synthetic_armed;
      if (rear_view.layer.has_value()) {
        toggle_rearview_off(rear_view, *scene);
        drm::println("rear-view: off");
      } else if (have_source) {
        if (toggle_rearview_on(rear_view, dev, *scene, rear_x, rear_y)) {
          drm::println("rear-view: on");
        }
      }
    }
  });

  // Session pause/resume bookkeeping. Both flags are touched from the
  // main thread only — libseat callbacks fire from inside
  // seat->dispatch(), which runs in the main loop. Defer the actual
  // device-fd swap until the loop's next iteration so we don't tear
  // down PageFlip mid-dispatch.
  bool session_paused = false;
  int pending_resume_fd = -1;
  if (seat) {
    seat->set_pause_callback([&]() {
      session_paused = true;
      flip_pending = false;
      scene->on_session_paused();
      (void)input_seat.suspend();
    });
    seat->set_resume_callback([&](std::string_view path, int new_fd) {
      if (path.substr(0, 9) != "/dev/dri/") {
        return;
      }
      pending_resume_fd = new_fd;
      session_paused = false;
      (void)input_seat.resume();
    });
  }

  if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    drm::println(stderr, "first commit failed: {}", r.error().message());
    return EXIT_FAILURE;
  }
  flip_pending = true;
  auto const start_time = std::chrono::steady_clock::now();
  drm::println("cluster_sim: {}x{} — Esc / Q to quit", fb_w, fb_h);

  // pfds[0..2] are stable; pfds[3] tracks the optional UVC fd, set to
  // -1 when the rear-view is off so poll() ignores it.
  pollfd pfds[4]{};
  pfds[0].fd = input_seat.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = dev.fd();
  pfds[1].events = POLLIN;
  pfds[2].fd = seat ? seat->poll_fd() : -1;
  pfds[2].events = POLLIN;
  pfds[3].fd = -1;
  pfds[3].events = POLLIN;

  while (!quit) {
#if CLUSTER_SIM_HAS_LIBYUV
    // Slot the UVC fd into the poll set whenever a UVC rear-view is
    // engaged; clear it when toggled off (or never enabled).
    pfds[3].fd = (rear_view.layer.has_value() && rear_view.uvc.fd >= 0) ? rear_view.uvc.fd : -1;
#endif
    int const timeout = flip_pending ? 16 : -1;
    if (int const ret = poll(pfds, 4, timeout); ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      drm::println(stderr, "poll: {}", std::system_category().message(errno));
      break;
    }
    if ((pfds[0].revents & POLLIN) != 0) {
      (void)input_seat.dispatch();
    }
    if ((pfds[1].revents & POLLIN) != 0) {
      (void)page_flip.dispatch(0);
    }
    if ((pfds[2].revents & POLLIN) != 0 && seat) {
      seat->dispatch();
    }
#if CLUSTER_SIM_HAS_LIBYUV
    if ((pfds[3].revents & POLLIN) != 0) {
      drive_rearview_uvc(rear_view);
    }
#endif

    if (pending_resume_fd != -1) {
      int const new_fd = pending_resume_fd;
      pending_resume_fd = -1;
      ctx.device = drm::Device::from_fd(new_fd);
      pfds[1].fd = dev.fd();
      if (auto r = dev.enable_universal_planes(); !r) {
        drm::println(stderr, "resume: enable_universal_planes failed");
        break;
      }
      if (auto r = dev.enable_atomic(); !r) {
        drm::println(stderr, "resume: enable_atomic failed");
        break;
      }
      if (auto r = scene->on_session_resumed(dev); !r) {
        drm::println(stderr, "resume: on_session_resumed: {}", r.error().message());
        break;
      }
      page_flip = drm::PageFlip(dev);
      page_flip.set_handler([&](std::uint32_t, std::uint64_t, std::uint64_t) {
        flip_pending = false;
        need_repaint = true;
      });
      // Buffer mappings were torn down on pause; repaint the bg + both
      // dials against the fresh mappings before the resume commit so
      // the post-resume frame is intact rather than stale-or-blank.
      if (auto m = bg_src_raw->map(drm::MapAccess::Write); m) {
        paint_bg_gradient(*m, fb_w, fb_h);
      }
      double const elapsed_resume =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
      double const speedo_resume = dial_norm_from_phase(elapsed_resume / k_speedo_period_s);
      double const tach_resume = dial_norm_from_phase(elapsed_resume / k_tach_period_s);
      auto const speed_resume_kmh =
          static_cast<std::uint32_t>(speedo_resume * static_cast<double>(k_info_speed_max_kmh));
      std::array<double, k_warn_count> warn_phases_resume{};
      for (int i = 0; i < k_warn_count; ++i) {
        double const period = k_warn_specs.at(static_cast<std::size_t>(i)).period_s;
        warn_phases_resume.at(static_cast<std::size_t>(i)) =
            std::fmod(elapsed_resume / period, 1.0);
      }
      repaint_layers(speedo_resume, tach_resume, speed_resume_kmh, warn_phases_resume);
      if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
        drm::println(stderr, "resume commit failed: {}", r.error().message());
        break;
      }
      flip_pending = true;
    }

    // Per-frame paint cycle: only when the prior flip has landed and
    // the session is live. Repaint both dials against their current
    // animation phase, then commit -- the kernel queues the flip for
    // the next vblank, the page-flip handler clears flip_pending and
    // sets need_repaint again, and the loop ticks at scanout cadence.
    if (need_repaint && !flip_pending && !session_paused) {
      need_repaint = false;
      double const elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
      double const speedo_norm = dial_norm_from_phase(elapsed / k_speedo_period_s);
      double const tach_norm = dial_norm_from_phase(elapsed / k_tach_period_s);
      auto const speed_kmh =
          static_cast<std::uint32_t>(speedo_norm * static_cast<double>(k_info_speed_max_kmh));
      std::array<double, k_warn_count> warn_phases{};
      for (int i = 0; i < k_warn_count; ++i) {
        double const period = k_warn_specs.at(static_cast<std::size_t>(i)).period_s;
        warn_phases.at(static_cast<std::size_t>(i)) = std::fmod(elapsed / period, 1.0);
      }
      repaint_layers(speedo_norm, tach_norm, speed_kmh, warn_phases);
      // Rear-view is decoder-driven (vicodec) or pump-driven (UVC) so
      // those tiers don't repaint here. The synthetic tier is the
      // exception: its content lives in a dumb buffer we own and we
      // refresh the scan-bar pattern each frame to keep it animated.
      if (rear_view.synthetic_source != nullptr) {
        if (auto m = rear_view.synthetic_source->map(drm::MapAccess::Write); m) {
          paint_rearview_synthetic(*m, elapsed, font_face);
        }
      }
      drive_rearview(rear_view);
      auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, &page_flip);
      if (!r) {
        if (r.error() == std::errc::permission_denied) {
          // Master got revoked between the flip event and the next
          // commit (libseat hasn't fired pause_cb yet). Treat as a
          // soft pause and let the resume path put us back together.
          session_paused = true;
          flip_pending = false;
          continue;
        }
        drm::println(stderr, "commit failed: {}", r.error().message());
        break;
      }
      flip_pending = true;
    }
  }

#if CLUSTER_SIM_HAS_LIBYUV
  // Tear down the always-on UVC stream cleanly. teardown is idempotent
  // and a no-op when no UVC was armed at startup.
  teardown_uvc_capture(rear_view.uvc);
#endif

  return EXIT_SUCCESS;
}
