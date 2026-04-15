// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Polyfill adapter for std::expected.
//
// Exposes drm::expected and drm::unexpected. Uses the C++23 standard
// library when available; otherwise falls back to tl::expected, which
// has the same ABI/codegen characteristics as std::expected.

#pragma once

#include <version>

// 202202L is the initial C++23 std::expected value (GCC 13 / libstdc++).
// 202211L added monadic operations, which this codebase does not use.
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#include <expected>
namespace drm {
template <class T, class E>
using expected = std::expected<T, E>;
template <class E>
using unexpected = std::unexpected<E>;
}  // namespace drm
#else
#include <tl/expected.hpp>
namespace drm {
template <class T, class E>
using expected = tl::expected<T, E>;
template <class E>
using unexpected = tl::unexpected<E>;
}  // namespace drm
#endif
