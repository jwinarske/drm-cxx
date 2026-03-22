// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "edid.hpp"

extern "C" {
#include <libdisplay-info/edid.h>
#include <libdisplay-info/info.h>
}

namespace drm::display {

std::expected<ConnectorInfo, std::error_code> parse_edid(std::span<const uint8_t> edid_blob) {
  if (edid_blob.empty()) {
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  }

  auto* info = di_info_parse_edid(edid_blob.data(), edid_blob.size());
  if (!info) {
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  }

  ConnectorInfo result;

  // Name from make + model
  const char* make = di_info_get_make(info);
  const char* model = di_info_get_model(info);
  if (make) result.name += make;
  if (make && model) result.name += ' ';
  if (model) result.name += model;

  // Colorimetry from default color primaries
  const auto* primaries = di_info_get_default_color_primaries(info);
  if (primaries && primaries->has_primaries) {
    ColorimetryInfo ci{};
    ci.red = {primaries->primary[0].x, primaries->primary[0].y};
    ci.green = {primaries->primary[1].x, primaries->primary[1].y};
    ci.blue = {primaries->primary[2].x, primaries->primary[2].y};
    ci.white = {primaries->default_white.x, primaries->default_white.y};
    result.colorimetry = ci;
  }

  // HDR static metadata
  const auto* hdr = di_info_get_hdr_static_metadata(info);
  if (hdr) {
    bool has_hdr_data = hdr->desired_content_max_luminance > 0 ||
                        hdr->desired_content_min_luminance > 0 || hdr->pq || hdr->hlg;

    if (has_hdr_data) {
      HdrStaticMetadata md{
          .max_luminance = hdr->desired_content_max_luminance,
          .min_luminance = hdr->desired_content_min_luminance,
          .max_cll = hdr->desired_content_max_luminance,
          .max_fall = hdr->desired_content_max_frame_avg_luminance,
      };
      result.hdr = md;
    }

    // Supported EOTFs
    if (hdr->traditional_sdr) result.supported_eotfs.push_back(0);  // HDMI_EOTF_TRADITIONAL_SDR
    if (hdr->traditional_hdr) result.supported_eotfs.push_back(1);  // HDMI_EOTF_TRADITIONAL_HDR
    if (hdr->pq) result.supported_eotfs.push_back(2);               // HDMI_EOTF_SMPTE_ST2084
    if (hdr->hlg) result.supported_eotfs.push_back(3);              // HDMI_EOTF_BT2100_HLG
  }

  di_info_destroy(info);
  return result;
}

}  // namespace drm::display
