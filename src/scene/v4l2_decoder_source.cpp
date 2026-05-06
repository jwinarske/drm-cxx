// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "v4l2_decoder_source.hpp"

#include "buffer_source.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <system_error>

namespace drm::scene {

namespace {

constexpr std::uint32_t k_min_buffers = 2U;
constexpr std::uint32_t k_max_buffers = 32U;

[[nodiscard]] std::error_code validate_config(const char* device_path,
                                              const V4l2DecoderConfig& cfg) noexcept {
  if (device_path == nullptr || std::strlen(device_path) == 0U) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (cfg.codec_fourcc == 0U || cfg.capture_fourcc == 0U) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (cfg.coded_width == 0U || cfg.coded_height == 0U) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (cfg.output_buffer_count < k_min_buffers || cfg.output_buffer_count > k_max_buffers) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (cfg.capture_buffer_count < k_min_buffers || cfg.capture_buffer_count > k_max_buffers) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  return {};
}

}  // namespace

// Holds the V4L2 + DRM-side state. Hidden behind pimpl so the public
// header doesn't need <linux/videodev2.h>. Field set will be filled
// out alongside the V4L2 ioctl implementation; for now the impl is an
// empty type that lets the source own a non-null pointer through its
// lifecycle.
struct V4l2DecoderSource::Impl {
  int v4l2_fd{-1};
  V4l2DecoderConfig cfg{};
  // Future: OUTPUT/CAPTURE buffer rings, format() cache, the dup'd DRM
  // fd and per-CAPTURE-buffer fb_id table, the most-recent-decoded
  // slot index, etc.
};

V4l2DecoderSource::V4l2DecoderSource() : impl_(std::make_unique<Impl>()) {}

V4l2DecoderSource::~V4l2DecoderSource() = default;

drm::expected<std::unique_ptr<V4l2DecoderSource>, std::error_code> V4l2DecoderSource::create(
    const drm::Device& /*dev*/, const char* device_path, const V4l2DecoderConfig& cfg) {
  if (auto ec = validate_config(device_path, cfg); ec) {
    return drm::unexpected<std::error_code>(ec);
  }
  // V4L2 open + format negotiation + buffer alloc + DRM import lands
  // in a follow-up commit on this branch.
  return drm::unexpected<std::error_code>(std::make_error_code(std::errc::function_not_supported));
}

int V4l2DecoderSource::fd() const noexcept {
  return impl_ ? impl_->v4l2_fd : -1;
}

drm::expected<void, std::error_code> V4l2DecoderSource::drive() noexcept {
  if (!impl_) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  return drm::unexpected<std::error_code>(std::make_error_code(std::errc::function_not_supported));
}

drm::expected<void, std::error_code> V4l2DecoderSource::submit_bitstream(
    drm::span<const std::uint8_t> coded, std::uint64_t /*timestamp_ns*/) {
  if (!impl_ || coded.empty()) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  return drm::unexpected<std::error_code>(std::make_error_code(std::errc::function_not_supported));
}

drm::expected<AcquiredBuffer, std::error_code> V4l2DecoderSource::acquire() {
  if (!impl_) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  return drm::unexpected<std::error_code>(std::make_error_code(std::errc::function_not_supported));
}

void V4l2DecoderSource::release(AcquiredBuffer /*acquired*/) noexcept {
  // No-op until the V4L2 CAPTURE re-queue path lands.
}

SourceFormat V4l2DecoderSource::format() const noexcept {
  return SourceFormat{};
}

void V4l2DecoderSource::on_session_paused() noexcept {
  // No-op until DRM-side state (FB IDs, GEM handles) is held.
}

drm::expected<void, std::error_code> V4l2DecoderSource::on_session_resumed(
    const drm::Device& /*new_dev*/) {
  if (!impl_) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  return {};
}

}  // namespace drm::scene
