// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "edid.hpp"

#include "display/connector_info.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <cstdint>
#include <cstring>
#include <system_error>

extern "C" {
#include <libdisplay-info/edid.h>
#include <libdisplay-info/info.h>
}

namespace drm::display {

namespace {

void populate_name(ConnectorInfo& out, const struct di_info* info) {
  const char* make = di_info_get_make(info);
  const char* model = di_info_get_model(info);
  if (make != nullptr) {
    out.name += make;
  }
  if ((make != nullptr) && (model != nullptr)) {
    out.name += ' ';
  }
  if (model != nullptr) {
    out.name += model;
  }
}

void populate_colorimetry(ConnectorInfo& out, const struct di_info* info) {
  const struct di_color_primaries* primaries = di_info_get_default_color_primaries(info);
  if (primaries == nullptr) {
    return;
  }
  if (!primaries->has_primaries && !primaries->has_default_white_point) {
    return;
  }
  ColorimetryInfo c;
  c.has_primaries = primaries->has_primaries;
  c.has_default_white = primaries->has_default_white_point;
  if (c.has_primaries) {
    c.red = {primaries->primary[0].x, primaries->primary[0].y};
    c.green = {primaries->primary[1].x, primaries->primary[1].y};
    c.blue = {primaries->primary[2].x, primaries->primary[2].y};
  }
  if (c.has_default_white) {
    c.white = {primaries->default_white.x, primaries->default_white.y};
  }
  out.colorimetry = c;
}

void populate_hdr(ConnectorInfo& out, const struct di_info* info) {
  const struct di_hdr_static_metadata* hsm = di_info_get_hdr_static_metadata(info);
  if (hsm == nullptr) {
    return;
  }
  HdrStaticMetadata m;
  m.desired_content_max_luminance = hsm->desired_content_max_luminance;
  m.desired_content_max_frame_avg_luminance = hsm->desired_content_max_frame_avg_luminance;
  m.desired_content_min_luminance = hsm->desired_content_min_luminance;
  m.type1 = hsm->type1;
  m.traditional_sdr = hsm->traditional_sdr;
  m.traditional_hdr = hsm->traditional_hdr;
  m.pq = hsm->pq;
  m.hlg = hsm->hlg;
  out.hdr = m;
}

void populate_wide_gamut(ConnectorInfo& out, const struct di_info* info) {
  const struct di_supported_signal_colorimetry* sgc =
      di_info_get_supported_signal_colorimetry(info);
  if (sgc == nullptr) {
    return;
  }
  SupportedColorimetry w;
  w.bt2020_cycc = sgc->bt2020_cycc;
  w.bt2020_ycc = sgc->bt2020_ycc;
  w.bt2020_rgb = sgc->bt2020_rgb;
  w.st2113_rgb = sgc->st2113_rgb;
  w.ictcp = sgc->ictcp;
  out.wide_gamut = w;
}

// Pull the vertical-refresh range from the EDID Display Range Limits descriptor
// (tag 0xFD). For a VRR panel this is its adaptive-sync range. Walks the
// NULL-terminated descriptor list for the range-limits one.
void populate_range_limits(ConnectorInfo& out, const struct di_info* info) {
  const struct di_edid* edid = di_info_get_edid(info);
  if (edid == nullptr) {
    return;
  }
  const struct di_edid_display_descriptor* const* descs = di_edid_get_display_descriptors(edid);
  if (descs == nullptr) {
    return;
  }
  for (std::size_t i = 0; descs[i] != nullptr; ++i) {
    if (di_edid_display_descriptor_get_tag(descs[i]) != DI_EDID_DISPLAY_DESCRIPTOR_RANGE_LIMITS) {
      continue;
    }
    const struct di_edid_display_range_limits* rl =
        di_edid_display_descriptor_get_range_limits(descs[i]);
    if (rl != nullptr && rl->max_vert_rate_hz > 0) {
      out.vrefresh_range = VrefreshRange{rl->min_vert_rate_hz, rl->max_vert_rate_hz};
    }
    return;
  }
}

}  // namespace

drm::expected<ConnectorInfo, std::error_code> parse_edid(drm::span<const uint8_t> edid_blob) {
  // EDID blocks are 128 bytes; reject obviously malformed data before
  // calling into libdisplay-info so we return invalid_argument rather
  // than a more cryptic backend error.
  static constexpr uint8_t edid_header[] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  if (edid_blob.size() < 128) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  if (std::memcmp(edid_blob.data(), edid_header, sizeof(edid_header)) != 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  auto* info = di_info_parse_edid(edid_blob.data(), edid_blob.size());
  if (info == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  ConnectorInfo result;
  populate_name(result, info);
  populate_colorimetry(result, info);
  populate_hdr(result, info);
  populate_wide_gamut(result, info);
  populate_range_limits(result, info);

  di_info_destroy(info);
  return result;
}

}  // namespace drm::display