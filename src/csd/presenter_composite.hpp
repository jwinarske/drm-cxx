// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// csd/presenter_composite.hpp — software composite onto one plane.
//
// Where the Plane presenter (presenter_plane.hpp) arms one DRM overlay
// per decoration, the Composite presenter blends every decoration into a
// single double-buffered ARGB8888 canvas and scans that canvas out from
// one plane — normally the CRTC's PRIMARY. It's the path for
// plane-starved hardware (mid-range ARM: i.MX8 DCSS, RK3399 VOP, Mali
// Komeda) that can't give each decoration its own overlay.
//
// The heavy lifting — premultiplied SRC_OVER blend, per-frame damage
// (clear last-frame's footprint, blend this frame's, copy only the
// touched bands), double-buffering, and session pause/resume — is reused
// verbatim from `scene::CompositeCanvas`; this presenter is the adapter
// that drives that canvas from `SurfaceRef`s and writes the resulting
// FB_ID + full-screen geometry onto the canvas plane each frame.
//
// Like PlanePresenter, property ids are cached at construction time, so
// a session pause/resume that replaces the DRM fd requires rebuilding
// the presenter (and its canvas) against the fresh Device — the same
// contract the rest of the CSD stack follows.

#pragma once

#include "presenter.hpp"
#include "presenter_plane.hpp"  // PlaneSlot, PropertyWrite

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

namespace drm::scene {
class CompositeCanvas;
}  // namespace drm::scene

namespace drm::csd {

/// Pure helper: the full-screen property writes that arm framebuffer
/// `fb_id` (the canvas) onto `slot`'s plane, covering the whole CRTC at
/// `canvas_w x canvas_h`. Always emits FB_ID + CRTC_ID + the four
/// CRTC_/four SRC_ geometry writes (the canvas plane is always armed —
/// unlike a per-decoration plane it never disarms). Exposed so the unit
/// tests can check the geometry math without a live DRM device.
[[nodiscard]] std::vector<PropertyWrite> compute_canvas_writes(const PlaneSlot& slot,
                                                               std::uint32_t fb_id,
                                                               std::uint32_t canvas_w,
                                                               std::uint32_t canvas_h);

class CompositePresenter : public Presenter {
 public:
  /// Build a presenter that composites onto `canvas_plane_id` (typically
  /// the CRTC's PRIMARY) at `canvas_w x canvas_h` — normally the CRTC
  /// mode size. `crtc_id` is the KMS object id. The plane's scanout
  /// format is chosen from its advertised formats (`canvas_format_for_plane`),
  /// so minimal controllers that expose only XBGR8888 / RGB565 still work;
  /// the internal blend is always ARGB8888 and the canvas converts per row.
  ///
  /// `registry` and `dev` must outlive the presenter. As with
  /// PlanePresenter, property ids are cached now; rebuild the presenter
  /// after a session resume that swaps the DRM fd.
  ///
  /// Errors:
  ///   * `errc::invalid_argument` — `crtc_id == 0`, zero canvas
  ///     dimensions, or `canvas_plane_id` missing from `registry.all()`.
  ///   * `errc::not_supported` — the plane advertises no format the
  ///     canvas can emit.
  ///   * `errc::no_such_device` — property caching failed (stale fd).
  ///   * whatever `CompositeCanvas::create` returns on buffer-allocation
  ///     failure.
  [[nodiscard]] static drm::expected<std::unique_ptr<CompositePresenter>, std::error_code> create(
      drm::Device& dev, const drm::planes::PlaneRegistry& registry, std::uint32_t crtc_id,
      std::uint32_t canvas_plane_id, std::uint32_t canvas_w, std::uint32_t canvas_h);

  CompositePresenter(const CompositePresenter&) = delete;
  CompositePresenter& operator=(const CompositePresenter&) = delete;
  CompositePresenter(CompositePresenter&&) = delete;
  CompositePresenter& operator=(CompositePresenter&&) = delete;
  ~CompositePresenter() override;

  [[nodiscard]] Tier tier() const noexcept override { return Tier::Composite; }

  /// Composite `surfaces` (bottom-to-top in index order, matching the
  /// Plane tier's zpos assignment) into the canvas and add the writes
  /// that arm the canvas onto its plane to `req`. Vacant slots
  /// (`surface == nullptr` / `empty()`) contribute nothing — a closed
  /// decoration simply stops being blended, and `clear()` scrubs its
  /// former footprint. Unlike the Plane tier there is no plane budget,
  /// so any number of surfaces composite onto the single canvas.
  ///
  /// Errors are pre-commit: a per-surface CPU map failure, a canvas
  /// flush failure, or a property add failure. On error the caller
  /// should discard `req`.
  drm::expected<void, std::error_code> apply(drm::span<const SurfaceRef> surfaces,
                                             drm::AtomicRequest& req) override;

  /// Diagnostic: the FB id the canvas currently scans out (0 before the
  /// first apply()). Useful for tests and screenshot tooling.
  [[nodiscard]] std::uint32_t canvas_fb_id() const noexcept;

  /// Diagnostic: the DRM FourCC the canvas buffers are allocated in.
  [[nodiscard]] std::uint32_t canvas_format() const noexcept;

 private:
  CompositePresenter() = default;

  PlaneSlot slot_;
  std::uint32_t canvas_w_{0};
  std::uint32_t canvas_h_{0};
  std::unique_ptr<drm::scene::CompositeCanvas> canvas_;
};

}  // namespace drm::csd
