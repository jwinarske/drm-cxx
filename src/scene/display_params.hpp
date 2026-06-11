// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// display_params.hpp — how the scene displays a layer's buffer.
//
// Separated from LayerBufferSource's SourceFormat per the KMS concept
// boundary: the buffer source describes what the buffer IS (format,
// modifier, intrinsic size), while DisplayParams describes how the
// plane that scans it out should be configured (src/dst rect, rotation,
// alpha, zpos). Same buffer can be displayed multiple ways; same
// plane configuration can scan different buffers.

#pragma once

#include <drm-cxx/display/connector_info.hpp>
#include <drm-cxx/display/hdr_metadata.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace drm::scene {

/// Container colorimetry of a layer's pixel data. Drives the connector
/// `Colorspace` enum at scene-lowering time and seeds the mastering
/// display primaries on `HDR_OUTPUT_METADATA` when the layer is HDR.
/// Distinct from `drm::display::Colorspace` which is the kernel-side
/// enum the driver advertises on a connector — `ColorPrimaries` is
/// the friendly producer-side description; the scene picks the
/// connector enum that best matches the widest-gamut layer at commit
/// time (see `output_signaling.hpp`).
enum class ColorPrimaries : std::uint8_t {
  Bt709,     // BT.709 / sRGB. SDR default.
  Bt2020,    // BT.2020 / Rec.2100. HDR + wide-gamut SDR.
  DciP3,     // DCI-P3 D65 (display-referred consumer P3).
  AdobeRgb,  // SMPTE Adobe RGB (1998).
};

/// Geometry primitive for src/dst rectangles. Aliased onto
/// `drm::planes::Rect` so scene → `planes::Layer` lowering is a
/// trivial field copy rather than a type conversion. `int32_t x/y`
/// match KMS `CRTC_X/Y` (signed — planes can extend off-screen);
/// `uint32_t w/h` match `CRTC_W/H`.
using Rect = drm::planes::Rect;

/// amdgpu per-plane color pipeline — the `AMD_PLANE_*` properties of amdgpu's
/// DRM/KMS color-management uAPI, used for HDR / tone-mapping / wide-gamut.
/// Presence-gated: planes or drivers without the properties ignore this
/// silently. Landing incrementally — stage 1 is the input degamma transfer
/// function plus the HDR luminance multiplier; the shaper / 3D-LUT / blend / CTM
/// (blob) stages follow.
/// Logical transfer-function selector for the amdgpu plane color stages. The
/// stage decides the direction: degamma + blend linearize an encoding (EOTF
/// direction), while the shaper (and the CRTC regamma) re-encode (inverse-EOTF).
/// The scene resolves the selection against each property's live enum list by
/// name, so the same selector maps to the right driver enum per stage.
enum class PlaneTransferFunction : std::uint8_t {
  Default,  ///< driver default
  Srgb,
  Bt709,
  Pq,  ///< HDR10
  Identity,
  Gamma22,
  Gamma24,
  Gamma26,
};

/// One RGB entry of an amdgpu color LUT / 3D-LUT. Values map linearly to
/// 0.0–1.0 (0x0 == 0.0, 0xffff == 1.0); packed into the kernel's `drm_color_lut`.
struct ColorLutEntry {
  std::uint16_t r{};
  std::uint16_t g{};
  std::uint16_t b{};
};

/// Per-layer amdgpu plane color-pipeline configuration, in pipeline order:
/// degamma → HDR mult → shaper(TF + LUT) → 3D-LUT → blend(TF + LUT), with a CTM.
/// Each field is independently optional/empty — unset means "don't touch that
/// property" (the plane keeps the driver default / whatever a previous
/// compositor left). The scene writes these onto the layer's assigned plane at
/// commit time, skipping any property the plane doesn't advertise. LUT vectors
/// must match the plane's advertised `AMD_PLANE_*_LUT_SIZE` (1D) or its cube
/// dimension `AMD_PLANE_LUT3D_SIZE`³ or the kernel rejects the commit.
struct AmdPlaneColor {
  /// `AMD_PLANE_DEGAMMA_TF` — linearizes the layer's input encoding (EOTF).
  std::optional<PlaneTransferFunction> degamma_tf;
  /// `AMD_PLANE_HDR_MULT` — luminance multiplier in linear light (S31.32 on the
  /// wire; 1.0 = identity).
  std::optional<double> hdr_mult;
  /// `AMD_PLANE_SHAPER_TF` — re-encodes linear → shaper space (inverse-EOTF).
  std::optional<PlaneTransferFunction> shaper_tf;
  /// `AMD_PLANE_SHAPER_LUT` — 1D shaper curve (size = `AMD_PLANE_SHAPER_LUT_SIZE`).
  std::vector<ColorLutEntry> shaper_lut;
  /// `AMD_PLANE_LUT3D` — gamut / tone-map cube (size = `AMD_PLANE_LUT3D_SIZE`³,
  /// blue-major then green then red).
  std::vector<ColorLutEntry> lut3d;
  /// `AMD_PLANE_BLEND_TF` — re-encodes into the blending space (EOTF).
  std::optional<PlaneTransferFunction> blend_tf;
  /// `AMD_PLANE_BLEND_LUT` — 1D blend curve (size = `AMD_PLANE_BLEND_LUT_SIZE`).
  std::vector<ColorLutEntry> blend_lut;
  /// `AMD_PLANE_CTM` — row-major 3×3 color matrix (packed S31.32 sign-magnitude).
  std::optional<std::array<double, 9>> ctm;
};

/// Per-layer display configuration. Lowered to plane properties at
/// commit time: src_rect → SRC_X/Y/W/H (scaled by 16 for the kernel's
/// 16.16 fixed-point convention); dst_rect → CRTC_X/Y/W/H; rotation →
/// the plane's rotation property (or software pre-rotation if the
/// plane lacks the property); alpha → plane.alpha property when
/// present; zpos → plane.zpos when set (unset = "let the allocator pick",
/// which is the right default when the layer doesn't care — e.g. a single-
/// layer scene shouldn't pre-filter out an amdgpu PRIMARY with an
/// immutable non-zero zpos).
///
/// needs_scaling is derived, not stored: src_rect.{w,h} != dst_rect.{w,h}
/// implies the plane must support scaling.
struct DisplayParams {
  Rect src_rect{};
  Rect dst_rect{};
  std::uint64_t rotation{0};    // DRM_MODE_ROTATE_* | DRM_MODE_REFLECT_*
  std::uint16_t alpha{0xFFFF};  // 0xFFFF = fully opaque
  std::optional<int> zpos;
  /// Override the YCbCr → RGB matrix the display engine applies to
  /// this layer's pixels. nullopt means "let LayerScene write its
  /// default" (BT.709), which neutralizes whatever stale value a
  /// previous compositor left on the plane. Only matters for YUV
  /// formats — RGB layers leave this alone and the scene still writes
  /// the default to keep the plane's state predictable.
  std::optional<drm::planes::ColorEncoding> color_encoding;
  /// Override `COLOR_RANGE` (limited vs. full). nullopt means "let
  /// LayerScene write its default" (limited, matching most camera /
  /// broadcast YCbCr).
  std::optional<drm::planes::ColorRange> color_range;

  /// Container colorimetry of this layer's pixel data. Drives the
  /// scene's choice of connector `Colorspace` (BT.2020 wins over
  /// DCI-P3 wins over Adobe RGB wins over BT.709). nullopt means
  /// "scene picks BT.709 SDR default."
  std::optional<ColorPrimaries> color_primaries;

  /// Source electro-optical transfer function. PQ + HLG are the HDR
  /// cases; TraditionalGammaHdr is rare (HDR10 over a BT.1886 path);
  /// the default in nullopt is the SDR transfer (BT.1886 / sRGB).
  /// When *any* layer in a scene reports an HDR EOTF the scene
  /// declares HDR signaling on the connector via
  /// `HDR_OUTPUT_METADATA`.
  std::optional<drm::display::TransferFunction> source_eotf;

  /// amdgpu per-plane color pipeline (`AMD_PLANE_*`). Default = all-unset =
  /// nothing written. See `AmdPlaneColor`.
  AmdPlaneColor amd_color{};

  [[nodiscard]] constexpr bool needs_scaling() const noexcept {
    return src_rect.w != dst_rect.w || src_rect.h != dst_rect.h;
  }
};

}  // namespace drm::scene
