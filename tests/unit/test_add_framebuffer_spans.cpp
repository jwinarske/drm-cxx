// SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Device::add_framebuffer takes its four AddFB2 plane arrays as fixed-extent
// spans. AddFB2 reads exactly four entries from each, and the previous
// `const std::uint32_t[4]` parameters decayed to pointers -- the extent was a
// comment the compiler discarded, so passing a shorter array compiled and the
// kernel read past it.
//
// These are the conversions that property rests on, asserted rather than
// assumed: the ergonomic ones must still work, and a bare pointer must not.
// Calling the function needs a DRM device; the signature's contract does not,
// so it is checked here at compile time.

#include "gtest/gtest.h"

#include <drm-cxx/detail/span.hpp>

#include <array>
#include <cstdint>
#include <type_traits>

namespace {

using Handles = drm::span<const std::uint32_t, 4>;
using Modifiers = drm::span<const std::uint64_t, 4>;

// What a caller naturally writes.
static_assert(std::is_constructible_v<Handles, std::uint32_t (&)[4]>,
              "a plain uint32_t[4] must still convert");
static_assert(std::is_constructible_v<Handles, std::array<std::uint32_t, 4>&>,
              "std::array<uint32_t, 4> must convert");
static_assert(std::is_constructible_v<Handles, const std::array<std::uint32_t, 4>&>,
              "a const std::array must convert");
static_assert(std::is_constructible_v<Modifiers, std::array<std::uint64_t, 4>&>,
              "the modifiers array is u64 and must convert the same way");

// What the change exists to reject.
static_assert(!std::is_constructible_v<Handles, const std::uint32_t*>,
              "a bare pointer must not convert: its length is unknown, which is "
              "the bug this signature prevents");
static_assert(!std::is_constructible_v<Handles, std::uint32_t (&)[3]>,
              "a short array must not convert: AddFB2 reads four entries");

TEST(AddFramebufferSpans, FixedExtentPreservesTheCallSite) {
  // The compile-time assertions above are the test. This case exists so the
  // translation unit is exercised by ctest, and to show the call-site form.
  const std::uint32_t handles[4] = {1, 0, 0, 0};
  const Handles as_span(handles);
  EXPECT_EQ(as_span.size(), 4u);
  EXPECT_EQ(as_span[0], 1u);
}

}  // namespace
