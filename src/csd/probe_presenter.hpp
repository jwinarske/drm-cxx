// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// csd/probe_presenter.hpp — startup selection between the KMS presenters.
//
// A shell that doesn't want to hard-code which presenter to use calls
// `probe_presenter` once at startup: given a DRM device, its plane
// registry, and how many decorations it wants to show, it picks the best
// presenter for the hardware and returns it ready to drive.
//
// It selects between the two *KMS* presenters:
//   * PlanePresenter    — chosen when the CRTC can reserve one overlay
//                         per decoration (every window gets its own plane).
//   * CompositePresenter — chosen when it can't, so the decorations are
//                         software-composited onto the primary instead
//                         (no per-window plane limit — every window shows).
//
// The `/dev/fb0` FramebufferPresenter is deliberately NOT a candidate
// here: it needs a *non-master* device (a DRM master suspends the kernel
// fbcon it blits into), the opposite of what the plane/composite path
// needs. fbdev is the fallback a caller reaches for when it has no usable
// KMS session at all, not a plane-budget decision — so it stays an
// explicit caller choice, built with `FramebufferPresenter::create`.
//
// PlanePresenter borrows its planes from an `OverlayReservation` that
// must outlive it, so the result carries that reservation back to the
// caller to hold (empty for the composite pick).

#pragma once

#include "overlay_reservation.hpp"
#include "presenter.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>

namespace drm {
class Device;
}  // namespace drm

namespace drm::planes {
class PlaneRegistry;
}  // namespace drm::planes

namespace drm::csd {

/// What `probe_presenter` needs to build whichever presenter it picks.
struct ProbeConfig {
  std::uint32_t crtc_id{0};            ///< KMS CRTC object id (for the presenters).
  std::uint32_t crtc_index{0};         ///< Ordinal CRTC index (for reservation).
  std::size_t desired_decorations{1};  ///< Windows the shell wants to show.
  std::uint32_t canvas_width{0};       ///< Composite canvas size (CRTC mode).
  std::uint32_t canvas_height{0};
  std::uint64_t plane_base_zpos{0};  ///< First decoration plane's zpos.
  /// Optional desktop backdrop for the Composite pick — forwarded to
  /// `CompositePresenter::create` (ARGB8888, `canvas_width*canvas_height*4`;
  /// empty leaves it black). Ignored for the Plane pick.
  drm::span<const std::uint8_t> background_argb;
};

/// The chosen presenter plus the state the caller must keep alive.
struct ProbedPresenter {
  std::unique_ptr<Presenter> presenter;
  /// Held only for the Plane pick — the leases the PlanePresenter writes
  /// to. Empty (`nullopt`) for Composite. Must outlive `presenter`.
  std::optional<OverlayReservation> reservation;
  /// Decorations the pick can host on distinct planes: `desired` for
  /// Plane, 0 for Composite (the canvas has no per-window limit).
  std::size_t plane_count{0};
};

/// Pure policy: with `reservable` overlays actually available, `desired`
/// windows wanted, and whether a canvas-hostable plane exists, which tier
/// wins. Plane only when it can host *all* `desired` (every window gets a
/// plane); else Composite when a canvas plane exists; else nullopt (no
/// usable KMS plane — the caller drops to fbdev). Exposed for unit tests.
[[nodiscard]] std::optional<Tier> choose_presenter_tier(std::size_t reservable, std::size_t desired,
                                                        bool has_canvas_plane);

/// Pick and build a KMS presenter for `cfg` on `dev`. Reserves `desired`
/// overlays all-or-nothing; on success builds a PlanePresenter (and
/// returns the reservation to hold), otherwise a CompositePresenter on
/// the CRTC's primary plane.
///
/// Errors:
///   * `errc::invalid_argument` — `crtc_id == 0` or `desired == 0`.
///   * `errc::not_supported` — overlays are short AND no primary plane can
///     host a composition canvas; the caller should fall back to fbdev.
///   * whatever the chosen presenter's `create` / reservation returns.
[[nodiscard]] drm::expected<ProbedPresenter, std::error_code> probe_presenter(
    drm::Device& dev, const drm::planes::PlaneRegistry& registry, const ProbeConfig& cfg);

}  // namespace drm::csd
