// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// external_ring_core.hpp — the presentation state machine shared by the
// externally-fed dma-buf sources (ExternalDmaBufRing and the forthcoming
// dynamic pool). It tracks which buffer is pending/scanning, hands out the
// monotonic tokens the scene's deferred-release protocol keys on, carries
// damage forward across frames it drops on a fence-deadline miss, and enforces
// the optional CPU fence pre-wait.
//
// It owns NO buffer storage. A buffer is identified only by an opaque `SlotKey`
// (the ring uses a dense slot index; a pool uses a stable per-buffer key). The
// owner (adapter) maps a key back to its cached fb_id and fires its own
// leave-scanout callback — so the same state machine drives a fixed ring and a
// lazily-populated map without knowing which it is.

#pragma once

#include "../buffer_source.hpp"  // DamageRect

#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace drm::scene::detail {

// Fixed damage store cap: submit() stays alloc-free, and an over-cap frame
// degrades to whole-frame rather than truncating (a short list would
// under-report the dirty area and corrupt the scanout).
inline constexpr std::size_t k_max_damage = 16;

// Opaque buffer identity. Fits both a dense slot index and a pointer-width
// pool key.
using SlotKey = std::uint64_t;

class RingPresenter {
 public:
  explicit RingPresenter(std::optional<std::chrono::nanoseconds> fence_deadline) noexcept
      : fence_deadline_(fence_deadline) {}

  [[nodiscard]] std::optional<std::chrono::nanoseconds> fence_deadline() const noexcept {
    return fence_deadline_;
  }

  // --- producer thread (thread-safe vs. acquire()/release()) ---

  // Stage `key` as the next frame, replacing any not-yet-acquired one.
  // `damage` is REPLACE-not-union; over-k_max_damage degrades to whole-frame.
  void submit(SlotKey key, std::optional<drm::sync::SyncFence> acquire,
              drm::span<const DamageRect> damage) noexcept;

  // True when a submitted frame is waiting to be acquired.
  [[nodiscard]] bool has_fresh_frame() const noexcept;

  // --- commit thread (single-threaded) ---

  enum class Present : std::uint8_t {
    None,   // nothing to present this vblank (adapter returns EAGAIN)
    Fresh,  // a newly-acquired frame under a new token
    Hold,   // idle-hold: re-present the scanning buffer under its live token
  };

  // The commit-thread decision. For Fresh the adapter builds an AcquiredBuffer
  // from fb_id(key) + token + fence + damage; for Hold it re-presents fb_id(key)
  // under token with no fence (validating fb_id != 0 first).
  struct Acquired {
    Present kind{Present::None};
    SlotKey key{0};
    std::uintptr_t token{0};
    std::optional<drm::sync::SyncFence> fence;  // Fresh only
    std::vector<DamageRect> damage;             // Fresh only
  };

  // Advance the state machine one vblank. Applies the fence deadline (dropping a
  // not-ready frame and carrying its damage), promotes a fresh frame under a new
  // token, or falls back to holding the scanning buffer.
  [[nodiscard]] Acquired acquire();

  // Retire the acquisition whose AcquiredBuffer carried `token`. Returns the key
  // to release (the adapter fires its own callback), or nullopt when the token
  // is still live/held or was already released.
  [[nodiscard]] std::optional<SlotKey> release(std::uintptr_t token) noexcept;

  // The key currently on screen, if any (for export_dma_buf()).
  [[nodiscard]] std::optional<SlotKey> scanning_key() const noexcept { return scanning_key_; }

  // Drop all pending/in-flight/scanning state (a session resume re-imports and
  // restarts presentation from scratch).
  void reset() noexcept;

  // AcquiredBuffer::opaque carries the token round-trip (pointer-width, so no
  // ILP32 truncation).
  [[nodiscard]] static void* token_to_opaque(std::uintptr_t token) noexcept;
  [[nodiscard]] static std::uintptr_t opaque_to_token(void* opaque) noexcept;

 private:
  void accumulate_carried_damage(const std::array<DamageRect, k_max_damage>& buf,
                                 std::size_t count) noexcept;
  void take_damage_with_carry(std::vector<DamageRect>& out,
                              const std::array<DamageRect, k_max_damage>& buf, std::size_t count);

  std::optional<std::chrono::nanoseconds> fence_deadline_;

  // Producer→commit handoff, guarded by mu_. Mutable so the const
  // has_fresh_frame() query can lock it.
  mutable std::mutex mu_;
  std::optional<SlotKey> pending_key_;
  std::optional<drm::sync::SyncFence> pending_fence_;
  std::array<DamageRect, k_max_damage> pending_damage_{};
  std::size_t pending_damage_count_{0};

  // Commit-thread only. Each fresh advance gets a new monotonic token (pointer-
  // width); idle re-holds reuse the live token. `outstanding_` maps each live
  // token to its key; release fires once per token by erasing its entry, keyed
  // by token not key so the scene's triple-deferred release of an aliased key
  // resolves correctly. Bounded by in-flight depth, so a linear scan is cheap.
  struct Outstanding {
    std::uintptr_t token{0};
    SlotKey key{0};
  };
  std::optional<SlotKey> scanning_key_;
  std::uintptr_t next_token_{0};
  std::uintptr_t scanning_token_{0};
  std::vector<Outstanding> outstanding_;

  // Damage carried across this presenter's own fence-deadline drops. A dropped
  // frame's dirty regions were never presented, so they must be unioned into the
  // next presented frame. `carried_damage_whole_` collapses to whole-frame when
  // a dropped frame was itself whole-frame or the accumulation overflows the cap.
  std::array<DamageRect, k_max_damage> carried_damage_{};
  std::size_t carried_damage_count_{0};
  bool carried_damage_whole_{false};
};

}  // namespace drm::scene::detail
