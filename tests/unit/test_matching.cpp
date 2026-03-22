// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "planes/matching.hpp"

TEST(BipartiteMatchingTest, EmptyGraph) {
  drm::planes::BipartiteMatching m(0, 0);
  EXPECT_EQ(m.solve(), 0u);
}

TEST(BipartiteMatchingTest, SingleEdge) {
  drm::planes::BipartiteMatching m(1, 1);
  m.add_edge(0, 0);
  EXPECT_EQ(m.solve(), 1u);
  EXPECT_EQ(m.match_for_left(0), 0u);
  EXPECT_EQ(m.match_for_right(0), 0u);
}

TEST(BipartiteMatchingTest, PerfectMatching) {
  // 3 layers, 3 planes, each layer connects to one plane
  drm::planes::BipartiteMatching m(3, 3);
  m.add_edge(0, 0);
  m.add_edge(1, 1);
  m.add_edge(2, 2);
  EXPECT_EQ(m.solve(), 3u);
}

TEST(BipartiteMatchingTest, PartialMatching) {
  // 3 layers but only 2 planes
  drm::planes::BipartiteMatching m(3, 2);
  m.add_edge(0, 0);
  m.add_edge(1, 0);
  m.add_edge(1, 1);
  m.add_edge(2, 1);
  EXPECT_EQ(m.solve(), 2u);
}

TEST(BipartiteMatchingTest, ConflictResolution) {
  // Two layers both want plane 0, but layer 1 can also use plane 1
  drm::planes::BipartiteMatching m(2, 2);
  m.add_edge(0, 0);
  m.add_edge(1, 0);
  m.add_edge(1, 1);

  EXPECT_EQ(m.solve(), 2u);

  auto m0 = m.match_for_left(0);
  auto m1 = m.match_for_left(1);
  ASSERT_TRUE(m0.has_value());
  ASSERT_TRUE(m1.has_value());
  EXPECT_NE(*m0, *m1);
}

TEST(BipartiteMatchingTest, NoEdges) {
  drm::planes::BipartiteMatching m(3, 3);
  EXPECT_EQ(m.solve(), 0u);
  EXPECT_FALSE(m.match_for_left(0).has_value());
  EXPECT_FALSE(m.match_for_right(0).has_value());
}

TEST(BipartiteMatchingTest, MatchedCountConsistency) {
  drm::planes::BipartiteMatching m(4, 3);
  m.add_edge(0, 0);
  m.add_edge(0, 1);
  m.add_edge(1, 0);
  m.add_edge(2, 1);
  m.add_edge(2, 2);
  m.add_edge(3, 2);

  auto count = m.solve();
  EXPECT_EQ(count, m.matched_count());
  EXPECT_EQ(count, 3u);
}

TEST(BipartiteMatchingTest, OutOfBoundsReturnsNullopt) {
  drm::planes::BipartiteMatching m(2, 2);
  m.add_edge(0, 0);
  m.solve();

  EXPECT_FALSE(m.match_for_left(5).has_value());
  EXPECT_FALSE(m.match_for_right(5).has_value());
}

TEST(BipartiteMatchingTest, StarGraph) {
  // One layer compatible with all planes — should get exactly one
  drm::planes::BipartiteMatching m(1, 4);
  m.add_edge(0, 0);
  m.add_edge(0, 1);
  m.add_edge(0, 2);
  m.add_edge(0, 3);
  EXPECT_EQ(m.solve(), 1u);
  ASSERT_TRUE(m.match_for_left(0).has_value());
}

TEST(BipartiteMatchingTest, CompleteGraph) {
  // All layers connect to all planes
  drm::planes::BipartiteMatching m(3, 3);
  for (std::size_t i = 0; i < 3; ++i)
    for (std::size_t j = 0; j < 3; ++j)
      m.add_edge(i, j);
  EXPECT_EQ(m.solve(), 3u);
}
