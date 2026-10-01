// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Private (not installed): multirect ("smart DMA") plane pairing.
//
// Some display controllers drive two rectangles from one hardware source pipe
// and publish the second rectangle as a separate, "virtual" DRM plane. Such a
// plane is only valid while its parent plane — the pipe's first rectangle — is
// also in use on the same commit; staged alone, atomic_check rejects it. The
// downstream SDE display driver marks these planes in the read-only
// `capabilities` blob with a `primary_smart_plane_id=<parent>` line. Drivers
// without that key are unaffected by everything here.

#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace drm::planes::detail {

/// Parent plane id from a `capabilities` blob's text, or nullopt when the blob
/// does not mark the plane as a multirect rectangle (or the value is garbage).
[[nodiscard]] inline std::optional<std::uint32_t> parse_multirect_parent(
    std::string_view caps_text) noexcept {
  constexpr std::string_view k_key = "primary_smart_plane_id=";
  std::size_t pos = 0;
  while ((pos = caps_text.find(k_key, pos)) != std::string_view::npos) {
    // Only a match at the start of a line counts (the blob is key=value lines).
    if (pos == 0 || caps_text[pos - 1] == '\n') {
      const auto value = caps_text.substr(pos + k_key.size());
      std::uint32_t id = 0;
      const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), id);
      if (ec == std::errc{} && end != value.data() && id != 0) {
        return id;
      }
      return std::nullopt;
    }
    pos += k_key.size();
  }
  return std::nullopt;
}

/// True when placing on a plane whose multirect parent is `parent` is allowed
/// given the planes already used: a plain plane always, a virtual one only once
/// its parent is in use. `in_use(id)` reports whether a plane id is taken.
template <typename InUse>
[[nodiscard]] bool multirect_pairing_ok(std::optional<std::uint32_t> parent, const InUse& in_use) {
  return !parent.has_value() || in_use(*parent);
}

}  // namespace drm::planes::detail
