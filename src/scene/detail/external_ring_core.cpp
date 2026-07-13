// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "external_ring_core.hpp"

#include "../buffer_source.hpp"  // DamageRect

#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace drm::scene::detail {

void* RingPresenter::token_to_opaque(std::uintptr_t token) noexcept {
  // NOLINTNEXTLINE(performance-no-int-to-ptr,cppcoreguidelines-pro-type-reinterpret-cast)
  return reinterpret_cast<void*>(token);
}

std::uintptr_t RingPresenter::opaque_to_token(void* opaque) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return reinterpret_cast<std::uintptr_t>(opaque);
}

void RingPresenter::submit(SlotKey key, std::optional<drm::sync::SyncFence> acquire,
                           drm::span<const DamageRect> damage) noexcept {
  const std::scoped_lock lock(mu_);
  pending_key_ = key;
  pending_fence_ = std::move(acquire);
  // Replace, not union. Over-cap degrades to whole-frame (count 0) rather than
  // truncating — a short list under-reports the dirty area.
  if (damage.size() > k_max_damage) {
    pending_damage_count_ = 0;
  } else {
    pending_damage_count_ = damage.size();
    std::copy_n(damage.begin(), damage.size(), pending_damage_.begin());
  }
}

bool RingPresenter::has_fresh_frame() const noexcept {
  const std::scoped_lock lock(mu_);
  return pending_key_.has_value();
}

RingPresenter::Acquired RingPresenter::acquire() {
  std::optional<SlotKey> key;
  std::optional<drm::sync::SyncFence> fence;
  // Drain the damage store into a stack buffer under the lock (a cheap POD copy,
  // no allocation) so the producer-contended critical section stays alloc-free,
  // matching submit()'s noexcept fixed-store design.
  std::array<DamageRect, k_max_damage> damage_buf{};
  std::size_t damage_count = 0;
  {
    const std::scoped_lock lock(mu_);
    if (pending_key_.has_value()) {
      key = std::exchange(pending_key_, std::nullopt);
      fence = std::exchange(pending_fence_, std::nullopt);
      // Drain atomically with the key. count 0 (whole-frame) leaves it empty.
      damage_count = std::exchange(pending_damage_count_, 0);
      std::copy_n(pending_damage_.begin(), damage_count, damage_buf.begin());
    }
  }

  // Fault isolation. When a deadline is configured, the producer's fence is
  // CPU-pre-waited here up to the deadline and NEVER handed to the kernel via
  // IN_FENCE_FD — a never-signaling in-fence the kernel already holds would
  // wedge the whole-CRTC pipeline. On a miss we drop this not-ready frame and
  // fall through to hold the last good buffer (frozen-but-alive, not blank); the
  // producer keeps submitting and whichever frame next signals in time advances.
  // With no deadline the fence passes through unchanged for the scene to wire.
  if (key.has_value() && fence_deadline_.has_value() && fence.has_value()) {
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
      // Deadline missed: drop this not-ready frame and hold the last good buffer
      // below. Its dirty regions were never presented, so carry them into the
      // next presented frame — here the presenter is the dropper, so
      // accumulation is our job (unlike the producer-side replace in submit()).
      accumulate_carried_damage(damage_buf, damage_count);
      key.reset();
    }
  }

  Acquired out;
  if (key.has_value()) {
    // Fresh frame: this key becomes live under a new token; the previously-
    // scanning buffer is retired by the scene's later release() (off-screen once
    // the flip carrying this one lands).
    scanning_key_ = key;
    ++next_token_;
    if (next_token_ == 0) {
      ++next_token_;  // skip the reserved "no acquisition" sentinel on wrap
    }
    scanning_token_ = next_token_;
    outstanding_.push_back(Outstanding{scanning_token_, *key});
    out.kind = Present::Fresh;
    out.key = *key;
    out.token = scanning_token_;
    out.fence = std::move(fence);
    // Fold in any damage carried from frames dropped on a deadline miss
    // (empty -> whole-frame).
    take_damage_with_carry(out.damage, damage_buf, damage_count);
    return out;
  }

  // Idle hold (and a fence-deadline miss): nothing fresh and ready — hold the
  // last good frame rather than drop the layer (which would blank the plane).
  // Re-hand the scanning key under the SAME token (so the held buffer isn't
  // mistaken for a superseded one and released out from under the display) and
  // with no acquire fence. No new outstanding entry — the live token's exists.
  // The adapter validates the key still resolves to a live fb_id before use.
  if (scanning_key_.has_value()) {
    out.kind = Present::Hold;
    out.key = *scanning_key_;
    out.token = scanning_token_;
    return out;
  }

  // First frame, nothing ever submitted: no buffer to contribute this vblank.
  out.kind = Present::None;
  return out;
}

std::optional<SlotKey> RingPresenter::release(std::uintptr_t token) noexcept {
  if (token == 0) {
    return std::nullopt;  // never-submitted sentinel
  }
  // A buffer still holding the live token (the current frame, or an idle
  // hold-last-frame re-commit) is still on screen — signaling "free" would race
  // the producer into overwriting a live scanout buffer. Fire only once the
  // token has been superseded by a newer advance.
  if (token == scanning_token_) {
    return std::nullopt;
  }
  // Erase-on-fire collapses the several idle re-holds that share one token down
  // to a single release, and keys the fire purely by token identity (no
  // ordering/overflow assumptions). A token not present was already fired.
  for (auto it = outstanding_.begin(); it != outstanding_.end(); ++it) {
    if (it->token == token) {
      const SlotKey key = it->key;
      outstanding_.erase(it);
      return key;
    }
  }
  return std::nullopt;
}

bool RingPresenter::is_referenced(SlotKey key) const noexcept {
  if (scanning_key_.has_value() && *scanning_key_ == key) {
    return true;
  }
  return std::any_of(outstanding_.begin(), outstanding_.end(),
                     [key](const Outstanding& o) { return o.key == key; });
}

void RingPresenter::reset() noexcept {
  const std::scoped_lock lock(mu_);
  pending_key_.reset();
  pending_fence_.reset();
  pending_damage_count_ = 0;
  scanning_key_.reset();
  next_token_ = 0;
  scanning_token_ = 0;
  outstanding_.clear();
  carried_damage_count_ = 0;
  carried_damage_whole_ = false;
}

void RingPresenter::accumulate_carried_damage(const std::array<DamageRect, k_max_damage>& buf,
                                              std::size_t count) noexcept {
  if (carried_damage_whole_) {
    return;  // already whole-frame; nothing finer to track
  }
  // A whole-frame drop (count 0) forces the carry to whole-frame: we can't know
  // which regions changed, so the next presented frame must repaint everything.
  // An accumulation past the cap collapses the same way (never truncate).
  if (count == 0 || carried_damage_count_ + count > k_max_damage) {
    carried_damage_whole_ = true;
    carried_damage_count_ = 0;
    return;
  }
  std::copy_n(buf.begin(), count,
              carried_damage_.begin() + static_cast<std::ptrdiff_t>(carried_damage_count_));
  carried_damage_count_ += count;
}

void RingPresenter::take_damage_with_carry(std::vector<DamageRect>& out,
                                           const std::array<DamageRect, k_max_damage>& buf,
                                           std::size_t count) {
  // Whole-frame if either side is whole-frame (a carried whole-frame, or this
  // frame's own count 0), or if the union would overflow the cap. Otherwise
  // concatenate carried (older) ++ this frame's rects; order is irrelevant to
  // FB_DAMAGE_CLIPS, an unordered set of dirty regions.
  const bool whole =
      carried_damage_whole_ || count == 0 || carried_damage_count_ + count > k_max_damage;
  if (!whole) {
    out.reserve(carried_damage_count_ + count);
    out.assign(carried_damage_.begin(),
               carried_damage_.begin() + static_cast<std::ptrdiff_t>(carried_damage_count_));
    out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(count));
  }
  carried_damage_count_ = 0;
  carried_damage_whole_ = false;
}

}  // namespace drm::scene::detail
