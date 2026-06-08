// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
#pragma once
// present/buffer_ring.hpp
//
// Producer-agnostic scanout slot pool implementing the buffer-age + damage
// accumulation contract for partial repaint (EGL_EXT_buffer_age style). The
// ring owns slot lifecycle (free / in-flight / scanning / pending-release) and,
// per slot, how stale its contents are and which region the producer must
// repaint to make it a correct frame again. The producer owns the actual
// buffers, indexed by slot.
//
// Not thread-safe: drive acquire()/present()/release() from one thread (the
// present loop), the same place the producer renders.

#include <drm-cxx/detail/span.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace drm::present {

// A damage rectangle in buffer pixels.
struct Rect {
  std::int32_t x{0};
  std::int32_t y{0};
  std::uint32_t width{0};
  std::uint32_t height{0};

  friend bool operator==(const Rect& a, const Rect& b) noexcept {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
  }
  friend bool operator!=(const Rect& a, const Rect& b) noexcept { return !(a == b); }
};

// What a reused slot must repaint before its contents are a correct frame.
struct Repaint {
  bool full{true};           // repaint the whole buffer (slot undefined or too stale to track)
  std::vector<Rect> region;  // when !full: union of damage since the slot was last presented
};

class BufferRing {
 public:
  // `max_slots` caps how deep the ring grows (>=1). `max_damage_rects` bounds the
  // accumulated union; a union exceeding it collapses to a full repaint so the
  // per-frame rect set stays small (a documented bandwidth-vs-bookkeeping knob).
  explicit BufferRing(std::size_t max_slots = 3, std::size_t max_damage_rects = 16);

  struct Lease {
    std::size_t slot{0};  // index into the producer's buffer array
    bool fresh{false};    // newly grown slot: the producer must allocate its buffer
    Repaint repaint;      // region the producer must repaint (full for fresh/stale slots)
  };

  // Pick a slot for the next frame: the freshest free slot (least repaint), or a
  // newly grown one up to max_slots. Never returns the scanning slot. nullopt
  // when every slot is busy and the ring is at capacity (retry next vblank).
  [[nodiscard]] std::optional<Lease> acquire();

  // Commit `slot` as the new scanout buffer; `frame_damage` is what changed in
  // this frame versus the previous one. The previously-scanning slot becomes
  // releasable and the present sequence advances.
  void present(std::size_t slot, drm::span<const Rect> frame_damage);

  // Return a replaced slot to the free pool once its replacing flip completed
  // (or its render was abandoned). Safe to call on any non-scanning slot.
  void release(std::size_t slot);

  // Presents committed since `slot` was last presented (its staleness). 0 means
  // the slot has never been presented -- its contents are undefined.
  [[nodiscard]] unsigned age(std::size_t slot) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }
  [[nodiscard]] std::size_t max_slots() const noexcept { return max_slots_; }
  [[nodiscard]] std::size_t in_flight() const noexcept;

 private:
  enum class State : std::uint8_t { Free, InFlight, Scanning, PendingRelease };
  struct Slot {
    State state{State::Free};
    std::uint64_t last_present{0};  // present seq when last presented; 0 = never presented
  };

  [[nodiscard]] Repaint repaint_for(std::uint64_t last_present) const;

  std::vector<Slot> slots_;
  // Recent per-frame damage. damage_.front() is the damage of present sequence
  // (base_seq_ + 1); damage_.back() that of present_seq_.
  std::deque<std::vector<Rect>> damage_;
  std::uint64_t present_seq_{0};  // last committed present sequence (0 = none yet)
  std::uint64_t base_seq_{0};     // present_seq just before damage_.front()
  std::size_t max_slots_;
  std::size_t max_damage_rects_;
};

}  // namespace drm::present
