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
#include <libdisplay-info/info.h>
}

namespace drm::display {

drm::expected<ConnectorInfo, std::error_code> parse_edid(drm::span<const uint8_t> edid_blob) {
  // EDID blocks are 128 bytes; reject obviously malformed data.
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

  // Name from make + model
  const char* make = di_info_get_make(info);
  const char* model = di_info_get_model(info);
  if (make != nullptr) {
    result.name += make;
  }
  if ((make != nullptr) && (model != nullptr)) {
    result.name += ' ';
  }
  if (model != nullptr) {
    result.name += model;
  }

  di_info_destroy(info);
  return result;
}

}  // namespace drm::display
