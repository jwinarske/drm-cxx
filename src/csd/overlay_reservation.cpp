// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "overlay_reservation.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::csd {

namespace {

drm::unexpected<std::error_code> err(std::errc e) {
  return drm::unexpected<std::error_code>(std::make_error_code(e));
}

}  // namespace

drm::expected<OverlayReservation, std::error_code> OverlayReservation::create(
    const drm::planes::PlaneRegistry& registry) {
  OverlayReservation out;
  out.registry_ = &registry;
  return out;
}

drm::expected<std::vector<std::uint32_t>, std::error_code> OverlayReservation::reserve(
    std::uint32_t crtc_index, std::uint32_t format, std::size_t count, std::uint64_t min_zpos) {
  if (registry_ == nullptr) {
    return err(std::errc::invalid_argument);
  }
  if (count == 0) {
    by_crtc_[crtc_index];  // ensure the empty entry exists for reserved_for()
    return std::vector<std::uint32_t>{};
  }

  // Drop any prior reservation for this CRTC so a re-reserve picks
  // fresh planes from the current free pool. Without this, a shell
  // that re-reserves on every mode change would leak planes into
  // all_reserved_ after the first call.
  release(crtc_index);

  // Gather candidates: type==OVERLAY, format-supporting, meeting the
  // zpos floor, compatible with this CRTC, not already taken.
  // Sorted ascending by zpos so the caller gets a stable order
  // matching the visual stack from bottom to top.
  std::vector<std::pair<std::uint64_t, std::uint32_t>> candidates;
  for (const auto* cap : registry_->for_crtc(crtc_index)) {
    if (cap == nullptr) {
      continue;
    }
    if (cap->type != drm::planes::DRMPlaneType::OVERLAY) {
      continue;
    }
    if (!cap->supports_format(format)) {
      continue;
    }
    // A plane with no zpos property at all can't be ordered
    // meaningfully against the primary; skip it to avoid surprising
    // the caller with an "above primary" slot they can't enforce.
    if (!cap->zpos_min.has_value()) {
      continue;
    }
    if (*cap->zpos_min < min_zpos) {
      continue;
    }
    if (all_reserved_.count(cap->id) != 0U) {
      continue;
    }
    candidates.emplace_back(*cap->zpos_min, cap->id);
  }

  if (candidates.size() < count) {
    return err(std::errc::resource_unavailable_try_again);
  }

  std::sort(candidates.begin(), candidates.end());

  std::vector<std::uint32_t> out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(candidates[i].second);
    all_reserved_.insert(candidates[i].second);
  }
  by_crtc_[crtc_index] = out;
  return out;
}

void OverlayReservation::release(std::uint32_t crtc_index) noexcept {
  auto it = by_crtc_.find(crtc_index);
  if (it == by_crtc_.end()) {
    return;
  }
  for (const std::uint32_t id : it->second) {
    all_reserved_.erase(id);
  }
  by_crtc_.erase(it);
}

drm::span<const std::uint32_t> OverlayReservation::reserved_for(
    std::uint32_t crtc_index) const noexcept {
  auto it = by_crtc_.find(crtc_index);
  if (it == by_crtc_.end()) {
    return {};
  }
  return {it->second.data(), it->second.size()};
}

std::vector<std::uint32_t> OverlayReservation::all_reserved() const {
  return {all_reserved_.begin(), all_reserved_.end()};
}

}  // namespace drm::csd