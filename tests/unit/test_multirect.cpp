// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "planes/multirect.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <string_view>
#include <vector>

using drm::planes::detail::multirect_pairing_ok;
using drm::planes::detail::parse_multirect_parent;

TEST(Multirect, ParsesParentFromCapabilitiesBlob) {
  constexpr std::string_view blob =
      "max_linewidth=4096\nprimary_smart_plane_id=118\nmax_upscale=20\n";
  EXPECT_EQ(parse_multirect_parent(blob), 118U);
}

TEST(Multirect, KeyAtBlobStart) {
  EXPECT_EQ(parse_multirect_parent("primary_smart_plane_id=97\n"), 97U);
}

TEST(Multirect, OrdinaryPlaneHasNoParent) {
  EXPECT_EQ(parse_multirect_parent("max_linewidth=4096\nscaler_version=2\n"), std::nullopt);
  EXPECT_EQ(parse_multirect_parent(""), std::nullopt);
}

TEST(Multirect, KeyMustStartALine) {
  EXPECT_EQ(parse_multirect_parent("not_primary_smart_plane_id=5\n"), std::nullopt);
}

TEST(Multirect, GarbageOrZeroValueIgnored) {
  EXPECT_EQ(parse_multirect_parent("primary_smart_plane_id=abc\n"), std::nullopt);
  EXPECT_EQ(parse_multirect_parent("primary_smart_plane_id=0\n"), std::nullopt);
}

TEST(Multirect, VirtualPlaneNeedsParentInUse) {
  const std::vector<std::uint32_t> used{115, 97};
  auto in_use = [&](std::uint32_t id) {
    for (const auto u : used) {
      if (u == id) {
        return true;
      }
    }
    return false;
  };
  EXPECT_TRUE(multirect_pairing_ok(std::nullopt, in_use));         // ordinary plane
  EXPECT_TRUE(multirect_pairing_ok(std::uint32_t{115}, in_use));   // parent armed
  EXPECT_FALSE(multirect_pairing_ok(std::uint32_t{118}, in_use));  // parent free
}

using drm::planes::detail::parse_blend_limit;

// SA8155P DP-4: stages 0-10, stage 0 the base, stage 10 secure UI: 9 planes.
TEST(BlendLimit, BaseAndSecureUiStagesAreNotPlanes) {
  constexpr std::string_view blob =
      "hw_version=1342177281\nmax_blendstages=11\nhas_src_split=1\nsec_ui_blendstage=10\n"
      "use_baselayer_for_stage=0\n";
  EXPECT_EQ(parse_blend_limit(blob), 9U);
}

TEST(BlendLimit, BaseLayerUsableForAStage) {
  EXPECT_EQ(parse_blend_limit("max_blendstages=11\nuse_baselayer_for_stage=1\n"), 11U);
  EXPECT_EQ(parse_blend_limit("max_blendstages=7\n"), 6U);
}

TEST(BlendLimit, AbsentOrGarbage) {
  EXPECT_FALSE(parse_blend_limit("has_src_split=1\n").has_value());
  EXPECT_FALSE(parse_blend_limit("max_blendstages=x\n").has_value());
  EXPECT_FALSE(parse_blend_limit("max_blendstages=1\n").has_value());
  // A key that only ends like ours does not count.
  EXPECT_FALSE(parse_blend_limit("not_max_blendstages=11\n").has_value());
}
