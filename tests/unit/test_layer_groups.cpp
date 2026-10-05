// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// The allocator's spatial split (planes/layer_groups.hpp): which items share a
// group, and the order the groups are placed in.

#include "planes/layer_groups.hpp"

#include <gtest/gtest.h>
#include <set>
#include <utility>
#include <vector>

using drm::planes::detail::independent_groups;

namespace {

// Items are ints; `edges` lists the overlapping pairs.
auto overlaps(std::set<std::pair<int, int>> edges) {
  return [edges = std::move(edges)](int a, int b) {
    return edges.count({a, b}) != 0 || edges.count({b, a}) != 0;
  };
}

const auto flat_priority = [](int /*item*/) { return 0; };

}  // namespace

// Transitive overlap joins a group; members keep input order.
TEST(LayerGroups, OverlapChainsFormOneGroup) {
  const std::vector<int> items{0, 1, 2, 3};
  const auto groups = independent_groups(items, overlaps({{0, 2}, {2, 3}}), flat_priority);
  ASSERT_EQ(groups.size(), 2U);
  EXPECT_EQ(groups[0], (std::vector<int>{0, 2, 3}));
  EXPECT_EQ(groups[1], (std::vector<int>{1}));
}

// Equal priority: groups in order of their first member, every time.
TEST(LayerGroups, TiesKeepInputOrder) {
  const std::vector<int> items{5, 4, 3, 2, 1, 0};
  const auto groups = independent_groups(items, overlaps({{4, 0}}), flat_priority);
  ASSERT_EQ(groups.size(), 5U);
  EXPECT_EQ(groups[0], (std::vector<int>{5}));
  EXPECT_EQ(groups[1], (std::vector<int>{4, 0}));
  EXPECT_EQ(groups[2], (std::vector<int>{3}));
  EXPECT_EQ(groups[3], (std::vector<int>{2}));
  EXPECT_EQ(groups[4], (std::vector<int>{1}));
}

// A later group holding the highest-priority member goes first: it gets first
// pick of the shared plane pool.
TEST(LayerGroups, HighestPriorityGroupFirst) {
  const std::vector<int> items{0, 1, 2, 3};
  // 3 is the Video layer; 1 and 3 overlap.
  auto priority = [](int item) { return item == 3 ? 100 : 10; };
  const auto groups = independent_groups(items, overlaps({{1, 3}}), priority);
  ASSERT_EQ(groups.size(), 3U);
  EXPECT_EQ(groups[0], (std::vector<int>{1, 3}));
  EXPECT_EQ(groups[1], (std::vector<int>{0}));
  EXPECT_EQ(groups[2], (std::vector<int>{2}));
}

TEST(LayerGroups, EmptyInput) {
  const std::vector<int> items;
  EXPECT_TRUE(independent_groups(items, overlaps({}), flat_priority).empty());
}
