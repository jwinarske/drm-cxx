// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "double_dumb_source.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/dumb/buffer.hpp>
#include <drm-cxx/scene/buffer_source.hpp>

#include <drm_fourcc.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>

namespace drm::examples::camera {

namespace {

// Mirrors the dispatch in src/scene/dumb_buffer_source.cpp:
// semi-planar 4:2:0 fourccs go through `create_planar()`, everything
// else (packed XRGB / ARGB, the only single-plane shape the camera
// example currently feeds in) through the plain `create()`.
[[nodiscard]] bool is_semiplanar_yuv(std::uint32_t fourcc) noexcept {
  return fourcc == DRM_FORMAT_NV12 || fourcc == DRM_FORMAT_NV21 || fourcc == DRM_FORMAT_P010 ||
         fourcc == DRM_FORMAT_P012 || fourcc == DRM_FORMAT_P016;
}

[[nodiscard]] drm::expected<drm::dumb::Buffer, std::error_code> allocate_for_format(
    const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format) {
  if (is_semiplanar_yuv(drm_format)) {
    return drm::dumb::Buffer::create_planar(dev, drm_format, width, height);
  }
  drm::dumb::Config cfg;
  cfg.width = width;
  cfg.height = height;
  cfg.drm_format = drm_format;
  cfg.bpp = 32;
  cfg.add_fb = true;
  return drm::dumb::Buffer::create(dev, cfg);
}

}  // namespace

drm::expected<std::unique_ptr<DoubleDumbSource>, std::error_code> DoubleDumbSource::create(
    const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format) {
  auto a = allocate_for_format(dev, width, height, drm_format);
  if (!a) {
    return drm::unexpected<std::error_code>(a.error());
  }
  auto b = allocate_for_format(dev, width, height, drm_format);
  if (!b) {
    return drm::unexpected<std::error_code>(b.error());
  }

  drm::scene::SourceFormat fmt;
  fmt.drm_fourcc = drm_format;
  fmt.modifier = 0;  // DRM_FORMAT_MOD_LINEAR
  fmt.width = width;
  fmt.height = height;

  return std::unique_ptr<DoubleDumbSource>(new DoubleDumbSource(std::move(*a), std::move(*b), fmt));
}

void DoubleDumbSource::publish() noexcept {
  // Flip front/back. The producer is now safe to clobber what used to
  // be the front; the scene's next acquire() picks up what used to be
  // the back. Single-threaded against the main loop, so no atomics.
  front_idx_ = 1U - front_idx_;
}

drm::expected<drm::scene::AcquiredBuffer, std::error_code> DoubleDumbSource::acquire() {
  const auto& front = buffers_.at(front_idx_);
  if (front.empty() || front.fb_id() == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  drm::scene::AcquiredBuffer acq;
  acq.fb_id = front.fb_id();
  acq.acquire_fence_fd = -1;
  acq.opaque = nullptr;
  return acq;
}

void DoubleDumbSource::release(drm::scene::AcquiredBuffer /*acquired*/) noexcept {
  // No per-frame state to release; both buffers are owned permanently
  // by this source and handed out alternately on each acquire().
}

drm::expected<drm::BufferMapping, std::error_code> DoubleDumbSource::map(drm::MapAccess access) {
  // Reads see the freshest *published* frame (front buffer); writes go
  // to the back buffer so the producer can fill it without disturbing
  // the in-flight scanout.
  const std::size_t idx = (access == drm::MapAccess::Read) ? front_idx_ : (1U - front_idx_);
  auto& target = buffers_.at(idx);
  if (target.empty() || target.data() == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  return target.map(access);
}

void DoubleDumbSource::on_session_paused() noexcept {
  // No outstanding ioctls; commit path is dormant while the scene is
  // paused. State carries through to on_session_resumed which
  // re-allocates against the new fd.
}

drm::expected<void, std::error_code> DoubleDumbSource::on_session_resumed(
    const drm::Device& new_dev) {
  // Old fd is dead; forget() drops the GEM + FB handles without
  // ioctls (forget zeros width/height/stride per the dumb::Buffer
  // contract, so we snapshot the dimensions from format_ first).
  const auto prev_width = format_.width;
  const auto prev_height = format_.height;
  const auto prev_format = format_.drm_fourcc;

  for (auto& b : buffers_) {
    b.forget();
  }
  auto a = allocate_for_format(new_dev, prev_width, prev_height, prev_format);
  if (!a) {
    return drm::unexpected<std::error_code>(a.error());
  }
  auto bb = allocate_for_format(new_dev, prev_width, prev_height, prev_format);
  if (!bb) {
    return drm::unexpected<std::error_code>(bb.error());
  }
  buffers_[0] = std::move(*a);
  buffers_[1] = std::move(*bb);
  front_idx_ = 0;
  return {};
}

}  // namespace drm::examples::camera
