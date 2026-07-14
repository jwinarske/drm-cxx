// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "seat.hpp"

#include "keyboard.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <libinput.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <libudev.h>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace drm::input {

// libinput's user_data. See the declaration in seat.hpp for why both
// the opener and the log sink have to share this one slot.
struct SeatContext {
  InputDeviceOpener opener;
  LogHandler log;
};

namespace {

auto* li(void* p) {
  return static_cast<struct libinput*>(p);
}
auto* ud(void* p) {
  return static_cast<struct udev*>(p);
}

// Trampolines bridging libinput's C callback ABI to our SeatContext.
// user_data is a pointer to the Seat-owned SeatContext (heap-allocated
// so its address survives moves). Both callbacks are present; whether
// they delegate to ::open or to a caller-provided lambda depends on how
// the opener was constructed.
int on_open_restricted(const char* path, int flags, void* user_data) {
  const auto* ctx = static_cast<const SeatContext*>(user_data);
  if (ctx != nullptr && ctx->opener.open) {
    return ctx->opener.open(path, flags);
  }
  int const fd = ::open(path, flags | O_CLOEXEC);
  if (fd < 0) {
    return -errno;
  }
  return fd;
}

void on_close_restricted(int fd, void* user_data) {
  const auto* ctx = static_cast<const SeatContext*>(user_data);
  if (ctx != nullptr && ctx->opener.close) {
    ctx->opener.close(fd);
    return;
  }
  ::close(fd);
}

const struct libinput_interface li_interface = {
    on_open_restricted,
    on_close_restricted,
};

// libinput hands the log handler only the context, so the SeatContext
// comes back out of the user_data slot it was installed into.
void on_log(struct libinput* ctx_li, enum libinput_log_priority priority, const char* format,
            va_list args) {
  const auto* ctx = static_cast<const SeatContext*>(libinput_get_user_data(ctx_li));
  if (ctx == nullptr) {
    return;
  }
  detail::dispatch_log(ctx->log, static_cast<int>(priority), format, args);
}

libinput_log_priority to_libinput_priority(LogPriority p) noexcept {
  switch (p) {
    case LogPriority::Debug:
      return LIBINPUT_LOG_PRIORITY_DEBUG;
    case LogPriority::Info:
      return LIBINPUT_LOG_PRIORITY_INFO;
    case LogPriority::Error:
      break;
  }
  return LIBINPUT_LOG_PRIORITY_ERROR;
}

}  // namespace

namespace detail {

LogPriority map_log_priority(int libinput_priority) noexcept {
  if (libinput_priority <= LIBINPUT_LOG_PRIORITY_DEBUG) {
    return LogPriority::Debug;
  }
  if (libinput_priority <= LIBINPUT_LOG_PRIORITY_INFO) {
    return LogPriority::Info;
  }
  return LogPriority::Error;
}

void dispatch_log(const LogHandler& handler, int libinput_priority, const char* format,
                  va_list args) {
  if (!handler || format == nullptr) {
    return;
  }

  // vsnprintf consumes the va_list, and libinput makes no promise about
  // what it does with args afterward — copy rather than move through.
  std::array<char, 1024> buf{};
  va_list copy;
  va_copy(copy, args);
  int const n = std::vsnprintf(buf.data(), buf.size(), format, copy);
  va_end(copy);
  if (n < 0) {
    return;
  }

  // n is the length that *would* have been written; clamp to what was.
  auto const len = std::min(static_cast<size_t>(n), buf.size() - 1);
  std::string_view msg(buf.data(), len);
  // libinput terminates its messages with a newline. Structured sinks
  // add their own framing, so hand over the bare message.
  while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
    msg.remove_suffix(1);
  }
  handler(map_log_priority(libinput_priority), msg);
}

}  // namespace detail

Seat::~Seat() {
  for (void* p : keyboard_devices_) {
    libinput_device_unref(static_cast<libinput_device*>(p));
  }
  keyboard_devices_.clear();
  if (li_ != nullptr) {
    libinput_unref(li(li_));
  }
  if (udev_ != nullptr) {
    udev_unref(ud(udev_));
  }
}

Seat::Seat(Seat&& other) noexcept
    : li_(other.li_),
      udev_(other.udev_),
      handler_(std::move(other.handler_)),
      ctx_(std::move(other.ctx_)),
      keyboard_devices_(std::move(other.keyboard_devices_)),
      last_leds_(other.last_leds_),
      fd_(other.fd_) {
  other.li_ = nullptr;
  other.udev_ = nullptr;
  other.fd_ = -1;
}

Seat& Seat::operator=(Seat&& other) noexcept {
  if (this != &other) {
    for (void* p : keyboard_devices_) {
      libinput_device_unref(static_cast<libinput_device*>(p));
    }
    keyboard_devices_.clear();
    if (li_ != nullptr) {
      libinput_unref(li(li_));
    }
    if (udev_ != nullptr) {
      udev_unref(ud(udev_));
    }

    li_ = other.li_;
    udev_ = other.udev_;
    handler_ = std::move(other.handler_);
    ctx_ = std::move(other.ctx_);
    keyboard_devices_ = std::move(other.keyboard_devices_);
    last_leds_ = other.last_leds_;
    fd_ = other.fd_;

    other.li_ = nullptr;
    other.udev_ = nullptr;
    other.fd_ = -1;
  }
  return *this;
}

drm::expected<Seat, std::error_code> Seat::open(SeatOptions opts) {
  return Seat::open(std::move(opts), InputDeviceOpener{});
}

drm::expected<Seat, std::error_code> Seat::open(SeatOptions opts, InputDeviceOpener opener) {
  // A mismatched pair (only one of open/close set) is always a bug:
  // whoever allocated the fd must also be the one freeing it. Reject
  // up front rather than have libinput paper over it.
  if (static_cast<bool>(opener.open) != static_cast<bool>(opener.close)) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  Seat seat;
  // Heap-allocate the context so its address is stable for libinput's
  // user_data, regardless of how Seat moves later.
  seat.ctx_ =
      std::make_unique<SeatContext>(SeatContext{std::move(opener), std::move(opts.log_handler)});

  seat.udev_ = udev_new();
  if (seat.udev_ == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::no_such_device));
  }

  seat.li_ = libinput_udev_create_context(&li_interface, seat.ctx_.get(), ud(seat.udev_));
  if (seat.li_ == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::no_such_device));
  }

  // Install before assign_seat: that call is what enumerates the seat's
  // devices, so a handler installed after it would miss every
  // device-added diagnostic — the ones a consumer most wants. Leave
  // libinput's default handler and its ERROR threshold alone when the
  // caller didn't opt in.
  if (seat.ctx_->log) {
    libinput_log_set_handler(li(seat.li_), &on_log);
    libinput_log_set_priority(li(seat.li_), to_libinput_priority(opts.log_priority));
  }

  std::string const seat_name(opts.seat_name);
  if (libinput_udev_assign_seat(li(seat.li_), seat_name.c_str()) != 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::no_such_device));
  }

  seat.fd_ = libinput_get_fd(li(seat.li_));

  return seat;
}

void Seat::set_event_handler(EventHandler handler) {
  handler_ = std::move(handler);
}

int Seat::fd() const noexcept {
  return fd_;
}

drm::expected<void, std::error_code> Seat::dispatch() {
  if (li_ == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  if (libinput_dispatch(li(li_)) != 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  process_events();
  return {};
}

drm::expected<void, std::error_code> Seat::suspend() {
  if (li_ == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  libinput_suspend(li(li_));
  return {};
}

drm::expected<void, std::error_code> Seat::resume() {
  if (li_ == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  if (libinput_resume(li(li_)) != 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  return {};
}

void Seat::update_keyboard_leds(KeyboardLeds leds) {
  last_leds_ = leds;
  std::uint32_t mask = 0;
  if (leds.caps_lock) {
    mask |= LIBINPUT_LED_CAPS_LOCK;
  }
  if (leds.num_lock) {
    mask |= LIBINPUT_LED_NUM_LOCK;
  }
  if (leds.scroll_lock) {
    mask |= LIBINPUT_LED_SCROLL_LOCK;
  }
  for (void* p : keyboard_devices_) {
    // libinput_led is a bitmask enum; OR'd combinations aren't single
    // enumerators, so the analyzer's range check fires spuriously.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    auto led_mask = static_cast<libinput_led>(mask);
    libinput_device_led_update(static_cast<libinput_device*>(p), led_mask);
  }
}

void Seat::process_events() {
  struct libinput_event* ev = nullptr;
  while ((ev = libinput_get_event(li(li_))) != nullptr) {
    auto type = libinput_event_get_type(ev);

    // Device add/remove tracking happens regardless of whether a user
    // event handler is set, so update_keyboard_leds() works even when
    // the caller hasn't installed a handler yet.
    if (type == LIBINPUT_EVENT_DEVICE_ADDED) {
      auto* dev = libinput_event_get_device(ev);
      if (dev != nullptr &&
          libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_KEYBOARD) != 0) {
        libinput_device_ref(dev);
        keyboard_devices_.push_back(dev);
        // Re-apply the last known LED state so a hotplugged keyboard
        // (or the post-VT-switch resume that re-adds every device)
        // matches the xkb state instead of coming up dark.
        if (last_leds_.has_value()) {
          std::uint32_t mask = 0;
          if (last_leds_->caps_lock) {
            mask |= LIBINPUT_LED_CAPS_LOCK;
          }
          if (last_leds_->num_lock) {
            mask |= LIBINPUT_LED_NUM_LOCK;
          }
          if (last_leds_->scroll_lock) {
            mask |= LIBINPUT_LED_SCROLL_LOCK;
          }
          // See update_keyboard_leds() for the bitmask-enum NOLINT rationale.
          // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
          auto led_mask = static_cast<libinput_led>(mask);
          libinput_device_led_update(dev, led_mask);
        }
      }
      libinput_event_destroy(ev);
      continue;
    }
    if (type == LIBINPUT_EVENT_DEVICE_REMOVED) {
      auto* dev = libinput_event_get_device(ev);
      auto it =
          std::find(keyboard_devices_.begin(), keyboard_devices_.end(), static_cast<void*>(dev));
      if (it != keyboard_devices_.end()) {
        libinput_device_unref(static_cast<libinput_device*>(*it));
        keyboard_devices_.erase(it);
      }
      libinput_event_destroy(ev);
      continue;
    }

    if (!handler_) {
      libinput_event_destroy(ev);
      continue;
    }

    switch (type) {
      case LIBINPUT_EVENT_KEYBOARD_KEY: {
        auto* kev = libinput_event_get_keyboard_event(ev);
        if (kev == nullptr) {
          break;
        }
        KeyboardEvent ke;
        ke.time_ms = libinput_event_keyboard_get_time(kev);
        ke.key = libinput_event_keyboard_get_key(kev);
        ke.pressed = libinput_event_keyboard_get_key_state(kev) == LIBINPUT_KEY_STATE_PRESSED;
        handler_(InputEvent{ke});
        break;
      }

      case LIBINPUT_EVENT_POINTER_MOTION: {
        auto* pev = libinput_event_get_pointer_event(ev);
        if (pev == nullptr) {
          break;
        }
        PointerMotionEvent me;
        me.time_ms = libinput_event_pointer_get_time(pev);
        me.dx = libinput_event_pointer_get_dx(pev);
        me.dy = libinput_event_pointer_get_dy(pev);
        me.device_name = libinput_device_get_name(libinput_event_get_device(ev));
        handler_(InputEvent{PointerEvent{me}});
        break;
      }

      case LIBINPUT_EVENT_POINTER_BUTTON: {
        auto* pev = libinput_event_get_pointer_event(ev);
        if (pev == nullptr) {
          break;
        }
        PointerButtonEvent be;
        be.time_ms = libinput_event_pointer_get_time(pev);
        be.button = libinput_event_pointer_get_button(pev);
        be.pressed = libinput_event_pointer_get_button_state(pev) == LIBINPUT_BUTTON_STATE_PRESSED;
        handler_(InputEvent{PointerEvent{be}});
        break;
      }

      case LIBINPUT_EVENT_POINTER_AXIS: {
        auto* pev = libinput_event_get_pointer_event(ev);
        if (pev == nullptr) {
          break;
        }
        PointerAxisEvent ae;
        ae.time_ms = libinput_event_pointer_get_time(pev);
        if (libinput_event_pointer_has_axis(pev, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL) != 0) {
          ae.horizontal =
              libinput_event_pointer_get_axis_value(pev, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
        }
        if (libinput_event_pointer_has_axis(pev, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL) != 0) {
          ae.vertical =
              libinput_event_pointer_get_axis_value(pev, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
        }
        handler_(InputEvent{PointerEvent{ae}});
        break;
      }

      case LIBINPUT_EVENT_TOUCH_DOWN: {
        auto* tev = libinput_event_get_touch_event(ev);
        if (tev == nullptr) {
          break;
        }
        TouchEvent te;
        te.time_ms = libinput_event_touch_get_time(tev);
        te.slot = libinput_event_touch_get_slot(tev);
        te.device_name = libinput_device_get_name(libinput_event_get_device(ev));
        // Normalized [0,1) touch position in the device's native orientation.
        // The raw libinput coordinate is in mm; normalizing here lets the
        // consumer scale to its render extent and apply any display rotation
        // without needing the digitizer's physical size.
        te.x = libinput_event_touch_get_x_transformed(tev, 1.0);
        te.y = libinput_event_touch_get_y_transformed(tev, 1.0);
        te.type = TouchEvent::Type::Down;
        handler_(InputEvent{te});
        break;
      }

      case LIBINPUT_EVENT_TOUCH_UP: {
        auto* tev = libinput_event_get_touch_event(ev);
        if (tev == nullptr) {
          break;
        }
        TouchEvent te;
        te.time_ms = libinput_event_touch_get_time(tev);
        te.slot = libinput_event_touch_get_slot(tev);
        te.device_name = libinput_device_get_name(libinput_event_get_device(ev));
        te.type = TouchEvent::Type::Up;
        handler_(InputEvent{te});
        break;
      }

      case LIBINPUT_EVENT_TOUCH_MOTION: {
        auto* tev = libinput_event_get_touch_event(ev);
        if (tev == nullptr) {
          break;
        }
        TouchEvent te;
        te.time_ms = libinput_event_touch_get_time(tev);
        te.slot = libinput_event_touch_get_slot(tev);
        te.device_name = libinput_device_get_name(libinput_event_get_device(ev));
        // Normalized [0,1) touch position in the device's native orientation.
        // The raw libinput coordinate is in mm; normalizing here lets the
        // consumer scale to its render extent and apply any display rotation
        // without needing the digitizer's physical size.
        te.x = libinput_event_touch_get_x_transformed(tev, 1.0);
        te.y = libinput_event_touch_get_y_transformed(tev, 1.0);
        te.type = TouchEvent::Type::Motion;
        handler_(InputEvent{te});
        break;
      }

      case LIBINPUT_EVENT_TOUCH_FRAME: {
        TouchEvent te;
        te.type = TouchEvent::Type::Frame;
        handler_(InputEvent{te});
        break;
      }

      case LIBINPUT_EVENT_TOUCH_CANCEL: {
        TouchEvent te;
        te.type = TouchEvent::Type::Cancel;
        handler_(InputEvent{te});
        break;
      }

      case LIBINPUT_EVENT_SWITCH_TOGGLE: {
        auto* sev = libinput_event_get_switch_event(ev);
        if (sev == nullptr) {
          break;
        }
        SwitchEvent se;
        se.time_ms = libinput_event_switch_get_time(sev);
        auto sw = libinput_event_switch_get_switch(sev);
        if (sw == LIBINPUT_SWITCH_LID) {
          se.which = SwitchEvent::Switch::Lid;
        } else {
          se.which = SwitchEvent::Switch::TabletMode;
        }
        se.active = libinput_event_switch_get_switch_state(sev) == LIBINPUT_SWITCH_STATE_ON;
        handler_(InputEvent{se});
        break;
      }

      default:
        break;
    }

    libinput_event_destroy(ev);
  }
}

}  // namespace drm::input
