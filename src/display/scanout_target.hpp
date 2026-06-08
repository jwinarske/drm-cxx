// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
#pragma once
// display/scanout_target.hpp
//
// One-call discovery of a usable scanout output: a connected connector + a
// chosen mode + a CRTC its encoders can drive + that CRTC's PRIMARY plane, plus
// the plane's IN_FORMATS when the driver exposes it. Replaces the hand-rolled
// connector/encoder/CRTC/plane walk every consumer otherwise repeats.

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/fmt/format_mod.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <xf86drmMode.h>

#include <cstdint>
#include <optional>
#include <system_error>

namespace drm {
class Device;
}

namespace drm::display {

struct ScanoutTarget {
  std::uint32_t connector_id{0};
  std::uint32_t crtc_id{0};
  std::uint32_t crtc_index{0};  // CRTC index in the resources table (possible_crtcs bit)
  std::uint32_t primary_plane_id{0};
  drmModeModeInfo mode{};
  // The primary plane's IN_FORMATS, when the driver exposes it; empty otherwise
  // (older/simple display controllers -> the caller assumes LINEAR scanout).
  std::optional<fmt::FormatTable> primary_formats;

  // Discover the first connected output: a connector with modes, the CRTC bound
  // to it (or the first its encoders allow), that CRTC's PRIMARY plane, and the
  // connector's preferred mode. Enables universal planes on `dev` so the primary
  // plane is visible. Returns std::errc::no_such_device when nothing is hooked up.
  [[nodiscard]] static drm::expected<ScanoutTarget, std::error_code> discover(
      const drm::Device& dev);
};

// The id of the PRIMARY plane usable on `crtc_index`, or nullopt. Pure helper
// over an enumerated PlaneRegistry, exposed for testing against synthetic caps.
[[nodiscard]] std::optional<std::uint32_t> primary_plane_for_crtc(
    const planes::PlaneRegistry& planes, std::uint32_t crtc_index);

}  // namespace drm::display
