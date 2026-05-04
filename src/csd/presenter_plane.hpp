// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// csd/presenter_plane.hpp — Tier 0 (one overlay plane per decoration).
//
// The Plane presenter is the simplest and highest-quality path: each
// decoration scans out from its own DRM overlay plane, no software
// composition. Per-frame work is one FB_ID + geometry write per
// active decoration (and a plane-disarm for any decoration that
// closed since the previous frame).
//
// The shell allocates the planes via `csd::OverlayReservation` once
// at startup, hands the resulting plane-id list to
// `PlanePresenter::create`, and from there per-frame presentation is
// `apply(surfaces, req)`. The presenter reads each plane's
// PlaneCapabilities (blend-mode and per-plane-alpha presence) at
// construction time so it can paint cleanly across drivers without
// re-querying every frame.

#pragma once

#include "presenter.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <cstdint>
#include <memory>
#include <system_error>
#include <vector>

namespace drm {
class Device;
}  // namespace drm

namespace drm::planes {
class PlaneRegistry;
}  // namespace drm::planes

namespace drm::csd {

/// One pre-resolved plane slot — reserved id, target CRTC, and the
/// property ids the presenter writes per frame. Exposed for the
/// `compute_writes` helper unit tests; production code never builds
/// these by hand.
struct PlaneSlot {
  std::uint32_t plane_id{0};
  std::uint32_t crtc_id{0};
  std::uint32_t fb_id_prop{0};
  std::uint32_t crtc_id_prop{0};
  std::uint32_t crtc_x_prop{0};
  std::uint32_t crtc_y_prop{0};
  std::uint32_t crtc_w_prop{0};
  std::uint32_t crtc_h_prop{0};
  std::uint32_t src_x_prop{0};
  std::uint32_t src_y_prop{0};
  std::uint32_t src_w_prop{0};
  std::uint32_t src_h_prop{0};
  /// Optional plane properties — written when non-zero. Mirror the
  /// PlaneCapabilities flags: `pixel_blend_mode` is set to the
  /// driver-defined enum value for "Pre-multiplied", `alpha` is set
  /// to 0xFFFF (full opacity) so a previous compositor's value
  /// doesn't bleed through, and `zpos` is set to the slot's resolved
  /// stacking value (slot index plus the caller's `base_zpos`). Zero
  /// means "plane doesn't expose this property or it's pinned
  /// immutable; skip the write."
  std::uint32_t blend_mode_prop{0};
  std::uint64_t blend_mode_value{0};
  std::uint32_t alpha_prop{0};
  std::uint32_t zpos_prop{0};
  std::uint64_t zpos_value{0};
};

/// One add_property call deferred for testing. Production code feeds
/// these straight into AtomicRequest; tests inspect the vector.
struct PropertyWrite {
  std::uint32_t object_id;
  std::uint32_t property_id;
  std::uint64_t value;
};

/// Pure helper: compute the property writes for one apply() pass.
/// Surface[i] lands on slots[i]; surplus slots get disarmed. Called
/// by `PlanePresenter::apply` after property-id resolution and
/// directly by the unit tests (production path goes through apply()).
///
/// Returns `errc::no_buffer_space` when surfaces.size() > slots.size().
drm::expected<std::vector<PropertyWrite>, std::error_code> compute_writes(
    drm::span<const PlaneSlot> slots, drm::span<const SurfaceRef> surfaces);

class PlanePresenter : public Presenter {
 public:
  /// Build a presenter for one CRTC. `reserved_plane_ids` must come
  /// from `OverlayReservation::reserve(...)` and is consumed in zpos
  /// order — surface[i] lands on reserved[i]. `crtc_id` is the KMS
  /// object id (the value commonly read from
  /// `drmModeRes::crtcs[crtc_index]`), not the ordinal index.
  ///
  /// `base_zpos` is the stacking value the first reserved plane
  /// receives; subsequent planes get `base_zpos + 1`, `+2`, etc. Set
  /// to `(primary_plane.zpos_max) + 1` to land the decorations
  /// directly above the primary on hardware where the primary is
  /// pinned (amdgpu pins primary at zpos=2; pass 3 there). Pass 0 to
  /// suppress the zpos writes entirely — useful only when the caller
  /// has already arranged stacking through some other channel and
  /// wants the kernel to keep whatever the planes currently report.
  /// Planes that pin zpos immutable (zpos_min == zpos_max) skip the
  /// write regardless.
  ///
  /// `registry` and `dev` must outlive the presenter. Property ids
  /// for each plane are cached at construction time; if the session
  /// is paused/resumed and the DRM fd is replaced, the caller should
  /// rebuild the presenter (the same way Renderer / LayerScene
  /// rebuild on resume).
  ///
  /// Errors:
  ///   * `errc::invalid_argument` — `crtc_id == 0` or any reserved id
  ///     is missing from `registry.all()`.
  ///   * `errc::no_such_device` — `cache_properties` failed for any
  ///     reserved plane (kernel ENODEV / EBADF — typically the fd is
  ///     stale and the caller hasn't rebuilt yet).
  static drm::expected<std::unique_ptr<PlanePresenter>, std::error_code> create(
      drm::Device& dev, const drm::planes::PlaneRegistry& registry, std::uint32_t crtc_id,
      drm::span<const std::uint32_t> reserved_plane_ids, std::uint64_t base_zpos = 0);

  PlanePresenter(const PlanePresenter&) = delete;
  PlanePresenter& operator=(const PlanePresenter&) = delete;
  PlanePresenter(PlanePresenter&&) = delete;
  PlanePresenter& operator=(PlanePresenter&&) = delete;
  ~PlanePresenter() override = default;

  [[nodiscard]] Tier tier() const noexcept override { return Tier::Plane; }

  drm::expected<void, std::error_code> apply(drm::span<const SurfaceRef> surfaces,
                                             drm::AtomicRequest& req) override;

  /// Diagnostic: the resolved plane slots. Useful for tests and for
  /// shells that want to introspect which planes ended up bound. The
  /// span is stable for the life of the presenter.
  [[nodiscard]] drm::span<const PlaneSlot> slots() const noexcept;

 private:
  PlanePresenter() = default;

  std::vector<PlaneSlot> slots_;
};

}  // namespace drm::csd