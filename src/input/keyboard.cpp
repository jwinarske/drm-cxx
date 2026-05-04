// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "keyboard.hpp"

#include "seat.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <xkbcommon/xkbcommon-names.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

namespace drm::input {

namespace {

// Standard evdev keycodes for the lock keys. These are layout-
// independent — RMLVO never remaps them — so hard-coding is safe.
constexpr std::uint32_t key_caps_lock = 58;
constexpr std::uint32_t key_num_lock = 69;
constexpr std::uint32_t key_scroll_lock = 70;

}  // namespace

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
    : ctx_(other.ctx_),
      keymap_(other.keymap_),
      state_(other.state_),
      held_keys_(std::move(other.held_keys_)) {
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
    held_keys_ = std::move(other.held_keys_);

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
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_enough_memory));
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
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  kb.state_ = xkb_state_new(kb.keymap_);
  if (kb.state_ == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_enough_memory));
  }

  return kb;
}

drm::expected<Keyboard, std::error_code> Keyboard::create_from_file(std::string_view keymap_path) {
  Keyboard kb;

  kb.ctx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (kb.ctx_ == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_enough_memory));
  }

  std::string const path_str(keymap_path);
  FILE* f = std::fopen(path_str.c_str(), "r");
  if (f == nullptr) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  kb.keymap_ =
      xkb_keymap_new_from_file(kb.ctx_, f, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  std::fclose(f);

  if (kb.keymap_ == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  kb.state_ = xkb_state_new(kb.keymap_);
  if (kb.state_ == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_enough_memory));
  }

  return kb;
}

drm::expected<Keyboard, std::error_code> Keyboard::create_from_string(std::string_view buffer) {
  Keyboard kb;

  kb.ctx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (kb.ctx_ == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_enough_memory));
  }

  // Use the explicit-length form so a non-NUL-terminated string_view
  // (e.g. a slice of an mmap'd Wayland keymap fd) is handled safely.
  kb.keymap_ = xkb_keymap_new_from_buffer(kb.ctx_, buffer.data(), buffer.size(),
                                          XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (kb.keymap_ == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  kb.state_ = xkb_state_new(kb.keymap_);
  if (kb.state_ == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_enough_memory));
  }

  return kb;
}

drm::expected<void, std::error_code> Keyboard::reload(KeymapOptions opts) {
  if (ctx_ == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  // Build the replacement keymap+state into locals first; only swap on
  // success so a malformed RMLVO leaves the existing state intact.
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

  auto* new_keymap = xkb_keymap_new_from_names(ctx_, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (new_keymap == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  auto* new_state = xkb_state_new(new_keymap);
  if (new_state == nullptr) {
    xkb_keymap_unref(new_keymap);
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_enough_memory));
  }

  // Snapshot before the swap so we can replay onto the fresh state.
  KeyboardLeds const snapped_leds = leds_state();

  if (state_ != nullptr) {
    xkb_state_unref(state_);
  }
  if (keymap_ != nullptr) {
    xkb_keymap_unref(keymap_);
  }
  state_ = new_state;
  keymap_ = new_keymap;

  // Replay held keys so a still-pressed Shift / Ctrl / letter doesn't
  // get stranded with a stale-down state on the new keymap. Lock keys
  // are included; their re-toggle is reconciled by set_leds() below.
  for (auto k : held_keys_) {
    xkb_state_update_key(state_, k + 8, XKB_KEY_DOWN);
  }

  // Restore the lock latch the snapshot captured, regardless of how
  // the held-key replay ended up.
  set_leds(snapped_leds);

  return {};
}

void Keyboard::set_leds(KeyboardLeds desired) noexcept {
  if (state_ == nullptr) {
    return;
  }
  auto current = leds_state();
  auto toggle = [this](std::uint32_t key) {
    xkb_state_update_key(state_, key + 8, XKB_KEY_DOWN);
    xkb_state_update_key(state_, key + 8, XKB_KEY_UP);
  };
  if (desired.caps_lock != current.caps_lock) {
    toggle(key_caps_lock);
  }
  if (desired.num_lock != current.num_lock) {
    toggle(key_num_lock);
  }
  if (desired.scroll_lock != current.scroll_lock) {
    toggle(key_scroll_lock);
  }
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

  // Track held keys for reload() replay. Press adds (idempotent),
  // release removes. Lock keys are tracked the same way; reload's
  // set_leds() reconciles any latch drift their replay introduces.
  if (event.pressed) {
    if (std::find(held_keys_.begin(), held_keys_.end(), event.key) == held_keys_.end()) {
      held_keys_.push_back(event.key);
    }
  } else {
    held_keys_.erase(std::remove(held_keys_.begin(), held_keys_.end(), event.key),
                     held_keys_.end());
  }

  // Get keysym
  event.sym = xkb_state_key_get_one_sym(const_cast<struct xkb_state*>(state_), keycode);

  // Get UTF-8 representation
  std::memset(event.utf8, 0, sizeof(event.utf8));
  xkb_state_key_get_utf8(const_cast<struct xkb_state*>(state_), keycode, event.utf8,
                         sizeof(event.utf8));
}

bool Keyboard::should_repeat(uint32_t key) const noexcept {
  if (keymap_ == nullptr) {
    return false;
  }
  // xkbcommon uses evdev keycodes + 8.
  xkb_keycode_t const keycode = key + 8;
  return xkb_keymap_key_repeats(keymap_, keycode) != 0;
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

bool Keyboard::caps_lock_active() const noexcept {
  if (state_ == nullptr) {
    return false;
  }
  return xkb_state_led_name_is_active(const_cast<struct xkb_state*>(state_), XKB_LED_NAME_CAPS) > 0;
}

bool Keyboard::num_lock_active() const noexcept {
  if (state_ == nullptr) {
    return false;
  }
  return xkb_state_led_name_is_active(const_cast<struct xkb_state*>(state_), XKB_LED_NAME_NUM) > 0;
}

bool Keyboard::scroll_lock_active() const noexcept {
  if (state_ == nullptr) {
    return false;
  }
  return xkb_state_led_name_is_active(const_cast<struct xkb_state*>(state_), XKB_LED_NAME_SCROLL) >
         0;
}

KeyboardLeds Keyboard::leds_state() const noexcept {
  KeyboardLeds out;
  if (state_ == nullptr) {
    return out;
  }
  out.caps_lock =
      xkb_state_led_name_is_active(const_cast<struct xkb_state*>(state_), XKB_LED_NAME_CAPS) > 0;
  out.num_lock =
      xkb_state_led_name_is_active(const_cast<struct xkb_state*>(state_), XKB_LED_NAME_NUM) > 0;
  out.scroll_lock =
      xkb_state_led_name_is_active(const_cast<struct xkb_state*>(state_), XKB_LED_NAME_SCROLL) > 0;
  return out;
}

}  // namespace drm::input
