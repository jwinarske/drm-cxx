// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::examples::rank_pick — the pure ranking step
// behind pick_connector(). The IO wrapper that loads connectors and
// applies the eligibility filter is exercised implicitly when
// open_and_pick_output() runs from an example; tests here cover the
// rank logic without needing a DRM fd.

#include "common/select_connector.hpp"

#include <drm-cxx/detail/span.hpp>

#include <drm_mode.h>

#include <array>
#include <cstdint>
#include <gtest/gtest.h>

using drm::examples::k_external_rank;
using drm::examples::k_internal_rank;
using drm::examples::k_main_rank;
using drm::examples::rank_pick;

TEST(RankPick, ChoosesEdpOverHdmiUnderMainRank) {
  // Docked laptop case: HDMI enumerated first, eDP second. k_main_rank
  // ranks eDP higher, so the eDP slot wins.
  constexpr std::array<std::uint32_t, 2> types = {DRM_MODE_CONNECTOR_HDMIA, DRM_MODE_CONNECTOR_eDP};
  const auto idx =
      rank_pick(drm::span<const std::uint32_t>(types), drm::span<const std::uint32_t>(k_main_rank));
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 1U);
}

TEST(RankPick, FallsBackToHdmiWhenNoInternal) {
  // Headless server case: only HDMI connected. k_main_rank still picks
  // it — internal panels just aren't available to be preferred.
  constexpr std::array<std::uint32_t, 2> types = {DRM_MODE_CONNECTOR_VGA, DRM_MODE_CONNECTOR_HDMIA};
  const auto idx =
      rank_pick(drm::span<const std::uint32_t>(types), drm::span<const std::uint32_t>(k_main_rank));
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 1U);  // HDMI ranks above VGA
}

TEST(RankPick, InternalRankSkipsExternalConnectors) {
  // Embedded app pinned to internal panels: HDMI present but
  // ignored, DSI picked.
  constexpr std::array<std::uint32_t, 2> types = {DRM_MODE_CONNECTOR_HDMIA, DRM_MODE_CONNECTOR_DSI};
  const auto idx = rank_pick(drm::span<const std::uint32_t>(types),
                             drm::span<const std::uint32_t>(k_internal_rank));
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 1U);
}

TEST(RankPick, InternalRankReturnsNullWhenOnlyExternalAvailable) {
  // Same embedded app on a system with no internal panel: rank_pick
  // returns nullopt rather than falling back. Caller decides whether
  // that's an error or whether to retry with k_main_rank.
  constexpr std::array<std::uint32_t, 2> types = {DRM_MODE_CONNECTOR_HDMIA,
                                                  DRM_MODE_CONNECTOR_DisplayPort};
  const auto idx = rank_pick(drm::span<const std::uint32_t>(types),
                             drm::span<const std::uint32_t>(k_internal_rank));
  EXPECT_FALSE(idx.has_value());
}

TEST(RankPick, ExternalRankSkipsEmbeddedPanels) {
  // Signage kiosk with both HDMI and a debug eDP attached: external
  // wins, eDP is invisible to the ranker.
  constexpr std::array<std::uint32_t, 2> types = {DRM_MODE_CONNECTOR_eDP, DRM_MODE_CONNECTOR_HDMIA};
  const auto idx = rank_pick(drm::span<const std::uint32_t>(types),
                             drm::span<const std::uint32_t>(k_external_rank));
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 1U);
}

TEST(RankPick, TieGoesToEarliestCandidate) {
  // Two HDMI outputs on a discrete GPU. Same type → kernel
  // enumeration order is the tiebreaker, returning the earlier slot.
  constexpr std::array<std::uint32_t, 2> types = {DRM_MODE_CONNECTOR_HDMIA,
                                                  DRM_MODE_CONNECTOR_HDMIA};
  const auto idx =
      rank_pick(drm::span<const std::uint32_t>(types), drm::span<const std::uint32_t>(k_main_rank));
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 0U);
}

TEST(RankPick, EmptyCandidatesReturnsNullopt) {
  const auto idx =
      rank_pick(drm::span<const std::uint32_t>{}, drm::span<const std::uint32_t>(k_main_rank));
  EXPECT_FALSE(idx.has_value());
}

TEST(RankPick, EmptyRanksReturnsNullopt) {
  // No rank order at all → no preference; rank_pick refuses to
  // arbitrarily pick. Callers wanting "any connector" should pass
  // a single-element rank with that type or use k_main_rank.
  constexpr std::array<std::uint32_t, 1> types = {DRM_MODE_CONNECTOR_HDMIA};
  const auto idx =
      rank_pick(drm::span<const std::uint32_t>(types), drm::span<const std::uint32_t>{});
  EXPECT_FALSE(idx.has_value());
}

TEST(RankPick, UnknownConnectorTypesAreIgnored) {
  // Future or vendor-specific connector_type values that don't appear
  // in our rank arrays: filtered out, the next eligible candidate
  // wins. Defaults to nullopt if none of the candidates match.
  constexpr std::uint32_t k_future_type = 0xFEU;
  constexpr std::array<std::uint32_t, 2> types = {k_future_type, DRM_MODE_CONNECTOR_HDMIA};
  const auto idx =
      rank_pick(drm::span<const std::uint32_t>(types), drm::span<const std::uint32_t>(k_main_rank));
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 1U);
}

TEST(RankPick, CallerProvidedCustomRankWorks) {
  // Custom policy: "VGA preferred above all else" (contrived, but the
  // generic API has to honor it). rank_pick walks ranks in order.
  constexpr std::array<std::uint32_t, 2> custom = {DRM_MODE_CONNECTOR_VGA,
                                                   DRM_MODE_CONNECTOR_HDMIA};
  constexpr std::array<std::uint32_t, 2> types = {DRM_MODE_CONNECTOR_HDMIA, DRM_MODE_CONNECTOR_VGA};
  const auto idx =
      rank_pick(drm::span<const std::uint32_t>(types), drm::span<const std::uint32_t>(custom));
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 1U);
}
