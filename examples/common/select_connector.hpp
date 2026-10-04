// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// select_connector.hpp — connector selection by preference rank.
//
// "First connected connector with an attached encoder" is wrong on
// every multi-output machine. A docked laptop with both eDP and HDMI
// connected, or an embedded SoC with both DSI and HDMI, will surface
// the wrong panel half the time depending on enumeration order. Most
// applications want a policy like "prefer the internal panel; fall
// back to the first cable-out display" — which is tedious enough to
// write per-app that everyone gets it slightly wrong.
//
// This helper ships three pre-baked rank arrays that cover the common
// policies, plus a pure `rank_pick()` that takes a caller-provided
// rank order so consumers can compose their own. Pattern after
// bsdrm's `connector_main_rank` / `connector_internal_rank` /
// `connector_external_rank` helpers.
//
//   - k_main_rank      — internal panels first, then cable-out, with
//                      VGA last as a legacy fallback. Right default
//                      for "open whichever display is the user's
//                      primary," which matches single-output kiosks
//                      and most laptop scenarios.
//   - k_internal_rank  — internal panels only (eDP > LVDS > DSI > DPI).
//                      For embedded apps that should never grab an
//                      external monitor even if one is plugged in.
//   - k_external_rank  — cable-out connectors only (HDMI > DP > DVI > VGA).
//                      For signage / digital-out kiosks that should
//                      ignore an embedded panel if both are present.
//
// The pure ranking step is in `rank_pick(types, ranks) -> optional<size_t>`
// so unit tests don't need a real DRM fd. The IO wrapper
// `pick_connector(fd, ids, ranks)` filters connectors to eligible
// (CONNECTED + has modes) and dispatches to rank_pick.

#pragma once

#include <drm-cxx/core/resources.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/display/mode_list.hpp>

#include <drm_mode.h>
#include <xf86drmMode.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace drm::examples {

/// Default rank: internal panels first, then cable-out, VGA, and finally
/// virtual and USB displays. Right for single-output kiosks and most
/// laptops; matches the behavior consumers usually want from "give me a
/// display." The last two keep machines whose only output is a virtual one
/// (vkms, virtio-gpu in a VM) or a USB display (gud) usable, while any
/// physical display still wins. Writeback connectors are never ranked.
inline constexpr std::array<std::uint32_t, 14> k_main_rank = {
    DRM_MODE_CONNECTOR_eDP,          // 14 — modern laptop panel
    DRM_MODE_CONNECTOR_LVDS,         // 7  — older laptop panel
    DRM_MODE_CONNECTOR_DSI,          // 16 — embedded MIPI panel
    DRM_MODE_CONNECTOR_DPI,          // 17 — embedded parallel RGB
    DRM_MODE_CONNECTOR_SPI,          // 19 — embedded SPI panel (small TFTs)
    DRM_MODE_CONNECTOR_HDMIA,        // 11 — primary HDMI flavor
    DRM_MODE_CONNECTOR_HDMIB,        // 12 — historical HDMI dual-link
    DRM_MODE_CONNECTOR_DisplayPort,  // 10
    DRM_MODE_CONNECTOR_DVID,         // 3  — digital DVI
    DRM_MODE_CONNECTOR_DVII,         // 2  — combined DVI
    DRM_MODE_CONNECTOR_DVIA,         // 4  — analog DVI
    DRM_MODE_CONNECTOR_VGA,          // 1  — legacy analog
    DRM_MODE_CONNECTOR_VIRTUAL,      // 15 — vkms, virtio-gpu
    DRM_MODE_CONNECTOR_USB,          // 20 — USB displays (gud)
};

/// Internal panels only — for embedded apps that must ignore any
/// external monitor even when plugged in (instrument cluster on a
/// docked dev board, etc).
inline constexpr std::array<std::uint32_t, 5> k_internal_rank = {
    DRM_MODE_CONNECTOR_eDP, DRM_MODE_CONNECTOR_LVDS, DRM_MODE_CONNECTOR_DSI,
    DRM_MODE_CONNECTOR_DPI, DRM_MODE_CONNECTOR_SPI,
};

/// Cable-out connectors only — for signage / kiosks that should land
/// on the external display even when an embedded panel is present.
inline constexpr std::array<std::uint32_t, 7> k_external_rank = {
    DRM_MODE_CONNECTOR_HDMIA, DRM_MODE_CONNECTOR_HDMIB, DRM_MODE_CONNECTOR_DisplayPort,
    DRM_MODE_CONNECTOR_DVID,  DRM_MODE_CONNECTOR_DVII,  DRM_MODE_CONNECTOR_DVIA,
    DRM_MODE_CONNECTOR_VGA,
};

/// Pure ranking step. Walks `ranks` in order; for each rank entry,
/// returns the index of the first candidate in `candidate_types`
/// matching it. Connectors whose type doesn't appear in `ranks` are
/// skipped. Returns nullopt when no candidate matches any rank entry.
///
/// Stable: ties (multiple candidates with the same type, e.g. two HDMI
/// outputs) resolve to the earliest position in `candidate_types`, so
/// the kernel's enumeration order acts as a deterministic tiebreaker.
[[nodiscard]] inline std::optional<std::size_t> rank_pick(
    drm::span<const std::uint32_t> candidate_types, drm::span<const std::uint32_t> ranks) {
  for (const auto wanted : ranks) {
    for (std::size_t i = 0; i < candidate_types.size(); ++i) {
      if (candidate_types[i] == wanted) {
        return i;
      }
    }
  }
  return std::nullopt;
}

/// `DRM_CXX_CONNECTOR=<name>` (the kernel's "DP-4" / "HDMI-A-1" form) pins the
/// output, overriding the rank. Needed where the rank lands on a connected but
/// unlit output — shared-display controllers publish one connected connector
/// per consumer, and only one of them is on screen. Returns the connector id, or
/// nullopt when unset or not found (warns on not found).
[[nodiscard]] inline std::optional<std::uint32_t> connector_override(
    int fd, drm::span<const std::uint32_t> connector_ids) {
  const char* want = std::getenv("DRM_CXX_CONNECTOR");
  if (want == nullptr || *want == '\0') {
    return std::nullopt;
  }
  for (const auto cid : connector_ids) {
    if (auto c = drm::get_connector(fd, cid); c) {
      const std::string name = std::string(drm::display::connector_type_name(c->connector_type)) +
                               "-" + std::to_string(c->connector_type_id);
      if (name == want) {
        return cid;
      }
    }
  }
  std::fprintf(stderr, "DRM_CXX_CONNECTOR=%s: no such connector; using the default pick\n", want);
  return std::nullopt;
}

/// IO wrapper: read each connector in `connector_ids`, keep those that
/// are CONNECTED with at least one mode, and
/// pick the highest-ranking by `ranks`. Returns the loaded
/// `drm::Connector` (an owning unique_ptr); a default-constructed /
/// null connector means "no eligible connector matched any rank."
///
/// `ranks` defaults to `k_main_rank`, so callers that don't care about
/// the policy choice get the sensible "internal first, then external"
/// behavior automatically.
[[nodiscard]] inline drm::Connector pick_connector(
    int fd, drm::span<const std::uint32_t> connector_ids,
    drm::span<const std::uint32_t> ranks = k_main_rank) {
  // Two parallel arrays: the loaded Connector smart pointers and a
  // flat type vector that rank_pick consumes. Loading happens once;
  // rank_pick is pure data.
  if (const auto pinned = connector_override(fd, connector_ids); pinned.has_value()) {
    if (auto c = drm::get_connector(fd, *pinned);
        c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
      return c;
    }
  }
  std::vector<drm::Connector> eligible;
  std::vector<std::uint32_t> types;
  eligible.reserve(connector_ids.size());
  types.reserve(connector_ids.size());

  for (const auto cid : connector_ids) {
    auto c = drm::get_connector(fd, cid);
    // Connected with at least one mode is the real requirement. A bound
    // encoder is NOT required: on cold KMS (no compositor has driven this
    // connector yet, or one exited) encoder_id is 0, but the connector is
    // still perfectly usable — the caller resolves a CRTC from the
    // connector's possible encoders. Requiring a live binding here made
    // every example fail on a headless / freshly-booted board with a
    // monitor attached (e.g. StarFive after stopping gdm).
    if (!c || c->connection != DRM_MODE_CONNECTED || c->count_modes == 0) {
      continue;
    }
    types.push_back(c->connector_type);
    eligible.push_back(std::move(c));
  }

  if (const auto idx = rank_pick(drm::span<const std::uint32_t>(types.data(), types.size()), ranks);
      idx.has_value()) {
    return std::move(eligible[*idx]);
  }
  return drm::Connector{nullptr, &drmModeFreeConnector};
}

}  // namespace drm::examples
