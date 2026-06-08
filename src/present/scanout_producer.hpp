// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
#pragma once
// present/scanout_producer.hpp
//
// The one renderer-specific seam of the present path. GL and Vulkan backends
// differ only in how they produce a renderable, exportable buffer; everything
// above this interface -- target discovery, modifier negotiation, the buffer
// ring, scene build, present pacing -- is shared.

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/scene/buffer_source.hpp>

#include <cstdint>
#include <memory>
#include <system_error>
#include <vector>

namespace drm::present {

// Abstract producer of scanout-capable buffers. One concrete producer per
// renderer (GBM/EGL, Vulkan). Implementations own their device and the buffers
// they hand out via the returned LayerBufferSource.
class ScanoutProducer {
 public:
  ScanoutProducer() = default;
  virtual ~ScanoutProducer() = default;
  ScanoutProducer(const ScanoutProducer&) = delete;
  ScanoutProducer& operator=(const ScanoutProducer&) = delete;
  ScanoutProducer(ScanoutProducer&&) = delete;
  ScanoutProducer& operator=(ScanoutProducer&&) = delete;

  // Modifiers this producer can render into and export for `fourcc`. The shared
  // negotiator (drm::present::negotiate) intersects these with the display
  // plane's IN_FORMATS set. GBM producers return the GBM-renderable set; Vulkan
  // producers return the VkDrmFormatModifierPropertiesListEXT entries.
  [[nodiscard]] virtual std::vector<std::uint64_t> exportable_modifiers(std::uint32_t fourcc) = 0;

  // Allocate one renderable buffer for `fourcc` at width x height against
  // `allowed` (the negotiated, ranked modifier set, most-preferred first). The
  // returned source feeds the scene; the producer owns making each acquired
  // buffer safe to scan out. The atomic TEST_ONLY commit remains the arbiter of
  // whatever modifier from `allowed` the producer actually picks.
  [[nodiscard]] virtual drm::expected<std::unique_ptr<scene::LayerBufferSource>, std::error_code>
  create_buffer(std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
                drm::span<const std::uint64_t> allowed) = 0;
};

}  // namespace drm::present
