// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// session/seat.hpp — thin C++ wrapper around libseat, muxing logind,
// seatd, and the builtin/noop backend behind a single interface.
//
// What the seat session buys you:
//   - Seat-level activation signaling (enable_seat / disable_seat) so
//     VT switches stop leaving the TTY on a stale framebuffer.
//   - Revocable device fds (via libseat_open_device) so the kernel can
//     yank access cleanly on session switch-out.
//   - SIGKILL recovery — the seat provider (logind, seatd) sees our
//     socket/DBus connection drop and releases the seat itself.
//
// libseat auto-selects the available backend. On systems without
// systemd (Alpine, void, embedded automotive), seatd or the builtin
// backend can drive the same API. When drm-cxx was built without
// libseat, Seat::open() returns nullopt and consumers fall back to
// opening devices directly via drm::Device::open.

#pragma once

#include "input/seat.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

// Forward declaration at global scope so the trampoline declarations
// below don't pull libseat's header into every TU that includes this.
struct libseat;

namespace drm::session {

/// RAII wrapper around a libseat connection. Call `open()` once at
/// startup and hold for the process lifetime. The destructor closes
/// every tracked device and the seat itself.
class Seat {
 public:
  /// Fd + libseat device id returned by `take_device`. The fd is
  /// revocable; on resume it's replaced with a fresh fd and the
  /// resume callback fires. `device_id` is libseat's handle for the
  /// device — needed only if you want to release early via
  /// `release_device`.
  struct DeviceHandle {
    int fd;
    int device_id;
  };

  /// Fires when the seat has been disabled (e.g. user VT-switched
  /// away). The caller must stop using every device fd immediately —
  /// subsequent ioctls will fail. Seat acknowledges the pause to
  /// libseat automatically after the callback returns.
  using PauseCallback = std::function<void()>;

  /// Fires after the seat has become active again, once for every
  /// device the caller had taken. Seat has already reopened the
  /// device and holds a fresh fd; the argument is that new fd (the
  /// old one is closed and should be discarded). Every piece of
  /// per-fd kernel state (framebuffers, property blobs, client caps,
  /// GEM handles) is dead and must be rebuilt on the new fd.
  using ResumeCallback = std::function<void(std::string_view path, int new_fd)>;

  /// Returns nullopt if no seat backend is available (no logind, no
  /// seatd, no permissions for builtin) or if drm-cxx was built
  /// without libseat support. Callers fall back to opening devices
  /// directly.
  [[nodiscard]] static std::optional<Seat> open();

  Seat(Seat&&) noexcept;
  Seat& operator=(Seat&&) noexcept;
  Seat(const Seat&) = delete;
  Seat& operator=(const Seat&) = delete;
  ~Seat();

  /// Open a device by path. Only succeeds when the seat is currently
  /// active. Stores the (path, fd, device_id) so resume can reopen
  /// transparently.
  [[nodiscard]] std::optional<DeviceHandle> take_device(std::string_view path);

  /// Close a device explicitly. Normally unnecessary — the destructor
  /// closes everything.
  void release_device(std::string_view path);

  void set_pause_callback(PauseCallback fn);
  void set_resume_callback(ResumeCallback fn);

  /// Pollable fd for the libseat connection. Becomes readable when
  /// there's a pending seat event to dispatch. Returns -1 if the
  /// session isn't backed by a real seat.
  [[nodiscard]] int poll_fd() const;

  /// Drain pending libseat events. Callbacks fire synchronously from
  /// inside this call.
  void dispatch();

  /// Returns a `drm::input::InputDeviceOpener` whose open/close
  /// callbacks route through this Seat. Pass it to
  /// `drm::input::Seat::open(opts, opener)` so libinput's privileged
  /// device opens (and matching closes) go through libseat too —
  /// giving input fds the same revocable / resume-aware lifetime as
  /// the DRM fd. The returned opener keeps a pointer to this Seat's
  /// internal state (heap-stable across moves); it remains valid
  /// until this Seat is destroyed.
  [[nodiscard]] drm::input::InputDeviceOpener input_opener();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit Seat(std::unique_ptr<Impl> impl);

  // libseat listener trampolines. Must be static with C linkage-safe
  // signatures, so they can be stored in a `struct libseat_seat_listener`;
  // declared here so they can reach the private `Impl` type.
  static void on_enable_trampoline(::libseat* seat, void* userdata);
  static void on_disable_trampoline(::libseat* seat, void* userdata);
};

}  // namespace drm::session