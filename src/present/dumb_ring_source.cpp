// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
// present/dumb_ring_source.cpp

#include "buffer_ring.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/dumb/buffer.hpp>
#include <drm-cxx/present/dumb_ring_source.hpp>
#include <drm-cxx/scene/buffer_source.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::present {

namespace {
[[nodiscard]] std::error_code err(std::errc code) noexcept {
  return std::make_error_code(code);
}
}  // namespace

drm::expected<std::unique_ptr<DumbRingSource>, std::error_code> DumbRingSource::create(
    const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format,
    std::size_t max_slots) {
  if (width == 0 || height == 0 || max_slots == 0) {
    return drm::unexpected<std::error_code>(err(std::errc::invalid_argument));
  }
  scene::SourceFormat fmt;
  fmt.drm_fourcc = drm_format;
  fmt.modifier = 0;  // dumb buffers are linear
  fmt.width = width;
  fmt.height = height;
  return std::unique_ptr<DumbRingSource>(new DumbRingSource(dev, fmt, max_slots));
}

drm::expected<void, std::error_code> DumbRingSource::ensure_slot(std::size_t slot) {
  if (slot >= buffers_.size()) {
    buffers_.resize(slot + 1);
  }
  if (buffers_[slot].has_value()) {
    return {};
  }
  drm::dumb::Config cfg;
  cfg.width = format_.width;
  cfg.height = format_.height;
  cfg.drm_format = format_.drm_fourcc;
  cfg.bpp = 32;  // packed ARGB/XRGB — the shape DumbRingSource ships
  cfg.add_fb = true;
  auto buf = drm::dumb::Buffer::create(*dev_, cfg);
  if (!buf) {
    return drm::unexpected<std::error_code>(buf.error());
  }
  buffers_[slot] = std::move(*buf);
  return {};
}

drm::expected<void, std::error_code> DumbRingSource::paint(const PaintFn& fn) {
  auto lease = ring_.acquire();
  if (!lease.has_value()) {
    return drm::unexpected<std::error_code>(err(std::errc::resource_unavailable_try_again));
  }
  if (auto r = ensure_slot(lease->slot); !r) {
    return r;
  }
  auto& slot_buf = buffers_[lease->slot];
  if (!slot_buf.has_value()) {  // ensure_slot guarantees this; satisfies the checker
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }
  drm::BufferMapping mapping = slot_buf->map(drm::MapAccess::Write);

  // App repaints the stale region + draws this frame; tells us what it changed.
  const std::vector<Rect> frame_damage = fn(mapping, lease->repaint);
  ring_.present(lease->slot, frame_damage);

  // FB_DAMAGE_CLIPS for this buffer == the region that differs from its previous
  // scanout, i.e. the stale union the app just repainted. A full repaint reports
  // no damage (the scene then does a full-frame commit).
  pending_damage_.clear();
  if (!lease->repaint.full) {
    pending_damage_.reserve(lease->repaint.region.size());
    for (const Rect& r : lease->repaint.region) {
      pending_damage_.push_back(scene::DamageRect{r.x, r.y, r.width, r.height});
    }
  }
  pending_slot_ = lease->slot;
  return {};
}

drm::expected<scene::AcquiredBuffer, std::error_code> DumbRingSource::acquire() {
  if (!pending_slot_.has_value()) {
    // Nothing painted since the last commit — skip this layer this vblank.
    return drm::unexpected<std::error_code>(err(std::errc::resource_unavailable_try_again));
  }
  const std::size_t slot = *pending_slot_;
  auto& slot_buf = buffers_[slot];
  if (!slot_buf.has_value()) {  // painted slot always exists; satisfies the checker
    pending_slot_.reset();
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }
  scene::AcquiredBuffer acq;
  acq.fb_id = slot_buf->fb_id();
  // NOLINTNEXTLINE(performance-no-int-to-ptr,cppcoreguidelines-pro-type-reinterpret-cast)
  acq.opaque = reinterpret_cast<void*>(static_cast<std::uintptr_t>(slot));
  acq.damage = pending_damage_;
  pending_slot_.reset();  // consumed: require a fresh paint() before the next commit
  return acq;
}

void DumbRingSource::release(scene::AcquiredBuffer acquired) noexcept {
  // NOLINTNEXTLINE(performance-no-int-to-ptr,cppcoreguidelines-pro-type-reinterpret-cast)
  const auto slot = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(acquired.opaque));
  ring_.release(slot);
}

void DumbRingSource::on_session_paused() noexcept {}

drm::expected<void, std::error_code> DumbRingSource::on_session_resumed(
    const drm::Device& new_dev) {
  // Old fd is dead: drop and re-allocate every live slot against the new device,
  // and abandon the in-flight painted frame (its commit was lost).
  dev_ = &new_dev;
  pending_slot_.reset();
  pending_damage_.clear();
  for (auto& slot : buffers_) {
    if (slot.has_value()) {
      slot->forget();
      drm::dumb::Config cfg;
      cfg.width = format_.width;
      cfg.height = format_.height;
      cfg.drm_format = format_.drm_fourcc;
      cfg.bpp = 32;
      cfg.add_fb = true;
      auto buf = drm::dumb::Buffer::create(new_dev, cfg);
      if (!buf) {
        return drm::unexpected<std::error_code>(buf.error());
      }
      slot = std::move(*buf);
    }
  }
  return {};
}

}  // namespace drm::present
