// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace drm::display {

struct ColorimetryInfo {
  struct { float x, y; } red, green, blue, white;
};

struct HdrStaticMetadata {
  float max_luminance{};
  float min_luminance{};
  float max_cll{};
  float max_fall{};
};

struct ConnectorInfo {
  std::string name;
  std::optional<ColorimetryInfo> colorimetry;
  std::optional<HdrStaticMetadata> hdr;
  std::vector<uint32_t> supported_eotfs;
};

} // namespace drm::display
