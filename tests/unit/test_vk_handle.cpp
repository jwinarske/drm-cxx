// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// vk_handle: a non-dispatchable handle from its 64-bit value, as a pointer
// (64-bit ABIs) or a uint64_t (32-bit ABIs).

#include <drm-cxx/present/vk_scanout_producer.hpp>

#include <cstdint>
#include <gtest/gtest.h>

#if DRM_CXX_HAS_VK_SCANOUT_PRODUCER

namespace {
struct Image_T;
using PointerHandle = Image_T*;
using IntegerHandle = std::uint64_t;
}  // namespace

TEST(VkHandle, IntegerHandleKeepsAllBits) {
  constexpr std::uint64_t k_bits = 0x8000'0001'2345'6789ULL;
  EXPECT_EQ(drm::present::vk_handle<IntegerHandle>(k_bits), k_bits);
}

TEST(VkHandle, PointerHandleRoundTrips) {
  int object = 0;
  auto* const ptr = reinterpret_cast<PointerHandle>(&object);
  const auto bits = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ptr));
  EXPECT_EQ(drm::present::vk_handle<PointerHandle>(bits), ptr);
  EXPECT_EQ(drm::present::vk_handle<PointerHandle>(0), nullptr);
}

#else

TEST(VkHandle, SkippedWithoutVulkan) {
  GTEST_SKIP() << "built without Vulkan";
}

#endif
