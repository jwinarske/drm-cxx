// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace drm::planes {

// Hopcroft-Karp maximum cardinality bipartite matching.
//
// Models the plane allocation as a bipartite graph:
//   Left nodes  = layers (indices 0..n_left-1)
//   Right nodes = planes (indices 0..n_right-1)
//   Edges       = static compatibility (layer can potentially use plane)
//
// Returns a matching: for each left node, the matched right node (or nullopt).
// Runs in O(E * sqrt(V)) — microseconds for typical counts (<=8 each).
class BipartiteMatching {
 public:
  BipartiteMatching(std::size_t n_left, std::size_t n_right);

  // Add an edge: left node u can be matched to right node v.
  void add_edge(std::size_t u, std::size_t v);

  // Add an edge with a score for weighted matching preference.
  void add_edge(std::size_t u, std::size_t v, int score);

  // Compute maximum cardinality matching (Hopcroft-Karp).
  // Returns the number of matched pairs.
  std::size_t solve();

  // After solve(), get the right node matched to left node u (or nullopt).
  [[nodiscard]] std::optional<std::size_t> match_for_left(std::size_t u) const;

  // After solve(), get the left node matched to right node v (or nullopt).
  [[nodiscard]] std::optional<std::size_t> match_for_right(std::size_t v) const;

  // Total number of matched pairs after solve().
  [[nodiscard]] std::size_t matched_count() const noexcept;

 private:
  bool bfs();
  bool dfs(std::size_t u);

  std::size_t n_left_;
  std::size_t n_right_;

  // Adjacency list: adj_[u] = list of (right_node, score) pairs.
  // Sorted by score descending in solve() so DFS visits high-score
  // right nodes first — tie-breaks Hopcroft-Karp's otherwise-arbitrary
  // choice toward the caller's preferred plane for each layer.
  std::vector<std::vector<std::pair<std::size_t, int>>> adj_;

  // Matching state
  static constexpr std::size_t nil = static_cast<std::size_t>(-1);
  std::vector<std::size_t> match_left_;   // left -> right
  std::vector<std::size_t> match_right_;  // right -> left
  std::vector<std::size_t> dist_;         // BFS distance for left nodes
  std::size_t matched_{0};
};

}  // namespace drm::planes
