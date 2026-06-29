// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "external_dma_buf_ring.hpp"

#include "buffer_source.hpp"
#include "external_dma_buf_source.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/fmt/format_mod.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <drm.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace drm::scene {

namespace {

constexpr std::uint64_t k_mod_invalid = (1ULL << 56U) - 1U;  // DRM_FORMAT_MOD_INVALID

[[nodiscard]] std::error_code last_errno_or(std::errc fallback) noexcept {
  const int e = errno;
  return {e != 0 ? e : static_cast<int>(fallback), std::system_category()};
}

[[nodiscard]] std::error_code err(std::errc code) noexcept {
  return std::make_error_code(code);
}

// AcquiredBuffer::opaque carries a monotonic acquisition token (full pointer
// width, so no truncation on ILP32) that identifies each acquisition. release
// fires per-acquisition keyed by token, not per-slot index, so the scene's
// triple-deferred release of an aliased slot (slot 0 retiring while slot 0 is
// live again two frames later) resolves correctly. The token→slot mapping for
// outstanding acquisitions lives in the ring (outstanding_), not packed into the
// pointer, so slot width never constrains the token.
[[nodiscard]] void* token_to_opaque(std::uintptr_t token) noexcept {
  // NOLINTNEXTLINE(performance-no-int-to-ptr,cppcoreguidelines-pro-type-reinterpret-cast)
  return reinterpret_cast<void*>(token);
}
[[nodiscard]] std::uintptr_t opaque_to_token(void* opaque) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return reinterpret_cast<std::uintptr_t>(opaque);
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
    if (desc.planes.empty() || desc.planes.size() > k_max_planes) {
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

  auto ring = std::unique_ptr<ExternalDmaBufRing>(new ExternalDmaBufRing());
  ring->fd_ = fd;
  ring->format_.drm_fourcc = drm_format;
  ring->format_.modifier = slots[0].modifier;  // slots are kept plane-compatible
  ring->format_.width = width;
  ring->format_.height = height;
  ring->on_release_ = std::move(options.on_release);
  ring->fence_deadline_ = options.fence_deadline;
  ring->slots_.reserve(slots.size());

  for (const ExternalSlotDesc& desc : slots) {
    // Build the slot in place so a partial failure is cleaned up by the
    // destructor (teardown_kernel_state + close_duped_fds over slots_).
    SlotRecord& rec = ring->slots_.emplace_back();
    rec.modifier = desc.modifier;
    for (std::size_t i = 0; i < desc.planes.size(); ++i) {
      const int duped = ::fcntl(desc.planes[i].fd, F_DUPFD_CLOEXEC, 0);
      if (duped < 0) {
        const auto ec = last_errno_or(std::errc::bad_file_descriptor);
        debug_step("fcntl F_DUPFD_CLOEXEC", ec.value());
        return drm::unexpected<std::error_code>(ec);
      }
      rec.planes.at(i).duped_fd = duped;
      rec.planes.at(i).offset = desc.planes[i].offset;
      rec.planes.at(i).pitch = desc.planes[i].pitch;
      rec.plane_count = i + 1;
    }
    if (auto r = ring->import_slot(fd, rec); !r) {
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

drm::expected<void, std::error_code> ExternalDmaBufRing::import_slot(int fd,
                                                                     SlotRecord& slot) const {
  // PRIME-import each plane's duped fd into a GEM handle. The kernel dedups
  // identical fds into the same handle, so re-importing a shared fd is correct.
  for (std::size_t i = 0; i < slot.plane_count; ++i) {
    std::uint32_t handle = 0;
    const int rc = drmPrimeFDToHandle(fd, slot.planes.at(i).duped_fd, &handle);
    if (rc != 0 || handle == 0) {
      const auto ec = last_errno_or(std::errc::io_error);
      debug_step("drmPrimeFDToHandle", ec.value());
      return drm::unexpected<std::error_code>(ec);
    }
    slot.planes.at(i).gem_handle = handle;
  }

  std::array<std::uint32_t, k_max_planes> handles{};
  std::array<std::uint32_t, k_max_planes> pitches{};
  std::array<std::uint32_t, k_max_planes> offsets{};
  std::array<std::uint64_t, k_max_planes> modifiers{};
  for (std::size_t i = 0; i < slot.plane_count; ++i) {
    const auto& rec = slot.planes.at(i);
    handles.at(i) = rec.gem_handle;
    pitches.at(i) = rec.pitch;
    offsets.at(i) = rec.offset;
    modifiers.at(i) = slot.modifier;
  }

  const bool use_modifiers = slot.modifier != k_mod_invalid;
  const int rc = drmModeAddFB2WithModifiers(fd, format_.width, format_.height, format_.drm_fourcc,
                                            handles.data(), pitches.data(), offsets.data(),
                                            use_modifiers ? modifiers.data() : nullptr, &slot.fb_id,
                                            use_modifiers ? DRM_MODE_FB_MODIFIERS : 0U);
  if (rc != 0 || slot.fb_id == 0) {
    const auto ec = last_errno_or(std::errc::io_error);
    debug_step("drmModeAddFB2WithModifiers", ec.value());
    return drm::unexpected<std::error_code>(ec);
  }
  return {};
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
  const std::scoped_lock lock(mu_);
  pending_slot_ = slot;
  pending_fence_ = std::move(acquire);
  // Replace, not union (see submit() contract). Over-cap degrades to whole-frame
  // (count 0) rather than truncating — a short list under-reports the dirty area.
  if (damage.size() > k_max_damage) {
    debug_step("submit() damage over cap — degrading to whole-frame");
    pending_damage_count_ = 0;
  } else {
    pending_damage_count_ = damage.size();
    std::copy_n(damage.begin(), damage.size(), pending_damage_.begin());
  }
}

drm::expected<AcquiredBuffer, std::error_code> ExternalDmaBufRing::acquire() {
  std::optional<std::size_t> slot;
  std::optional<drm::sync::SyncFence> fence;
  std::vector<DamageRect> damage;
  {
    const std::scoped_lock lock(mu_);
    if (pending_slot_.has_value()) {
      slot = std::exchange(pending_slot_, std::nullopt);
      fence = std::exchange(pending_fence_, std::nullopt);
      // Drain the damage store atomically with the slot. count 0 (whole-frame)
      // leaves `damage` empty. The allocation lives here, not in noexcept submit().
      if (pending_damage_count_ > 0) {
        damage.assign(pending_damage_.begin(),
                      pending_damage_.begin() + static_cast<std::ptrdiff_t>(pending_damage_count_));
        pending_damage_count_ = 0;
      }
    }
  }

  // Fault isolation. When a deadline is configured, the producer's
  // fence is CPU-pre-waited here up to the deadline and NEVER handed to the
  // kernel via IN_FENCE_FD — a never-signaling in-fence the kernel already holds
  // would wedge the whole-CRTC pipeline (the blast radius this guards). On a
  // miss we drop this not-ready frame and fall through to hold the last good
  // slot (frozen-but-alive, not blank); the producer keeps submitting and
  // whichever frame next signals in time advances (auto-recovery). With no
  // deadline the fence passes through unchanged for the scene to wire/CPU-wait.
  if (slot.has_value() && fence_deadline_.has_value() && fence.has_value()) {
    // Round a sub-millisecond deadline up to 1 ms: SyncFence::wait() polls in ms,
    // so truncating e.g. 500 us to 0 would never actually wait. A 0 ns deadline
    // (explicit "don't wait") stays 0.
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(*fence_deadline_);
    if (ms.count() == 0 && fence_deadline_->count() > 0) {
      ms = std::chrono::milliseconds(1);
    }
    const bool signaled = fence->wait(ms).has_value();
    fence.reset();  // either way, do not also wire IN_FENCE_FD (mutual exclusion)
    if (!signaled) {
      slot.reset();  // deadline missed -> hold the last good slot below
    }
  }

  if (slot.has_value()) {
    // Fresh frame: this slot becomes live under a new token; the previously-
    // scanning buffer is retired by the scene's later release() (off-screen once
    // the flip carrying this one lands).
    scanning_slot_ = slot;
    ++next_token_;
    if (next_token_ == 0) {
      ++next_token_;  // skip the reserved "no acquisition" sentinel on wrap
    }
    scanning_token_ = next_token_;
    Outstanding entry;
    entry.token = scanning_token_;
    entry.slot = *slot;
    outstanding_.push_back(entry);
    AcquiredBuffer acq;
    acq.fb_id = slots_.at(*slot).fb_id;
    acq.opaque = token_to_opaque(scanning_token_);
    acq.acquire_fence = std::move(fence);
    acq.damage = std::move(damage);  // empty = whole-frame
    return acq;
  }

  // Idle hold (and a fence-deadline miss): nothing fresh and ready — hold the
  // last good frame rather than drop the layer (which would blank the plane).
  // Re-hand the scanning slot under the SAME token (so the held buffer isn't
  // mistaken for a superseded one and released out from under the display) and
  // with no acquire fence (its pixels are valid and on screen). No new
  // outstanding entry — the live token's entry already exists.
  if (scanning_slot_.has_value() && slots_.at(*scanning_slot_).fb_id != 0) {
    AcquiredBuffer acq;
    acq.fb_id = slots_.at(*scanning_slot_).fb_id;
    acq.opaque = token_to_opaque(scanning_token_);
    return acq;
  }

  // First frame, nothing ever submitted: no buffer to contribute this vblank.
  return drm::unexpected<std::error_code>(err(std::errc::resource_unavailable_try_again));
}

void ExternalDmaBufRing::release(AcquiredBuffer acquired) noexcept {
  release_with_fence(std::move(acquired), std::nullopt);
}

void ExternalDmaBufRing::release_with_fence(
    AcquiredBuffer acquired, std::optional<drm::sync::SyncFence> release_fence) noexcept {
  const std::uintptr_t token = opaque_to_token(acquired.opaque);
  if (token == 0) {
    return;  // never-submitted sentinel
  }
  // A buffer still holding the live token (the current frame, or an idle
  // hold-last-frame re-commit) is still on screen — signaling "free" would race
  // the producer into overwriting a live scanout buffer. Fire only once the token
  // has been superseded by a newer advance.
  if (token == scanning_token_) {
    return;
  }
  // Erase-on-fire collapses the several idle re-holds that share one token down
  // to a single release, and makes the fire keyed purely by token identity (no
  // ordering/overflow assumptions). A token not present was already fired.
  for (auto it = outstanding_.begin(); it != outstanding_.end(); ++it) {
    if (it->token == token) {
      const std::size_t slot = it->slot;
      outstanding_.erase(it);
      fire_release(slot, std::move(release_fence));
      return;
    }
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

  // Abandon any in-flight handoff and live-slot bookkeeping — the commit that
  // would have consumed them was lost with the old fd.
  {
    const std::scoped_lock lock(mu_);
    pending_slot_.reset();
    pending_fence_.reset();
  }
  scanning_slot_.reset();
  // Reset release tracking: pre-resume buffers are abandoned (the scene drains
  // its deferred-release ring on pause). Clearing outstanding_ and the token
  // counters means any stale post-resume release finds no entry and no-ops.
  outstanding_.clear();
  next_token_ = 0;
  scanning_token_ = 0;

  // The old fd is dead: drop cached FB IDs + GEM handles without ioctls (they
  // were reclaimed on fd close) and re-import every slot's duped fds on new_fd.
  for (SlotRecord& slot : slots_) {
    slot.fb_id = 0;
    for (std::size_t i = 0; i < slot.plane_count; ++i) {
      slot.planes.at(i).gem_handle = 0;
    }
  }
  for (SlotRecord& slot : slots_) {
    if (auto r = import_slot(new_fd, slot); !r) {
      teardown_kernel_state();
      return drm::unexpected<std::error_code>(r.error());
    }
  }
  return {};
}

void ExternalDmaBufRing::teardown_kernel_state() noexcept {
  if (fd_ < 0) {
    return;
  }
  for (SlotRecord& slot : slots_) {
    if (slot.fb_id != 0) {
      drmModeRmFB(fd_, slot.fb_id);
      slot.fb_id = 0;
    }
    for (std::size_t i = 0; i < slot.plane_count; ++i) {
      auto& rec = slot.planes.at(i);
      if (rec.gem_handle != 0) {
        drm_gem_close gc{};
        gc.handle = rec.gem_handle;
        ::ioctl(fd_, DRM_IOCTL_GEM_CLOSE, &gc);
        rec.gem_handle = 0;
      }
    }
  }
}

void ExternalDmaBufRing::close_duped_fds() noexcept {
  for (SlotRecord& slot : slots_) {
    for (std::size_t i = 0; i < slot.plane_count; ++i) {
      auto& rec = slot.planes.at(i);
      if (rec.duped_fd >= 0) {
        ::close(rec.duped_fd);
        rec.duped_fd = -1;
      }
    }
    slot.plane_count = 0;
  }
}

}  // namespace drm::scene
