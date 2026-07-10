// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "probe_presenter.hpp"

#include "../scene/composite_canvas.hpp"
#include "overlay_reservation.hpp"
#include "presenter.hpp"
#include "presenter_composite.hpp"
#include "presenter_plane.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <drm_fourcc.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::csd {

namespace {

drm::unexpected<std::error_code> err(std::errc e) {
  return drm::unexpected<std::error_code>(std::make_error_code(e));
}

// The id of a PRIMARY plane on `crtc_index` that can host a composition
// canvas (advertises a format `CompositeCanvas` can emit), or 0 if none.
std::uint32_t canvas_plane_for(const drm::planes::PlaneRegistry& registry,
                               std::uint32_t crtc_index) {
  for (const auto* cap : registry.for_crtc(crtc_index)) {
    if (cap != nullptr && cap->type == drm::planes::DRMPlaneType::PRIMARY &&
        drm::scene::canvas_format_for_plane(*cap).has_value()) {
      return cap->id;
    }
  }
  return 0;
}

}  // namespace

std::optional<Tier> choose_presenter_tier(std::size_t reservable, std::size_t desired,
                                          bool has_canvas_plane) {
  if (desired > 0 && reservable >= desired) {
    return Tier::Plane;  // every window gets its own overlay
  }
  if (has_canvas_plane) {
    return Tier::Composite;  // plane-starved: composite onto the primary
  }
  return std::nullopt;  // no usable KMS plane — caller drops to fbdev
}

drm::expected<ProbedPresenter, std::error_code> probe_presenter(
    drm::Device& dev, const drm::planes::PlaneRegistry& registry, const ProbeConfig& cfg) {
  if (cfg.crtc_id == 0U || cfg.desired_decorations == 0U) {
    return err(std::errc::invalid_argument);
  }

  const std::uint32_t canvas_plane = canvas_plane_for(registry, cfg.crtc_index);

  // Try to reserve one overlay per decoration, all-or-nothing. A short
  // reservation returns resource_unavailable; treat that as zero reserved
  // (the reservation object is dropped, releasing any partial claim).
  auto reservation = drm::csd::OverlayReservation::create(registry);
  if (!reservation) {
    return drm::unexpected<std::error_code>(reservation.error());
  }
  std::vector<std::uint32_t> reserved;
  if (auto r = reservation->reserve(cfg.crtc_index, DRM_FORMAT_ARGB8888, cfg.desired_decorations,
                                    cfg.plane_base_zpos)) {
    reserved = std::move(*r);
  }

  const auto tier =
      choose_presenter_tier(reserved.size(), cfg.desired_decorations, canvas_plane != 0U);
  if (!tier) {
    return err(std::errc::not_supported);
  }

  ProbedPresenter out;
  if (*tier == Tier::Plane) {
    auto p = PlanePresenter::create(
        dev, registry, cfg.crtc_id,
        drm::span<const std::uint32_t>(reserved.data(), reserved.size()), cfg.plane_base_zpos);
    if (!p) {
      return drm::unexpected<std::error_code>(p.error());
    }
    out.presenter = std::move(*p);
    out.reservation = std::move(*reservation);  // keep the leases alive
    out.plane_count = reserved.size();
    return out;
  }

  auto p = CompositePresenter::create(dev, registry, cfg.crtc_id, canvas_plane, cfg.canvas_width,
                                      cfg.canvas_height);
  if (!p) {
    return drm::unexpected<std::error_code>(p.error());
  }
  out.presenter = std::move(*p);
  out.plane_count = 0;
  return out;
}

}  // namespace drm::csd
