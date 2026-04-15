// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Polyfill adapter for std::span.
//
// Exposes drm::span. Uses the C++20 standard library when available;
// otherwise falls back to tcb::span, a single-header C++17 backport
// with identical layout (pointer + size) and zero overhead.

#pragma once

#include <cstddef>
#include <version>

#if defined(__cpp_lib_span) && __cpp_lib_span >= 202002L
#include <span>
namespace drm {
template <class T, std::size_t Extent = std::dynamic_extent>
using span = std::span<T, Extent>;
inline constexpr std::size_t dynamic_extent = std::dynamic_extent;
}  // namespace drm
#else
#include <tcb/span.hpp>
namespace drm {
template <class T, std::size_t Extent = tcb::dynamic_extent>
using span = tcb::span<T, Extent>;
inline constexpr std::size_t dynamic_extent = tcb::dynamic_extent;
}  // namespace drm
#endif
