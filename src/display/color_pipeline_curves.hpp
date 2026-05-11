// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// color_pipeline_curves.hpp — LUT and CTM builders for the CRTC
// (and per-plane, future ) color pipelines.
//
// The kernel's `drm_color_lut` is a per-channel input→output map
// stored as 16-bit unsigned values, written to the DEGAMMA_LUT /
// GAMMA_LUT blob properties. `drm_color_ctm` is a 3×3 matrix in
// S31.32 sign-magnitude fixed-point, written to CTM. These helpers
// build the LUT entries / matrix coefficients for the standard
// transfer functions and gamut conversions HDR plumbing wants to
// reach for; the resulting structures are agnostic of which
// property they end up on (DEGAMMA vs. GAMMA, per-CRTC vs.
// per-plane).
//
// Curves are kept as separate free functions rather than members of
// a pipeline class so unit tests can exercise the math against
// published reference values without having to instantiate a kernel
// blob.

#pragma once

#include <drm-cxx/detail/span.hpp>

#include <drm/drm_mode.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace drm::display {

/// Encode a real-valued matrix coefficient as the kernel's S31.32
/// sign-magnitude fixed-point representation:
///   * bit 63: sign (1 == negative)
///   * bits 62..32: integer magnitude (31 bits)
///   * bits 31..0: fractional magnitude (32 bits)
///
/// Note: this is *not* two's complement — the kernel docs were
/// updated in 6.x to clarify the format. Values out of S31.32 range
/// saturate to the largest representable magnitude.
[[nodiscard]] std::uint64_t encode_s31_32(double v) noexcept;

/// Decode the kernel's S31.32 sign-magnitude representation back to
/// a real value. Inverse of `encode_s31_32`.
[[nodiscard]] double decode_s31_32(std::uint64_t encoded) noexcept;

/// Quantize a normalized real value in `[0, 1]` to the kernel's
/// 16-bit LUT-entry representation. Values below 0 or above 1
/// clamp; rounding is half-up.
[[nodiscard]] std::uint16_t quantize_lut_value(double normalized) noexcept;

// ── Float-domain transfer functions ─────────────────────────────────
//
// Per-channel curves used by both the LUT builders below and the
// CPU tone-mapper. All operate on normalized values in
// `[0, 1]` and clamp out-of-range inputs.

/// SMPTE ST 2084 (PQ) EOTF — encoded → linear. Output `1.0` represents
/// the PQ peak (10 000 cd/m²).
[[nodiscard]] double pq_eotf(double encoded) noexcept;

/// SMPTE ST 2084 (PQ) OETF — linear → encoded. Inverse of `pq_eotf`.
[[nodiscard]] double pq_oetf(double linear) noexcept;

/// ITU-R BT.2100 HLG OETF^-1 — encoded → scene-linear (no OOTF).
[[nodiscard]] double hlg_oetf_inverse(double encoded) noexcept;

/// IEC 61966-2-1 sRGB EOTF — encoded → linear. Used for SDR sources.
[[nodiscard]] double srgb_eotf(double encoded) noexcept;

/// IEC 61966-2-1 sRGB OETF — linear → encoded.
[[nodiscard]] double srgb_oetf(double linear) noexcept;

/// ITU-R BT.1886 OETF — linear → encoded with a fixed gamma of 2.4.
/// Use as the SDR output curve when the display is broadcast-style
/// (gamma 2.4); use `srgb_oetf` for desktop / mobile sRGB displays.
[[nodiscard]] double bt1886_oetf(double linear) noexcept;

// ── LUT builders ────────────────────────────────────────────────────
//
// All builders fill `out` (size `out.size()` controls quantization
// granularity, typically the driver's `*_LUT_SIZE`) with a
// per-channel curve. `red`, `green`, `blue` are populated identically;
// the `reserved` field is left zero.

/// Identity LUT: `lut[i] = i / (N - 1)` quantized into u16. Useful
/// as a passthrough when only one stage of the pipeline is
/// configured but the kernel still demands a blob.
void build_identity_lut(drm::span<drm_color_lut> out) noexcept;

/// SMPTE ST 2084 (PQ) EOTF — encoded → linear. Maps PQ-encoded
/// values in `[0, 1]` to display-referred linear values in `[0, 1]`,
/// where `1.0` represents the PQ peak (10 000 cd/m²). Use this on
/// `DEGAMMA_LUT` when the source content is HDR10 / HDR10+ and you
/// want to operate in linear light through the rest of the
/// pipeline.
void build_pq_eotf_lut(drm::span<drm_color_lut> out) noexcept;

/// SMPTE ST 2084 (PQ) OETF — linear → encoded. Inverse of the
/// EOTF; goes on `GAMMA_LUT` when the *output* is HDR10 / HDR10+.
void build_pq_oetf_lut(drm::span<drm_color_lut> out) noexcept;

/// ITU-R BT.2100 HLG OETF^-1 — encoded → scene-linear. Maps HLG-
/// encoded values to scene-linear values without the OOTF system-
/// gamma factor (the display applies that). Goes on `DEGAMMA_LUT`
/// for HLG sources.
void build_hlg_oetf_inverse_lut(drm::span<drm_color_lut> out) noexcept;

// ── CTM builders ────────────────────────────────────────────────────

/// Identity 3×3 matrix encoded as S31.32 sign-magnitude per entry.
/// Diagonal is `1.0`, off-diagonal is `0.0`.
[[nodiscard]] drm_color_ctm build_identity_ctm() noexcept;

/// BT.2020 → BT.709 RGB matrix (linear-light). From ITU-R BT.2087.
/// Use only after a DEGAMMA stage that produces linear-light values
/// — applying this matrix to gamma-encoded values is incorrect.
[[nodiscard]] drm_color_ctm build_bt2020_to_bt709_ctm() noexcept;

/// BT.709 → BT.2020 RGB matrix (linear-light). The mathematical
/// inverse of `build_bt2020_to_bt709_ctm`.
[[nodiscard]] drm_color_ctm build_bt709_to_bt2020_ctm() noexcept;

}  // namespace drm::display
