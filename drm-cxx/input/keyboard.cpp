// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "keyboard.hpp"

#include "seat.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <xkbcommon/xkbcommon-names.h>
#include <xkbcommon/xkbcommon.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>

namespace drm::input {

Keyboard::~Keyboard() {
  if (state_ != nullptr) {
    xkb_state_unref(state_);
  }
  if (keymap_ != nullptr) {
    xkb_keymap_unref(keymap_);
  }
  if (ctx_ != nullptr) {
    xkb_context_unref(ctx_);
  }
}

Keyboard::Keyboard(Keyboard&& other) noexcept
    : ctx_(other.ctx_), keymap_(other.keymap_), state_(other.state_) {
  other.ctx_ = nullptr;
  other.keymap_ = nullptr;
  other.state_ = nullptr;
}

Keyboard& Keyboard::operator=(Keyboard&& other) noexcept {
  if (this != &other) {
    if (state_ != nullptr) {
      xkb_state_unref(state_);
    }
    if (keymap_ != nullptr) {
      xkb_keymap_unref(keymap_);
    }
    if (ctx_ != nullptr) {
      xkb_context_unref(ctx_);
    }

    ctx_ = other.ctx_;
    keymap_ = other.keymap_;
    state_ = other.state_;

    other.ctx_ = nullptr;
    other.keymap_ = nullptr;
    other.state_ = nullptr;
  }
  return *this;
}

drm::expected<Keyboard, std::error_code> Keyboard::create(KeymapOptions opts) {
  Keyboard kb;

  kb.ctx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (kb.ctx_ == nullptr) {
    return drm::unexpected(std::make_error_code(std::errc::not_enough_memory));
  }

  struct xkb_rule_names names{};
  std::string rules_str;
  std::string model_str;
  std::string layout_str;
  std::string variant_str;
  std::string options_str;

  if (!opts.rules.empty()) {
    rules_str = std::string(opts.rules);
    names.rules = rules_str.c_str();
  }
  if (!opts.model.empty()) {
    model_str = std::string(opts.model);
    names.model = model_str.c_str();
  }
  if (!opts.layout.empty()) {
    layout_str = std::string(opts.layout);
    names.layout = layout_str.c_str();
  }
  if (!opts.variant.empty()) {
    variant_str = std::string(opts.variant);
    names.variant = variant_str.c_str();
  }
  if (!opts.options.empty()) {
    options_str = std::string(opts.options);
    names.options = options_str.c_str();
  }

  kb.keymap_ = xkb_keymap_new_from_names(kb.ctx_, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (kb.keymap_ == nullptr) {
    return drm::unexpected(std::make_error_code(std::errc::invalid_argument));
  }

  kb.state_ = xkb_state_new(kb.keymap_);
  if (kb.state_ == nullptr) {
    return drm::unexpected(std::make_error_code(std::errc::not_enough_memory));
  }

  return kb;
}

drm::expected<Keyboard, std::error_code> Keyboard::create_from_file(std::string_view keymap_path) {
  Keyboard kb;

  kb.ctx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (kb.ctx_ == nullptr) {
    return drm::unexpected(std::make_error_code(std::errc::not_enough_memory));
  }

  std::string const path_str(keymap_path);
  FILE* f = std::fopen(path_str.c_str(), "r");
  if (f == nullptr) {
    return drm::unexpected(std::error_code(errno, std::system_category()));
  }

  kb.keymap_ =
      xkb_keymap_new_from_file(kb.ctx_, f, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  std::fclose(f);

  if (kb.keymap_ == nullptr) {
    return drm::unexpected(std::make_error_code(std::errc::invalid_argument));
  }

  kb.state_ = xkb_state_new(kb.keymap_);
  if (kb.state_ == nullptr) {
    return drm::unexpected(std::make_error_code(std::errc::not_enough_memory));
  }

  return kb;
}

void Keyboard::process_key(KeyboardEvent& event) const {
  if (state_ == nullptr) {
    return;
  }

  // xkbcommon uses evdev keycodes + 8
  xkb_keycode_t const keycode = event.key + 8;

  // Update state
  auto direction = event.pressed ? XKB_KEY_DOWN : XKB_KEY_UP;
  xkb_state_update_key(const_cast<struct xkb_state*>(state_), keycode, direction);

  // Get keysym
  event.sym = xkb_state_key_get_one_sym(const_cast<struct xkb_state*>(state_), keycode);

  // Get UTF-8 representation
  std::memset(event.utf8, 0, sizeof(event.utf8));
  xkb_state_key_get_utf8(const_cast<struct xkb_state*>(state_), keycode, event.utf8,
                         sizeof(event.utf8));
}

bool Keyboard::shift_active() const noexcept {
  if (state_ == nullptr) {
    return false;
  }
  return xkb_state_mod_name_is_active(const_cast<struct xkb_state*>(state_), XKB_MOD_NAME_SHIFT,
                                      XKB_STATE_MODS_EFFECTIVE) > 0;
}

bool Keyboard::ctrl_active() const noexcept {
  if (state_ == nullptr) {
    return false;
  }
  return xkb_state_mod_name_is_active(const_cast<struct xkb_state*>(state_), XKB_MOD_NAME_CTRL,
                                      XKB_STATE_MODS_EFFECTIVE) > 0;
}

bool Keyboard::alt_active() const noexcept {
  if (state_ == nullptr) {
    return false;
  }
  return xkb_state_mod_name_is_active(const_cast<struct xkb_state*>(state_), XKB_MOD_NAME_ALT,
                                      XKB_STATE_MODS_EFFECTIVE) > 0;
}

bool Keyboard::super_active() const noexcept {
  if (state_ == nullptr) {
    return false;
  }
  return xkb_state_mod_name_is_active(const_cast<struct xkb_state*>(state_), XKB_MOD_NAME_LOGO,
                                      XKB_STATE_MODS_EFFECTIVE) > 0;
}

}  // namespace drm::input
