// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// select_device.hpp — enumerate /dev/dri/card* and prompt the user if needed.

#pragma once

#include "drm-cxx/detail/format.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace drm::examples {

/// Enumerate /dev/dri/card* devices, returning a sorted list of paths.
inline std::vector<std::string> enumerate_cards() {
  std::vector<std::string> cards;
  const std::filesystem::path dri_dir("/dev/dri");
  if (!std::filesystem::exists(dri_dir)) {
    return cards;
  }

  for (const auto& entry : std::filesystem::directory_iterator(dri_dir)) {
    if (const auto name = entry.path().filename().string(); name.compare(0, 4, "card") == 0) {
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
    drm::println("Using {}", cards.front());
    return cards.front();
  }

  drm::println("Available DRM devices:");
  for (std::size_t i = 0; i < cards.size(); ++i) {
    drm::println("  [{}] {}", i, cards.at(i));
  }

  drm::print("Select device [0-{}]: ", cards.size() - 1);
  std::cout.flush();

  std::string line;
  if (!std::getline(std::cin, line) || line.empty()) {
    drm::println(stderr, "No selection made");
    return std::nullopt;
  }

  std::size_t idx = 0;
  const auto [ptr, ec] = std::from_chars(line.data(), line.data() + line.size(), idx);
  if (ec != std::errc{} || idx >= cards.size()) {
    drm::println(stderr, "Invalid selection: {}", line);
    return std::nullopt;
  }

  return cards.at(idx);
}

}  // namespace drm::examples
