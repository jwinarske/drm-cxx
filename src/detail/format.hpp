// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Polyfill adapter for std::format / std::print / std::println.
//
// Exposes drm::format, drm::format_string, drm::print, drm::println.
// Uses the C++23 standard library when <print> is available; otherwise
// falls back to {fmt}, whose API matches std::format 1:1.

#pragma once

#include <version>

#if defined(__cpp_lib_print) && __cpp_lib_print >= 202207L && defined(__cpp_lib_format) && \
    __cpp_lib_format >= 201907L
#include <format>
#include <print>
namespace drm {
using std::format;
using std::print;
using std::println;
template <class... Args>
using format_string = std::format_string<Args...>;
}  // namespace drm
#else
#include <fmt/format.h>
namespace drm {
using fmt::format;
using fmt::print;
using fmt::println;
template <class... Args>
using format_string = fmt::format_string<Args...>;
}  // namespace drm
#endif
