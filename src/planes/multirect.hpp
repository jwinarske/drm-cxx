// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Private (not installed): multirect ("smart DMA") plane pairing, and the
// blend-stage limit, both read from the downstream SDE driver's `capabilities`
// blobs (`key=value` lines).
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

/// The unsigned value of line `key=` in a `capabilities` blob's text, or nullopt
/// when the key is absent or its value is not a number.
[[nodiscard]] inline std::optional<std::uint32_t> caps_value(std::string_view caps_text,
                                                             std::string_view key) noexcept {
  std::size_t pos = 0;
  while ((pos = caps_text.find(key, pos)) != std::string_view::npos) {
    // Only a match at the start of a line, followed by '=', counts.
    const std::size_t after = pos + key.size();
    if ((pos == 0 || caps_text[pos - 1] == '\n') && after < caps_text.size() &&
        caps_text[after] == '=') {
      const auto value = caps_text.substr(after + 1);
      std::uint32_t v = 0;
      const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
      if (ec == std::errc{} && end != value.data()) {
        return v;
      }
      return std::nullopt;
    }
    pos = after;
  }
  return std::nullopt;
}

/// Parent plane id from a plane's `capabilities` blob text, or nullopt when the
/// blob does not mark the plane as a multirect rectangle (or the value is garbage).
[[nodiscard]] inline std::optional<std::uint32_t> parse_multirect_parent(
    std::string_view caps_text) noexcept {
  const auto id = caps_value(caps_text, "primary_smart_plane_id");
  return id.has_value() && *id != 0 ? id : std::nullopt;
}

/// Planes a CRTC can blend, from its `capabilities` blob text: one per mixer
/// stage. Stages run 0 to max_blendstages - 1; stage 0 is the mixer's base
/// unless use_baselayer_for_stage is set, and stages from sec_ui_blendstage up
/// are held for secure UI. nullopt when the blob has no max_blendstages.
[[nodiscard]] inline std::optional<std::uint32_t> parse_blend_limit(
    std::string_view caps_text) noexcept {
  const auto max_stages = caps_value(caps_text, "max_blendstages");
  if (!max_stages.has_value() || *max_stages == 0) {
    return std::nullopt;
  }
  std::uint32_t top = *max_stages;
  if (const auto sec_ui = caps_value(caps_text, "sec_ui_blendstage");
      sec_ui.has_value() && *sec_ui > 0 && *sec_ui < top) {
    top = *sec_ui;
  }
  const bool base_usable = caps_value(caps_text, "use_baselayer_for_stage").value_or(0) != 0;
  const std::uint32_t limit = base_usable ? top : top - 1;
  return limit > 0 ? std::optional<std::uint32_t>(limit) : std::nullopt;
}

/// True when placing on a plane whose multirect parent is `parent` is allowed
/// given the planes already used: a plain plane always, a virtual one only once
/// its parent is in use. `in_use(id)` reports whether a plane id is taken.
template <typename InUse>
[[nodiscard]] bool multirect_pairing_ok(std::optional<std::uint32_t> parent, const InUse& in_use) {
  return !parent.has_value() || in_use(*parent);
}

}  // namespace drm::planes::detail
