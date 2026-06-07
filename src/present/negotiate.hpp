// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
#pragma once
// present/negotiate.hpp
//
// Pure modifier negotiation: intersect what a producer can render/export with
// what a display plane can scan out, drop entries the rotation can't satisfy,
// and rank the survivors compression-first. No DRM fd, no allocation policy --
// the atomic TEST_ONLY commit remains the ground truth (see drm::fmt). This is
// the shared step both the GL and Vulkan producers feed (GBM: the plane's
// IN_FORMATS set; Vulkan: VkDrmFormatModifierPropertiesListEXT).

#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/fmt/format_mod.hpp>

#include <cstdint>
#include <vector>

namespace drm::present {

// Intersect `producer` (modifiers the producer can render + export for a fourcc)
// with `plane` (the plane's IN_FORMATS modifiers for that fourcc), keep only
// those compatible with `rotation`, de-duplicate, and rank compression-first
// (then tiling, then linear; stable within a class, so producer preference is
// preserved). Returns the ranked candidates, most-preferred first; empty when
// the sets don't overlap. The result feeds a ScanoutBuffer/allocator whose
// TEST_ONLY commit is the real arbiter.
[[nodiscard]] std::vector<fmt::Modifier> negotiate(drm::span<const fmt::Modifier> producer,
                                                   drm::span<const fmt::Modifier> plane,
                                                   fmt::Rotation rotation = fmt::Rotation::Rotate0);

// Convenience overload: take the plane's modifiers straight from a FormatTable
// for `fourcc`. Equivalent to negotiate(producer, plane.modifiers_for(fourcc), r).
[[nodiscard]] std::vector<fmt::Modifier> negotiate(drm::span<const fmt::Modifier> producer,
                                                   const fmt::FormatTable& plane,
                                                   std::uint32_t fourcc,
                                                   fmt::Rotation rotation = fmt::Rotation::Rotate0);

}  // namespace drm::present
