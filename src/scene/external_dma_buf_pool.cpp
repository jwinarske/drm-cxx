// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "external_dma_buf_pool.hpp"

#include "buffer_source.hpp"
#include "detail/dmabuf_slot.hpp"
#include "detail/external_ring_core.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace drm::scene {

namespace {

[[nodiscard]] std::error_code err(std::errc code) noexcept {
  return std::make_error_code(code);
}

bool pool_debug() noexcept {
  static const bool enabled = std::getenv("DRM_EXT_DMABUF_DEBUG") != nullptr;
  return enabled;
}

void debug_step(const char* step) noexcept {
  if (pool_debug()) {
    std::fprintf(stderr, "[drm-cxx] ExternalDmaBufPool: %s\n", step);
  }
}

}  // namespace

drm::expected<std::unique_ptr<ExternalDmaBufPool>, std::error_code> ExternalDmaBufPool::create(
    const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_fourcc,
    std::uint64_t modifier, Options options) {
  if (width == 0 || height == 0 || drm_fourcc == 0) {
    debug_step("create: zero width/height/fourcc");
    return drm::unexpected<std::error_code>(err(std::errc::invalid_argument));
  }
  const int fd = dev.fd();
  if (fd < 0) {
    debug_step("create: bad device fd");
    return drm::unexpected<std::error_code>(err(std::errc::bad_file_descriptor));
  }
  auto pool = std::unique_ptr<ExternalDmaBufPool>(new ExternalDmaBufPool(options.fence_deadline));
  pool->fd_ = fd;
  pool->format_.drm_fourcc = drm_fourcc;
  pool->format_.modifier = modifier;
  pool->format_.width = width;
  pool->format_.height = height;
  pool->on_release_ = std::move(options.on_release);
  return pool;
}

ExternalDmaBufPool::~ExternalDmaBufPool() {
  // Drop every buffer's FB + GEM handles and close its duped fds. No on_release
  // on teardown: the callback is a per-buffer leave-scanout signal, not a
  // destruction signal — the producer owns its buffers and is going away.
  for (auto& [key, slot] : slots_) {
    detail::teardown_slot(fd_, slot);
    detail::close_slot_fds(slot);
  }
}

void ExternalDmaBufPool::submit(std::uintptr_t buffer_key,
                                drm::span<const ExternalPlaneInfo> planes,
                                std::optional<drm::sync::SyncFence> acquire,
                                drm::span<const DamageRect> damage) noexcept {
  {
    const std::scoped_lock lock(slots_mu_);
    if (slots_.find(buffer_key) == slots_.end()) {
      // First sight of this key: validate + import its planes, caching the fb_id.
      if (planes.empty() || planes.size() > detail::k_max_planes) {
        debug_step("submit: bad plane count — frame skipped");
        return;
      }
      for (const auto& p : planes) {
        if (p.fd < 0 || p.pitch == 0) {
          debug_step("submit: bad plane fields — frame skipped");
          return;
        }
      }
      detail::DmaBufSlot slot;
      slot.modifier = format_.modifier;
      if (auto r = detail::dup_planes(slot, planes); !r) {
        detail::close_slot_fds(slot);
        debug_step("submit: dup_planes failed — holding last frame");
        return;
      }
      if (auto r = detail::import_slot(fd_, slot, format_); !r) {
        detail::teardown_slot(fd_, slot);
        detail::close_slot_fds(slot);
        debug_step("submit: import failed — holding last frame");
        return;
      }
      slots_.emplace(buffer_key, slot);
    }
  }
  presenter_.submit(static_cast<detail::SlotKey>(buffer_key), std::move(acquire), damage);
}

std::size_t ExternalDmaBufPool::cached_count() const noexcept {
  const std::scoped_lock lock(slots_mu_);
  return slots_.size();
}

drm::expected<AcquiredBuffer, std::error_code> ExternalDmaBufPool::acquire() {
  // The presenter runs the state machine over buffer-key keys; the pool resolves
  // the chosen key to its cached fb_id under the import-map lock.
  detail::RingPresenter::Acquired d = presenter_.acquire();
  std::uint32_t fb_id = 0;
  if (d.kind != detail::RingPresenter::Present::None) {
    const std::scoped_lock lock(slots_mu_);
    if (auto it = slots_.find(static_cast<std::uintptr_t>(d.key)); it != slots_.end()) {
      fb_id = it->second.fb_id;
    }
  }
  if (d.kind == detail::RingPresenter::Present::Fresh) {
    AcquiredBuffer acq;
    acq.fb_id = fb_id;
    acq.opaque = detail::RingPresenter::token_to_opaque(d.token);
    acq.acquire_fence = std::move(d.fence);
    acq.damage = std::move(d.damage);
    return acq;
  }
  // Idle hold: re-present the scanning buffer under its live token, provided it
  // still resolves to a live FB (an evicted/torn-down buffer has fb_id == 0).
  if (d.kind == detail::RingPresenter::Present::Hold && fb_id != 0) {
    AcquiredBuffer acq;
    acq.fb_id = fb_id;
    acq.opaque = detail::RingPresenter::token_to_opaque(d.token);
    return acq;
  }
  return drm::unexpected<std::error_code>(err(std::errc::resource_unavailable_try_again));
}

void ExternalDmaBufPool::release(AcquiredBuffer acquired) noexcept {
  release_with_fence(std::move(acquired), std::nullopt);
}

void ExternalDmaBufPool::release_with_fence(
    AcquiredBuffer acquired, std::optional<drm::sync::SyncFence> release_fence) noexcept {
  if (auto key = presenter_.release(detail::RingPresenter::opaque_to_token(acquired.opaque)); key) {
    fire_release(static_cast<std::uintptr_t>(*key), std::move(release_fence));
  }
}

void ExternalDmaBufPool::fire_release(std::uintptr_t buffer_key,
                                      std::optional<drm::sync::SyncFence> release_fence) noexcept {
  if (on_release_) {
    on_release_(buffer_key, std::move(release_fence));
  }
}

drm::expected<DmaBufDesc, std::error_code> ExternalDmaBufPool::export_dma_buf() {
  const auto scanning = presenter_.scanning_key();
  if (!scanning.has_value()) {
    return drm::unexpected<std::error_code>(err(std::errc::function_not_supported));
  }
  const std::scoped_lock lock(slots_mu_);
  const auto it = slots_.find(static_cast<std::uintptr_t>(*scanning));
  if (it == slots_.end() || it->second.plane_count == 0) {
    return drm::unexpected<std::error_code>(err(std::errc::function_not_supported));
  }
  const detail::DmaBufSlot& s = it->second;
  DmaBufDesc d;
  d.n_planes = static_cast<std::uint32_t>(s.plane_count);
  d.drm_fourcc = format_.drm_fourcc;
  d.modifier = s.modifier;
  d.width = format_.width;
  d.height = format_.height;
  for (std::size_t i = 0; i < s.plane_count; ++i) {
    d.fds.at(i) = s.planes.at(i).duped_fd;
    d.offsets.at(i) = s.planes.at(i).offset;
    d.pitches.at(i) = s.planes.at(i).pitch;
  }
  return d;
}

drm::expected<void, std::error_code> ExternalDmaBufPool::on_session_resumed(
    const drm::Device& new_dev) {
  const int new_fd = new_dev.fd();
  if (new_fd < 0) {
    return drm::unexpected<std::error_code>(err(std::errc::bad_file_descriptor));
  }
  fd_ = new_fd;
  // Abandon all pending/in-flight/scanning presentation state (the commit that
  // would have consumed it was lost with the old fd).
  presenter_.reset();

  // The old fd is dead: its FB IDs and GEM handles were reclaimed on fd close.
  // Re-import each cached buffer's duped fds on the new device. A buffer that
  // fails to re-import is dropped (its fds closed) so it re-imports on its next
  // submit() rather than lingering with a dead fb_id.
  const std::scoped_lock lock(slots_mu_);
  std::vector<std::uintptr_t> dropped;
  for (auto& [key, slot] : slots_) {
    slot.fb_id = 0;
    for (std::size_t i = 0; i < slot.plane_count; ++i) {
      slot.planes.at(i).gem_handle = 0;
    }
    if (auto r = detail::import_slot(new_fd, slot, format_); !r) {
      detail::teardown_slot(new_fd, slot);
      detail::close_slot_fds(slot);
      dropped.push_back(key);
    }
  }
  for (const std::uintptr_t key : dropped) {
    slots_.erase(key);
  }
  return {};
}

}  // namespace drm::scene
