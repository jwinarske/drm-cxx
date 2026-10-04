// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
// present/gbm_producer.cpp

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/present/gbm_producer.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/gbm_buffer_source.hpp>

#include <drm_fourcc.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::present {

std::vector<std::uint64_t> GbmScanoutProducer::exportable_modifiers(std::uint32_t /*fourcc*/) {
  // GbmBufferSource allocates with GBM_BO_USE_LINEAR, so LINEAR is all we export.
  return {DRM_FORMAT_MOD_LINEAR};
}

drm::expected<std::unique_ptr<scene::LayerBufferSource>, std::error_code>
GbmScanoutProducer::create_buffer(std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
                                  drm::span<const std::uint64_t> allowed) {
  // LINEAR-only producer (GbmBufferSource fixes GBM_BO_USE_SCANOUT |
  // GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE): honor `allowed` by refusing a set
  // without LINEAR rather than allocating a buffer no plane takes. Empty means
  // no constraint.
  if (!allowed.empty() &&
      std::find(allowed.begin(), allowed.end(), DRM_FORMAT_MOD_LINEAR) == allowed.end()) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_supported));
  }
  auto source = scene::GbmBufferSource::create(*dev_, width, height, fourcc);
  if (!source) {
    return drm::unexpected<std::error_code>(source.error());
  }
  return std::unique_ptr<scene::LayerBufferSource>(std::move(*source));
}

}  // namespace drm::present
