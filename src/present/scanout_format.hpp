// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// scanout_format.hpp — pick a plane-supported scanout format.
//
// Display controllers don't all advertise XRGB8888: TI tilcdc on the BeagleBone
// Black, for example, exposes only RGB565 / XBGR8888 / BGR888. A CPU producer
// (dumb buffer, software renderer) that hardcodes XRGB8888 is rejected at commit
// there. negotiate_scanout_format() returns the first format from a preference
// list that the CRTC's primary plane can actually scan out, so the producer can
// render in a hardware-accepted format. It's also the query a higher-level
// integration (e.g. a Skia software renderer that can target either XRGB8888 or
// RGB565) uses to decide which surface format to render.

#pragma once

#include <drm-cxx/detail/span.hpp>

#include <cstdint>

namespace drm {
class Device;
}  // namespace drm

namespace drm::present {

/// Return the first format in `preferred` that the primary plane on `crtc_id`
/// can scan out, or 0 if none match (or the planes can't be queried). With an
/// empty `preferred`, a default packed-format order is used: XRGB8888, then
/// XBGR8888 / ARGB8888 / ABGR8888, then RGB565 / BGR565. Callers that can only
/// render certain formats should pass their own renderable subset (e.g.
/// {XRGB8888, RGB565}) so the result is always something they can produce.
[[nodiscard]] std::uint32_t negotiate_scanout_format(const drm::Device& dev, std::uint32_t crtc_id,
                                                     drm::span<const std::uint32_t> preferred = {});

}  // namespace drm::present
