// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Private (not installed): the allocator's spatial split. Layers that overlap,
// directly or through a chain of overlaps, form one group; groups are placed
// one after another from a shared plane pool, so their order decides who gets
// a contested plane.
//
// The order is the highest keep priority in each group, descending, so the
// rule place_group applies inside a group (a Video layer outranks a Generic
// one) also holds between groups. Ties keep the input order: the group whose
// first member comes first goes first. Members stay in input order.

#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <utility>
#include <vector>

namespace drm::planes::detail {

/// Split `items` into groups connected by `intersect(a, b)`, ordered by
/// `priority` (highest member first, ties in input order).
template <typename T, typename Intersect, typename Priority>
[[nodiscard]] std::vector<std::vector<T>> independent_groups(const std::vector<T>& items,
                                                             const Intersect& intersect,
                                                             const Priority& priority) {
  const std::size_t n = items.size();
  std::vector<std::size_t> parent(n);
  std::iota(parent.begin(), parent.end(), static_cast<std::size_t>(0));
  auto find = [&](std::size_t x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = i + 1; j < n; ++j) {
      if (intersect(items[i], items[j])) {
        const auto a = find(i);
        const auto b = find(j);
        if (a != b) {
          parent[std::max(a, b)] = std::min(a, b);  // root = lowest index
        }
      }
    }
  }

  // Collected in order of each group's first member.
  std::vector<std::vector<T>> groups;
  std::vector<std::size_t> slot(n, n);  // root -> index into groups
  for (std::size_t i = 0; i < n; ++i) {
    const auto root = find(i);
    if (slot[root] == n) {
      slot[root] = groups.size();
      groups.emplace_back();
    }
    groups[slot[root]].push_back(items[i]);
  }

  using Key = decltype(priority(items.front()));
  std::vector<std::pair<Key, std::size_t>> keys;  // (highest priority, group)
  keys.reserve(groups.size());
  for (std::size_t g = 0; g < groups.size(); ++g) {
    Key best = priority(groups[g].front());
    for (const auto& item : groups[g]) {
      best = std::max(best, priority(item));
    }
    keys.emplace_back(best, g);
  }
  std::stable_sort(keys.begin(), keys.end(),
                   [](const auto& a, const auto& b) { return a.first > b.first; });

  std::vector<std::vector<T>> ordered;
  ordered.reserve(groups.size());
  for (const auto& [best, g] : keys) {
    ordered.push_back(std::move(groups[g]));
  }
  return ordered;
}

}  // namespace drm::planes::detail
