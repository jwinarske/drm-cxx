// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// csd/overlay_reservation.hpp — startup-time overlay-plane reservation
// helper for the Tier 0 (Plane) presenter.
//
// The Plane presenter binds one decoration `csd::Surface` to one DRM
// overlay plane per managed window. Picking those planes happens once
// at startup (and again on hotplug); the per-frame path is just FB_ID
// writes on the already-reserved planes. `OverlayReservation` is the
// stateful coordinator that does the picking — given a
// `planes::PlaneRegistry`, it tracks "who has what" across every CRTC
// the shell drives so that a `reserve(crtc, format, count, min_zpos)`
// call returns a stable, zpos-sorted set of plane IDs that no other
// CRTC can also claim.
//
// Reservation is a stateful operation, not a pure function, because
// shared-plane hardware (e.g. Mali Komeda) can route the same plane to
// either of two CRTCs — the helper has to remember "this plane is now
// taken by CRTC 0" so that a subsequent `reserve(crtc=1, ...)` skips
// it. On partitioned hardware (the common case: amdgpu DCN, Intel
// Tigerlake+, i.MX8 DCSS, RK3399 VOP) the bookkeeping is no-op; the
// API admits the hard case without burdening the easy case.
//
// Per-frame plane reassignment is *not* this helper's concern. The
// caller writes the returned IDs to `Allocator::apply`'s
// `external_reserved` so the per-frame allocator excludes them from
// layer assignment.
//
// See docs/csd_overlay_reservation.md for the full design rationale,
// hardware reality table, and multi-monitor scenario walk-throughs.

#pragma once

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <cstddef>
#include <cstdint>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace drm::planes {
class PlaneRegistry;
}  // namespace drm::planes

namespace drm::csd {

class OverlayReservation {
 public:
  // Build against an enumerated PlaneRegistry. The registry's
  // capability snapshot is borrowed; the registry must outlive the
  // reservation. Returns `errc::invalid_argument` only if a future
  // policy change adds preconditions — the current implementation
  // accepts any registry.
  static drm::expected<OverlayReservation, std::error_code> create(
      const drm::planes::PlaneRegistry& registry);

  OverlayReservation() = default;
  ~OverlayReservation() = default;
  OverlayReservation(OverlayReservation&&) noexcept = default;
  OverlayReservation& operator=(OverlayReservation&&) noexcept = default;
  OverlayReservation(const OverlayReservation&) = delete;
  OverlayReservation& operator=(const OverlayReservation&) = delete;

  // Reserve `count` overlay planes for `crtc_index` that
  //   * are of type OVERLAY,
  //   * support `format` (DRM FourCC, e.g. ARGB8888),
  //   * have zpos_min >= `min_zpos` (typically primary_zpos + 1),
  //   * are compatible_with_crtc(crtc_index),
  //   * are NOT already reserved for any CRTC (including this one).
  //
  // Returns the IDs in zpos-ascending order. Caller writes them to
  // `Allocator::apply`'s `external_reserved` so the per-frame
  // allocator excludes them from layer assignment.
  //
  // Errors:
  //   * `errc::invalid_argument` — moved-from / default-constructed
  //     reservation (no registry bound).
  //   * `errc::resource_unavailable_try_again` — fewer than `count`
  //     candidate planes are available; the caller should fall back
  //     to a software-composited tier for this CRTC.
  //
  // A successful reservation is committed eagerly: the returned IDs
  // are recorded as taken and excluded from subsequent `reserve`
  // calls until `release(crtc_index)` is invoked. There is no
  // "transactional" rollback if the caller rejects the result — they
  // can simply `release()` and try again with a different `count`.
  drm::expected<std::vector<std::uint32_t>, std::error_code> reserve(std::uint32_t crtc_index,
                                                                     std::uint32_t format,
                                                                     std::size_t count,
                                                                     std::uint64_t min_zpos = 0);

  // Drop the reservation for `crtc_index` (hotplug-out, mode change,
  // or shell shutting down a monitor). Idempotent. Frees the planes
  // for future `reserve` calls on any CRTC — relevant on
  // shared-plane hardware where releasing CRTC A's planes makes them
  // available to CRTC B.
  void release(std::uint32_t crtc_index) noexcept;

  // Diagnostic: planes currently reserved for `crtc_index`. Empty
  // span if none. Stable for the life of the entry (the underlying
  // vector is not reallocated until `release` clears it).
  [[nodiscard]] drm::span<const std::uint32_t> reserved_for(
      std::uint32_t crtc_index) const noexcept;

  // Diagnostic: union of all reserved IDs across every CRTC. Useful
  // when the shell drives more than one CRTC through one allocator
  // pass (rare; most shells run one allocator per CRTC).
  [[nodiscard]] std::vector<std::uint32_t> all_reserved() const;

 private:
  // by_crtc_[crtc_index] = sorted (ascending zpos) plane IDs reserved
  // for that CRTC. all_reserved_ is the union for O(1) "is this plane
  // taken?" checks during `reserve`.
  std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> by_crtc_;
  std::unordered_set<std::uint32_t> all_reserved_;
  const drm::planes::PlaneRegistry* registry_{nullptr};
};

}  // namespace drm::csd