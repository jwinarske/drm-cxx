// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
#pragma once
// present/gbm_producer.hpp
//
// The simplest ScanoutProducer: a single CPU-mappable LINEAR GBM scanout buffer
// per create_buffer (backed by scene::GbmBufferSource). Suitable for software /
// static content and for driving the backend on any KMS device, including
// LINEAR-only ones. GPU producers (EGL/Vulkan) implement the same interface with
// their own multi-buffer rings.

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/present/scanout_producer.hpp>
#include <drm-cxx/scene/buffer_source.hpp>

#include <cstdint>
#include <memory>
#include <system_error>
#include <vector>

namespace drm {
class Device;
}

namespace drm::present {

class GbmScanoutProducer : public ScanoutProducer {
 public:
  // Borrows `dev`; it must outlive the producer and any buffer it hands out.
  explicit GbmScanoutProducer(drm::Device& dev) noexcept : dev_(&dev) {}

  [[nodiscard]] std::vector<std::uint64_t> exportable_modifiers(std::uint32_t fourcc) override;
  [[nodiscard]] drm::expected<std::unique_ptr<scene::LayerBufferSource>, std::error_code>
  create_buffer(std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
                drm::span<const std::uint64_t> allowed) override;

 private:
  drm::Device* dev_;
};

}  // namespace drm::present
