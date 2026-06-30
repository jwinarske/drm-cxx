// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>

namespace drm::display {

/// CIE 1931 2-degree observer chromaticity coordinates.
struct Chromaticity {
  float x{};
  float y{};
};

/// Display color primaries and default white point, derived from EDID.
/// Coordinates are display-referred CIE xy. Values may be all-zero
/// when the display doesn't publish primaries (most monochrome / very-
/// low-end panels).
struct ColorimetryInfo {
  Chromaticity red{};
  Chromaticity green{};
  Chromaticity blue{};
  Chromaticity white{};
  bool has_primaries{false};
  bool has_default_white{false};
};

/// HDR static metadata data block contents per ANSI/CTA-861-H §7.5.13.
/// Describes what HDR signaling the *display* accepts (capability),
/// not what the source emits — the source-side equivalent is the
/// HDR_OUTPUT_METADATA blob.
struct HdrStaticMetadata {
  /// Desired content max luminance, cd/m². Zero when unset.
  float desired_content_max_luminance{};
  /// Desired content max frame-average luminance, cd/m². Zero when unset.
  float desired_content_max_frame_avg_luminance{};
  /// Desired content min luminance, cd/m². Zero when unset.
  float desired_content_min_luminance{};

  /// Static Metadata Type 1 (CTA-861.3 §6.9.1) is supported.
  bool type1{false};

  /// Supported EOTFs:
  bool traditional_sdr{false};
  bool traditional_hdr{false};
  bool pq{false};   // SMPTE ST 2084 — HDR10 / HDR10+
  bool hlg{false};  // BT.2100 — HLG
};

/// Wide-gamut signal colorimetry encodings the display accepts beyond
/// the default RGB colorimetry (which `ColorimetryInfo` describes).
struct SupportedColorimetry {
  bool bt2020_cycc{false};  // BT.2020 constant-luminance YCbCr
  bool bt2020_ycc{false};   // BT.2020 non-constant-luminance YCbCr
  bool bt2020_rgb{false};
  bool st2113_rgb{false};  // SMPTE ST 2113 — P3D65 + P3DCI
  bool ictcp{false};       // BT.2100 ICtCp HDR (with PQ and/or HLG)
};

// EDID display range limits — the panel's vertical-refresh range in Hz, from
// the EDID Display Range Limits descriptor (tag 0xFD). For a variable-refresh
// panel this is its VRR range (e.g. 40–90 on the Steam Deck OLED); for a
// fixed-rate panel min==max. nullopt when the EDID carries no range descriptor.
struct VrefreshRange {
  int min_hz{0};
  int max_hz{0};
};

struct ConnectorInfo {
  std::string name;

  // EDID identity. `make` and `model` are also concatenated into `name` for a
  // human-readable label; they are exposed separately here for matching and
  // display. `serial` is the per-unit serial number (often absent). All are
  // empty / nullopt when the EDID does not carry them.
  std::string make;                   // manufacturer (company name or PNP ID)
  std::string model;                  // product name / model
  std::optional<std::string> serial;  // per-unit serial number

  // Physical screen size in millimeters; zero when the EDID does not state it.
  // EDID carries whole-centimeter precision, so these are multiples of 10.
  int width_mm{0};
  int height_mm{0};

  std::optional<ColorimetryInfo> colorimetry;
  std::optional<HdrStaticMetadata> hdr;
  std::optional<SupportedColorimetry> wide_gamut;
  std::optional<VrefreshRange> vrefresh_range;
};

}  // namespace drm::display