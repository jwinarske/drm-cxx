// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// select_device.hpp — enumerate /dev/dri/card* and prompt the user if needed.

#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "drm-cxx/detail/format.hpp"

namespace drm::examples {

/// Enumerate /dev/dri/card* devices, returning a sorted list of paths.
inline std::vector<std::string> enumerate_cards() {
  std::vector<std::string> cards;
  const std::filesystem::path dri_dir("/dev/dri");
  if (!std::filesystem::exists(dri_dir)) {
    return cards;
  }

  for (const auto& entry : std::filesystem::directory_iterator(dri_dir)) {
    const auto name = entry.path().filename().string();
    if (name.starts_with("card")) {
      cards.push_back(entry.path().string());
    }
  }
  std::sort(cards.begin(), cards.end());
  return cards;
}

/// Select a DRM device path.
///
/// If a command-line argument is provided, use it directly.
/// Otherwise enumerate /dev/dri/card* devices: auto-select if exactly one,
/// prompt the user to choose if multiple.
///
/// Returns std::nullopt on error (no devices found or invalid input).
inline std::optional<std::string> select_device(int argc, char* argv[]) {
  if (argc > 1) {
    return std::string(argv[1]);
  }

  auto cards = enumerate_cards();
  if (cards.empty()) {
    drm::println(stderr, "No DRM devices found in /dev/dri/");
    return std::nullopt;
  }

  if (cards.size() == 1) {
    drm::println("Using {}", cards[0]);
    return cards[0];
  }

  drm::println("Available DRM devices:");
  for (std::size_t i = 0; i < cards.size(); ++i) {
    drm::println("  [{}] {}", i, cards[i]);
  }

  drm::print("Select device [0-{}]: ", cards.size() - 1);
  std::cout.flush();

  std::string line;
  if (!std::getline(std::cin, line) || line.empty()) {
    drm::println(stderr, "No selection made");
    return std::nullopt;
  }

  char* end = nullptr;
  errno = 0;
  const auto idx = std::strtoul(line.c_str(), &end, 10);
  if (end == line.c_str() || errno == ERANGE || idx >= cards.size()) {
    drm::println(stderr, "Invalid selection: {}", line);
    return std::nullopt;
  }

  return cards[idx];
}

}  // namespace drm::examples
