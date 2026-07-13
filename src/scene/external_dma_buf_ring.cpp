// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "external_dma_buf_ring.hpp"

#include "buffer_source.hpp"
#include "detail/dmabuf_slot.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/fmt/format_mod.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::scene {

namespace {

[[nodiscard]] std::error_code err(std::errc code) noexcept {
  return std::make_error_code(code);
}

bool ring_debug() {
  static const bool enabled = std::getenv("DRM_EXT_DMABUF_DEBUG") != nullptr;
  return enabled;
}

void debug_step(const char* step, int saved_errno = 0) {
  if (!ring_debug()) {
    return;
  }
  if (saved_errno != 0) {
    std::fprintf(stderr, "[drm-cxx] ExternalDmaBufRing: %s (errno=%d: %s)\n", step, saved_errno,
                 std::strerror(saved_errno));
  } else {
    std::fprintf(stderr, "[drm-cxx] ExternalDmaBufRing: %s\n", step);
  }
}

}  // namespace

drm::expected<std::unique_ptr<ExternalDmaBufRing>, std::error_code> ExternalDmaBufRing::create(
    const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format,
    drm::span<const ExternalSlotDesc> slots, Options options) {
  if (width == 0 || height == 0 || drm_format == 0 || slots.empty()) {
    debug_step("validate args (width/height/drm_format non-zero, >=1 slot)");
    return drm::unexpected<std::error_code>(err(std::errc::invalid_argument));
  }

  // Structural + modifier validation first (no device needed) so callers get a
  // clean rejection before any fd is touched — mirrors ExternalDmaBufSource.
  for (const ExternalSlotDesc& desc : slots) {
    if (desc.planes.empty() || desc.planes.size() > detail::k_max_planes) {
      debug_step("validate slot plane count");
      return drm::unexpected<std::error_code>(err(std::errc::invalid_argument));
    }
    for (const ExternalPlaneInfo& p : desc.planes) {
      if (p.fd < 0 || p.pitch == 0) {
        debug_step("validate slot plane fields (fd >= 0, pitch != 0)");
        return drm::unexpected<std::error_code>(err(std::errc::invalid_argument));
      }
    }
    // Validate-not-negotiate: a modifier the assigned plane can't scan out is
    // rejected up front (errc::not_supported) so the producer can re-negotiate
    // via candidate_modifiers / negotiate() rather than fail at AddFB2 or commit.
    if (options.validate_against != nullptr &&
        !options.validate_against->supports(drm_format, fmt::Modifier{desc.modifier})) {
      debug_step("slot modifier not in plane IN_FORMATS");
      return drm::unexpected<std::error_code>(err(std::errc::not_supported));
    }
  }

  const int fd = dev.fd();
  if (fd < 0) {
    debug_step("validate device fd");
    return drm::unexpected<std::error_code>(err(std::errc::bad_file_descriptor));
  }

  auto ring = std::unique_ptr<ExternalDmaBufRing>(new ExternalDmaBufRing(options.fence_deadline));
  ring->fd_ = fd;
  ring->format_.drm_fourcc = drm_format;
  ring->format_.modifier = slots[0].modifier;  // slots are kept plane-compatible
  ring->format_.width = width;
  ring->format_.height = height;
  ring->on_release_ = std::move(options.on_release);
  ring->slots_.reserve(slots.size());

  for (const ExternalSlotDesc& desc : slots) {
    // Build the slot in place so a partial failure is cleaned up by the
    // destructor (teardown_kernel_state + close_duped_fds over slots_).
    SlotRecord& rec = ring->slots_.emplace_back();
    rec.modifier = desc.modifier;
    if (auto r = detail::dup_planes(rec, desc.planes); !r) {
      return drm::unexpected<std::error_code>(r.error());
    }
    if (auto r = detail::import_slot(fd, rec, ring->format_); !r) {
      return drm::unexpected<std::error_code>(r.error());
    }
  }

  return ring;
}

drm::expected<std::unique_ptr<ExternalDmaBufRing>, std::error_code> ExternalDmaBufRing::create(
    const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format,
    drm::span<const ExternalSlotDesc> slots) {
  return create(dev, width, height, drm_format, slots, Options{});
}

ExternalDmaBufRing::~ExternalDmaBufRing() {
  teardown_kernel_state();
  close_duped_fds();
  // No on_release on teardown: the callback is a per-slot leave-scanout signal,
  // not a destruction signal — the producer owns its buffers and is going away.
}

void ExternalDmaBufRing::submit(std::size_t slot, std::optional<drm::sync::SyncFence> acquire,
                                drm::span<const DamageRect> damage) noexcept {
  if (slot >= slots_.size()) {
    debug_step("submit() slot out of range — ignored");
    return;
  }
  presenter_.submit(slot, std::move(acquire), damage);
}

drm::expected<AcquiredBuffer, std::error_code> ExternalDmaBufRing::acquire() {
  // The presenter runs the state machine (fence deadline, token advance, idle
  // hold, damage carry) over slot-index keys; the ring resolves the chosen key
  // to its cached fb_id.
  detail::RingPresenter::Acquired d = presenter_.acquire();
  if (d.kind == detail::RingPresenter::Present::Fresh) {
    AcquiredBuffer acq;
    acq.fb_id = slots_.at(d.key).fb_id;
    acq.opaque = detail::RingPresenter::token_to_opaque(d.token);
    acq.acquire_fence = std::move(d.fence);
    acq.damage = std::move(d.damage);
    return acq;
  }
  // Idle hold: re-present the scanning slot under its live token, provided it
  // still resolves to a live FB (a paused-then-torn-down slot has fb_id == 0).
  if (d.kind == detail::RingPresenter::Present::Hold && slots_.at(d.key).fb_id != 0) {
    AcquiredBuffer acq;
    acq.fb_id = slots_.at(d.key).fb_id;
    acq.opaque = detail::RingPresenter::token_to_opaque(d.token);
    return acq;
  }

  // Nothing to contribute this vblank (first frame, or a held slot with no FB).
  return drm::unexpected<std::error_code>(err(std::errc::resource_unavailable_try_again));
}

void ExternalDmaBufRing::release(AcquiredBuffer acquired) noexcept {
  release_with_fence(std::move(acquired), std::nullopt);
}

void ExternalDmaBufRing::release_with_fence(
    AcquiredBuffer acquired, std::optional<drm::sync::SyncFence> release_fence) noexcept {
  // The presenter decides whether this acquisition's token may retire (never
  // while it is still the live/held frame) and maps it back to its slot key; the
  // ring fires its per-slot leave-scanout callback.
  if (auto key = presenter_.release(detail::RingPresenter::opaque_to_token(acquired.opaque)); key) {
    fire_release(static_cast<std::size_t>(*key), std::move(release_fence));
  }
}

void ExternalDmaBufRing::fire_release(std::size_t slot,
                                      std::optional<drm::sync::SyncFence> release_fence) noexcept {
  if (on_release_) {
    // Release fence: when the scene supplies it, `release_fence` is the OUT_FENCE of
    // the commit that displaced this slot — water waits on it GPU-side before
    // re-rendering. nullopt (CEF / OUT_FENCE-less CRTCs) means the callback edge
    // itself is the "slot free" signal.
    on_release_(slot, std::move(release_fence));
  }
}

void ExternalDmaBufRing::on_session_paused() noexcept {
  // No outstanding ioctls; the scene's commit path is quiesced while paused.
}

drm::expected<void, std::error_code> ExternalDmaBufRing::on_session_resumed(
    const drm::Device& new_dev) {
  const int new_fd = new_dev.fd();
  if (new_fd < 0) {
    return drm::unexpected<std::error_code>(err(std::errc::bad_file_descriptor));
  }
  fd_ = new_fd;

  // Abandon all pending/in-flight/scanning presentation state and damage carried
  // from the dead fd's session — the commit that would have consumed them was
  // lost with the old fd, and any stale post-resume release finds no entry.
  presenter_.reset();

  // The old fd is dead: drop cached FB IDs + GEM handles without ioctls (they
  // were reclaimed on fd close) and re-import every slot's duped fds on new_fd.
  for (SlotRecord& slot : slots_) {
    slot.fb_id = 0;
    for (std::size_t i = 0; i < slot.plane_count; ++i) {
      slot.planes.at(i).gem_handle = 0;
    }
  }
  for (SlotRecord& slot : slots_) {
    if (auto r = detail::import_slot(new_fd, slot, format_); !r) {
      teardown_kernel_state();
      return drm::unexpected<std::error_code>(r.error());
    }
  }
  return {};
}

void ExternalDmaBufRing::teardown_kernel_state() noexcept {
  for (SlotRecord& slot : slots_) {
    detail::teardown_slot(fd_, slot);
  }
}

void ExternalDmaBufRing::close_duped_fds() noexcept {
  for (SlotRecord& slot : slots_) {
    detail::close_slot_fds(slot);
  }
}

}  // namespace drm::scene
