// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "matching.hpp"

#include <limits>
#include <queue>

namespace drm::planes {

BipartiteMatching::BipartiteMatching(std::size_t n_left, std::size_t n_right)
    : n_left_(n_left),
      n_right_(n_right),
      adj_(n_left),
      match_left_(n_left, NIL),
      match_right_(n_right, NIL),
      dist_(n_left + 1) {}

void BipartiteMatching::add_edge(std::size_t u, std::size_t v) {
  adj_[u].push_back(v);
}

void BipartiteMatching::add_edge(std::size_t u, std::size_t v, [[maybe_unused]] int score) {
  // Score is used for ordering in the adjacency list.
  // For now we just add the edge; sorting happens before solve if needed.
  adj_[u].push_back(v);
}

bool BipartiteMatching::bfs() {
  std::queue<std::size_t> queue;

  for (std::size_t u = 0; u < n_left_; ++u) {
    if (match_left_[u] == NIL) {
      dist_[u] = 0;
      queue.push(u);
    } else {
      dist_[u] = std::numeric_limits<std::size_t>::max();
    }
  }

  // dist_[n_left_] is the sentinel distance for "free right node reached"
  dist_[n_left_] = std::numeric_limits<std::size_t>::max();

  while (!queue.empty()) {
    std::size_t u = queue.front();
    queue.pop();

    if (dist_[u] < dist_[n_left_]) {
      for (std::size_t v : adj_[u]) {
        std::size_t pair_v = match_right_[v];
        std::size_t idx = (pair_v == NIL) ? n_left_ : pair_v;
        if (dist_[idx] == std::numeric_limits<std::size_t>::max()) {
          dist_[idx] = dist_[u] + 1;
          if (idx != n_left_) {
            queue.push(idx);
          }
        }
      }
    }
  }

  return dist_[n_left_] != std::numeric_limits<std::size_t>::max();
}

bool BipartiteMatching::dfs(std::size_t u) {
  if (u == n_left_) return true;  // Sentinel: free right node

  for (std::size_t v : adj_[u]) {
    std::size_t pair_v = match_right_[v];
    std::size_t idx = (pair_v == NIL) ? n_left_ : pair_v;

    if (dist_[idx] == dist_[u] + 1 && dfs(idx)) {
      match_right_[v] = u;
      match_left_[u] = v;
      return true;
    }
  }

  dist_[u] = std::numeric_limits<std::size_t>::max();
  return false;
}

std::size_t BipartiteMatching::solve() {
  matched_ = 0;

  while (bfs()) {
    for (std::size_t u = 0; u < n_left_; ++u) {
      if (match_left_[u] == NIL && dfs(u)) {
        ++matched_;
      }
    }
  }

  return matched_;
}

std::optional<std::size_t> BipartiteMatching::match_for_left(std::size_t u) const {
  if (u >= n_left_ || match_left_[u] == NIL) return std::nullopt;
  return match_left_[u];
}

std::optional<std::size_t> BipartiteMatching::match_for_right(std::size_t v) const {
  if (v >= n_right_ || match_right_[v] == NIL) return std::nullopt;
  return match_right_[v];
}

std::size_t BipartiteMatching::matched_count() const noexcept {
  return matched_;
}

}  // namespace drm::planes
