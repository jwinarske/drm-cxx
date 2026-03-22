// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <expected>
#include <string_view>
#include <system_error>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace drm::input {

struct KeyboardEvent;

struct KeymapOptions {
  std::string_view rules;
  std::string_view model;
  std::string_view layout;
  std::string_view variant;
  std::string_view options;
};

class Keyboard {
 public:
  // Create from RMLVO names (empty = system defaults).
  static std::expected<Keyboard, std::error_code> create(KeymapOptions opts = {});

  // Create from a keymap file path (e.g. $HOME/.xkb/keymap.xkb).
  static std::expected<Keyboard, std::error_code> create_from_file(std::string_view keymap_path);

  // Process a key event: fills in sym and utf8 fields.
  void process_key(KeyboardEvent& event) const;

  // Query modifier state.
  [[nodiscard]] bool shift_active() const noexcept;
  [[nodiscard]] bool ctrl_active() const noexcept;
  [[nodiscard]] bool alt_active() const noexcept;
  [[nodiscard]] bool super_active() const noexcept;

  ~Keyboard();
  Keyboard(Keyboard&&) noexcept;
  Keyboard& operator=(Keyboard&&) noexcept;
  Keyboard(const Keyboard&) = delete;
  Keyboard& operator=(const Keyboard&) = delete;

 private:
  Keyboard() = default;

  struct xkb_context* ctx_{};
  struct xkb_keymap* keymap_{};
  struct xkb_state* state_{};
};

}  // namespace drm::input
