// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// csd/presenter.hpp — abstract Presenter interface for the CSD module.
//
// A Presenter takes painted decoration `csd::Surface`s and arranges
// for them to reach a CRTC. The CSD plan envisions three concrete
// implementations:
//
//   * Tier::Plane     — one DRM overlay plane per decoration. Desktop /
//                       well-provisioned ARM / virtio-gpu. Lives in
//                       `presenter_plane.cpp`.
//   * Tier::Composite — software compositor onto the primary plane,
//                       damage-tracked. Mid-range ARM (i.MX8 DCSS,
//                       RK3399 VOP, Mali Komeda). Lives in a follow-up.
//   * Tier::Fb        — `/dev/fb0` blit. Legacy / no-KMS targets.
//                       Lives in a later follow-up.
//
// The decoration renderer and shadow cache are tier-agnostic; only the
// presenter — the component that takes rendered Surfaces and gets them
// onto the output — differs. `csd::probe_presenter` (when all tiers
// have landed) selects one at startup based on plane budget.
//
// A presenter is constructed once per CRTC and owns no per-frame state
// beyond whatever's needed to track plane disarm. The shell calls
// `apply(surfaces, req)` each frame to add the relevant property
// writes to its caller-built AtomicRequest, then commits the request
// itself — page-flip-event flag, blocking flip flag, IN_FENCE_FD, and
// post-commit callback are all the caller's choice.

#pragma once

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <cstdint>
#include <system_error>

namespace drm {
class AtomicRequest;
}  // namespace drm

namespace drm::csd {

class Surface;

/// Which presentation tier this presenter implements. Selected by
/// `probe_presenter` based on hardware budget; callers can also branch
/// on it to enable per-tier policies (e.g. a heavy theme on Plane,
/// minimal theme on Fb).
enum class Tier : std::uint8_t {
  Plane,
  Composite,
  Fb,
};

/// One painted decoration ready for presentation. The presenter reads
/// `surface->fb_id()` and the surface's dimensions; it does not draw
/// or mutate the surface, so the caller is free to paint other
/// decorations in parallel.
///
/// `surface == nullptr` (or `surface->empty()`) signals "no decoration
/// at this slot this frame" — the presenter disarms the corresponding
/// reserved plane (FB_ID = 0, CRTC_ID = 0) so a closed decoration
/// vacates its plane on the next commit. Slots are matched to reserved
/// planes by index: surfaces[i] lands on reserved_planes[i].
struct SurfaceRef {
  const Surface* surface{nullptr};
  std::int32_t x{0};
  std::int32_t y{0};
};

class Presenter {
 public:
  Presenter() = default;
  Presenter(const Presenter&) = delete;
  Presenter& operator=(const Presenter&) = delete;
  Presenter(Presenter&&) = delete;
  Presenter& operator=(Presenter&&) = delete;
  virtual ~Presenter() = default;

  [[nodiscard]] virtual Tier tier() const noexcept = 0;

  /// Add the property writes that present `surfaces` to `req`. The
  /// caller commits `req` itself (TEST + COMMIT, ATOMIC_NONBLOCK,
  /// PAGE_FLIP_EVENT, IN_FENCE_FD — all the caller's call).
  ///
  /// Errors are pre-commit only: surfaces.size() exceeding the
  /// presenter's plane budget, an internal property-id lookup
  /// missing, or a default-constructed presenter. A successful
  /// return guarantees the request now contains every property write
  /// needed to scan out the surfaces; partial writes on failure are
  /// possible and the caller should discard the request.
  virtual drm::expected<void, std::error_code> apply(drm::span<const SurfaceRef> surfaces,
                                                     drm::AtomicRequest& req) = 0;
};

}  // namespace drm::csd