// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
// present/buffer_ring.cpp

#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/present/buffer_ring.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace drm::present {

namespace {
constexpr std::size_t k_min_slots = 1;
}  // namespace

BufferRing::BufferRing(std::size_t max_slots, std::size_t max_damage_rects)
    : max_slots_(max_slots < k_min_slots ? k_min_slots : max_slots),
      max_damage_rects_(max_damage_rects) {}

unsigned BufferRing::age(std::size_t slot) const noexcept {
  if (slot >= slots_.size() || slots_[slot].last_present == 0) {
    return 0;  // never presented -> contents undefined
  }
  return static_cast<unsigned>(present_seq_ - slots_[slot].last_present);
}

std::size_t BufferRing::in_flight() const noexcept {
  std::size_t n = 0;
  for (const Slot& s : slots_) {
    if (s.state == State::InFlight) {
      ++n;
    }
  }
  return n;
}

Repaint BufferRing::repaint_for(std::uint64_t last_present) const {
  // Never presented, or its damage history has scrolled off: repaint everything.
  if (last_present == 0 || last_present < base_seq_) {
    return Repaint{true, {}};
  }
  // Union the damage of presents (last_present, present_seq_]. Present sequence S
  // lives at damage_ index (S - base_seq_ - 1), so the first index we need is
  // (last_present + 1) - base_seq_ - 1 == last_present - base_seq_.
  Repaint out;
  out.full = false;
  for (auto i = static_cast<std::size_t>(last_present - base_seq_); i < damage_.size(); ++i) {
    for (const Rect& r : damage_[i]) {
      out.region.push_back(r);
      if (out.region.size() > max_damage_rects_) {
        return Repaint{true, {}};  // too many rects: cheaper to repaint the whole buffer
      }
    }
  }
  return out;
}

std::optional<BufferRing::Lease> BufferRing::acquire() {
  // Prefer the freshest free slot (largest last_present -> smallest repaint).
  std::optional<std::size_t> best;
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i].state != State::Free) {
      continue;
    }
    if (!best.has_value() || slots_[i].last_present > slots_[*best].last_present) {
      best = i;
    }
  }
  if (best.has_value()) {
    slots_[*best].state = State::InFlight;
    return Lease{*best, false, repaint_for(slots_[*best].last_present)};
  }
  // Grow the ring if it has not reached capacity.
  if (slots_.size() < max_slots_) {
    const std::size_t idx = slots_.size();
    slots_.push_back(Slot{State::InFlight, 0});
    return Lease{idx, true, Repaint{true, {}}};
  }
  return std::nullopt;  // every slot busy and at capacity: caller retries next vblank
}

void BufferRing::present(std::size_t slot, drm::span<const Rect> frame_damage) {
  if (slot >= slots_.size()) {
    return;
  }
  // The outgoing scanout buffer becomes releasable once its flip completes.
  for (Slot& s : slots_) {
    if (s.state == State::Scanning) {
      s.state = State::PendingRelease;
    }
  }
  ++present_seq_;
  damage_.emplace_back(frame_damage.begin(), frame_damage.end());
  // Keep only as much damage history as the deepest reusable slot can need; older
  // frames falling off just force a full repaint for any slot that stale (safe).
  while (damage_.size() > max_slots_) {
    damage_.pop_front();
    ++base_seq_;
  }
  slots_[slot].state = State::Scanning;
  slots_[slot].last_present = present_seq_;
}

void BufferRing::release(std::size_t slot) {
  if (slot >= slots_.size() || slots_[slot].state == State::Scanning) {
    return;  // never release the live scanout slot
  }
  slots_[slot].state = State::Free;
}

}  // namespace drm::present
