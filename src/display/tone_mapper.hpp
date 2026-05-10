// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// tone_mapper.hpp — CPU tone-mapping fallback.
//
// When neither the CRTC pipeline nor the per-plane
// pipeline is exposed by the kernel, applications still
// need to mix HDR + SDR layers. `drm::display::ToneMapper` ships a
// per-pixel functor that handles the three highest-leverage
// cases: SDR → HDR pre-amp (BT.709 → BT.2020 PQ), HDR → SDR clamp
// (BT.2020 PQ / HLG → BT.709), with caller-selectable tone-map
// curves for the HDR-to-SDR direction.
//
// Pixel layout: little-endian `std::uint64_t` packing four `u16`
// channels: bits 0..15 = R, 16..31 = G, 32..47 = B, 48..63 = A.
// Alpha passes through unchanged. Callers operating on 8-bit
// surfaces are expected to pre-expand to u16 (multiply by 257) and
// re-quantize after.
//
// The mapper is move-only and cheap to construct; configurations
// are immutable (re-create with a new `target_max_nits` to change).

#pragma once

#include <cstdint>

namespace drm::display {

/// Curve applied during the linear-light HDR → SDR step. The choice
/// is a quality / complexity tradeoff:
///
///   * `Reinhard` — `x / (1 + x)`. Cheapest; preserves shadows, rolls
///     highlights smoothly. The design's default.
///   * `Hable` — Uncharted 2 / John Hable filmic curve. ACES-ish
///     S-shape; preserves contrast better at the cost of slight
///     mid-tone shift.
///   * `None` — clamp to 1.0. Useful for HLG (BT.2100 OOTF already
///     does the heavy lifting before we hit this stage) and for
///     test pipelines that want the matrix + EOTF without any
///     subjective tone-map.
enum class ToneMapCurve : std::uint8_t {
  None,
  Reinhard,
  Hable,
};

class ToneMapper {
 public:
  /// Direction the mapper transforms.
  enum class Direction : std::uint8_t {
    Bt709ToBt2020Pq,
    Bt2020PqToBt709,
    HlgToBt709,
  };

  /// SDR (BT.709 / sRGB encoded) → HDR (BT.2020 / PQ encoded).
  /// `sdr_white_nits` is where the SDR `1.0` lands on the PQ curve;
  /// 100 cd/m² is the conventional default and matches how most
  /// HDR displays render their SDR signal. Higher values (203 nits
  /// is a common "diffuse white" reference) brighten the SDR
  /// portion within the HDR composition.
  [[nodiscard]] static ToneMapper bt709_to_bt2020_pq(float sdr_white_nits = 100.0F) noexcept;

  /// HDR (BT.2020 / PQ encoded) → SDR (BT.709 encoded). The
  /// `target_max_nits` is what the SDR `1.0` represents in absolute
  /// terms; values above that are tone-mapped through `curve` so
  /// HDR highlights compress into SDR range without clipping.
  /// Reinhard is the design's default and the safest choice for
  /// content with extreme dynamic range.
  [[nodiscard]] static ToneMapper bt2020_pq_to_bt709(
      float target_max_nits = 100.0F, ToneMapCurve curve = ToneMapCurve::Reinhard) noexcept;

  /// HLG (BT.2100 encoded) → SDR (BT.709 encoded). HLG's OOTF (the
  /// system-gamma factor between scene-linear and display-linear)
  /// uses a target gamma of 1.2 nominal; the mapper applies a
  /// fixed gamma matched to the SDR display target.
  [[nodiscard]] static ToneMapper hlg_to_bt709(float target_max_nits = 100.0F) noexcept;

  /// Apply the configured transform to one pixel.
  [[nodiscard]] std::uint64_t operator()(std::uint64_t input_pixel) const noexcept;

  // Diagnostics
  [[nodiscard]] Direction direction() const noexcept { return direction_; }
  [[nodiscard]] ToneMapCurve curve() const noexcept { return curve_; }
  [[nodiscard]] float sdr_white_nits() const noexcept { return sdr_white_nits_; }
  [[nodiscard]] float target_max_nits() const noexcept { return target_max_nits_; }

 private:
  ToneMapper(Direction direction, float sdr_white, float target_max, ToneMapCurve curve) noexcept;

  Direction direction_;
  ToneMapCurve curve_;
  float sdr_white_nits_;
  float target_max_nits_;
};

}  // namespace drm::display
