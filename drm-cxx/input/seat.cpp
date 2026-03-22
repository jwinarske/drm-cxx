// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "seat.hpp"

#include <libinput.h>

#include <cerrno>
#include <expected>
#include <fcntl.h>
#include <libudev.h>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace drm::input {

namespace {

auto* li(void* p) {
  return static_cast<struct libinput*>(p);
}
auto* ud(void* p) {
  return static_cast<struct udev*>(p);
}

int open_restricted(const char* path, int flags, void* /*user_data*/) {
  int const fd = ::open(path, flags | O_CLOEXEC);
  if (fd < 0) {
    return -errno;
  }
  return fd;
}

void close_restricted(int fd, void* /*user_data*/) {
  ::close(fd);
}

const struct libinput_interface li_interface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

}  // namespace

Seat::~Seat() {
  if (li_ != nullptr) {
    libinput_unref(li(li_));
  }
  if (udev_ != nullptr) {
    udev_unref(ud(udev_));
  }
}

Seat::Seat(Seat&& other) noexcept
    : li_(other.li_), udev_(other.udev_), handler_(std::move(other.handler_)), fd_(other.fd_) {
  other.li_ = nullptr;
  other.udev_ = nullptr;
  other.fd_ = -1;
}

Seat& Seat::operator=(Seat&& other) noexcept {
  if (this != &other) {
    if (li_ != nullptr) {
      libinput_unref(li(li_));
    }
    if (udev_ != nullptr) {
      udev_unref(ud(udev_));
    }

    li_ = other.li_;
    udev_ = other.udev_;
    handler_ = std::move(other.handler_);
    fd_ = other.fd_;

    other.li_ = nullptr;
    other.udev_ = nullptr;
    other.fd_ = -1;
  }
  return *this;
}

std::expected<Seat, std::error_code> Seat::open(SeatOptions opts) {
  Seat seat;

  seat.udev_ = udev_new();
  if (seat.udev_ == nullptr) {
    return std::unexpected(std::make_error_code(std::errc::no_such_device));
  }

  seat.li_ = libinput_udev_create_context(&li_interface, nullptr, ud(seat.udev_));
  if (seat.li_ == nullptr) {
    return std::unexpected(std::make_error_code(std::errc::no_such_device));
  }

  std::string const seat_name(opts.seat_name);
  if (libinput_udev_assign_seat(li(seat.li_), seat_name.c_str()) != 0) {
    return std::unexpected(std::make_error_code(std::errc::no_such_device));
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

std::expected<void, std::error_code> Seat::dispatch() {
  if (li_ == nullptr) {
    return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
  }

  if (libinput_dispatch(li(li_)) != 0) {
    return std::unexpected(std::error_code(errno, std::system_category()));
  }

  process_events();
  return {};
}

std::expected<void, std::error_code> Seat::suspend() {
  if (li_ == nullptr) {
    return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
  }
  libinput_suspend(li(li_));
  return {};
}

std::expected<void, std::error_code> Seat::resume() {
  if (li_ == nullptr) {
    return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
  }
  if (libinput_resume(li(li_)) != 0) {
    return std::unexpected(std::error_code(errno, std::system_category()));
  }
  return {};
}

void Seat::process_events() {
  if (!handler_) {
    return;
  }

  struct libinput_event* ev = nullptr;
  while ((ev = libinput_get_event(li(li_))) != nullptr) {
    auto type = libinput_event_get_type(ev);

    switch (type) {
      case LIBINPUT_EVENT_KEYBOARD_KEY: {
        auto* kev = libinput_event_get_keyboard_event(ev);
        if (kev == nullptr) {
          break;
        }
        KeyboardEvent ke{
            .time_ms = libinput_event_keyboard_get_time(kev),
            .key = libinput_event_keyboard_get_key(kev),
            .pressed = libinput_event_keyboard_get_key_state(kev) == LIBINPUT_KEY_STATE_PRESSED,
        };
        handler_(InputEvent{ke});
        break;
      }

      case LIBINPUT_EVENT_POINTER_MOTION: {
        auto* pev = libinput_event_get_pointer_event(ev);
        if (pev == nullptr) {
          break;
        }
        PointerMotionEvent me{
            .time_ms = libinput_event_pointer_get_time(pev),
            .dx = libinput_event_pointer_get_dx(pev),
            .dy = libinput_event_pointer_get_dy(pev),
        };
        handler_(InputEvent{PointerEvent{me}});
        break;
      }

      case LIBINPUT_EVENT_POINTER_BUTTON: {
        auto* pev = libinput_event_get_pointer_event(ev);
        if (pev == nullptr) {
          break;
        }
        PointerButtonEvent be{
            .time_ms = libinput_event_pointer_get_time(pev),
            .button = libinput_event_pointer_get_button(pev),
            .pressed =
                libinput_event_pointer_get_button_state(pev) == LIBINPUT_BUTTON_STATE_PRESSED,
        };
        handler_(InputEvent{PointerEvent{be}});
        break;
      }

      case LIBINPUT_EVENT_POINTER_AXIS: {
        auto* pev = libinput_event_get_pointer_event(ev);
        if (pev == nullptr) {
          break;
        }
        PointerAxisEvent ae{
            .time_ms = libinput_event_pointer_get_time(pev),
        };
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
        TouchEvent te{
            .time_ms = libinput_event_touch_get_time(tev),
            .slot = libinput_event_touch_get_slot(tev),
            .x = libinput_event_touch_get_x(tev),
            .y = libinput_event_touch_get_y(tev),
            .type = TouchEvent::Type::Down,
        };
        handler_(InputEvent{te});
        break;
      }

      case LIBINPUT_EVENT_TOUCH_UP: {
        auto* tev = libinput_event_get_touch_event(ev);
        if (tev == nullptr) {
          break;
        }
        TouchEvent te{
            .time_ms = libinput_event_touch_get_time(tev),
            .slot = libinput_event_touch_get_slot(tev),
            .type = TouchEvent::Type::Up,
        };
        handler_(InputEvent{te});
        break;
      }

      case LIBINPUT_EVENT_TOUCH_MOTION: {
        auto* tev = libinput_event_get_touch_event(ev);
        if (tev == nullptr) {
          break;
        }
        TouchEvent te{
            .time_ms = libinput_event_touch_get_time(tev),
            .slot = libinput_event_touch_get_slot(tev),
            .x = libinput_event_touch_get_x(tev),
            .y = libinput_event_touch_get_y(tev),
            .type = TouchEvent::Type::Motion,
        };
        handler_(InputEvent{te});
        break;
      }

      case LIBINPUT_EVENT_TOUCH_FRAME: {
        TouchEvent te{.type = TouchEvent::Type::Frame};
        handler_(InputEvent{te});
        break;
      }

      case LIBINPUT_EVENT_TOUCH_CANCEL: {
        TouchEvent te{.type = TouchEvent::Type::Cancel};
        handler_(InputEvent{te});
        break;
      }

      case LIBINPUT_EVENT_SWITCH_TOGGLE: {
        auto* sev = libinput_event_get_switch_event(ev);
        if (sev == nullptr) {
          break;
        }
        SwitchEvent se{
            .time_ms = libinput_event_switch_get_time(sev),
        };
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
