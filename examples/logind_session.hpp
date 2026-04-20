// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// logind_session.hpp — thin sdbus-c++ (v2.x) wrapper over the subset of
// systemd-logind's org.freedesktop.login1.Session API that KMS examples
// care about: TakeControl / TakeDevice / PauseDevice / ResumeDevice /
// ReleaseDevice / ReleaseControl.
//
// Using logind gives us two things:
//   1. A revocable DRM fd — when the user Ctrl+Alt+F-switches away,
//      logind revokes the fd (ioctls fail with -ENODEV) and fires
//      PauseDevice, so the example can stop driving scanout cleanly.
//      On switch-back, logind fires ResumeDevice with a fresh fd.
//   2. Automatic session cleanup on process death — including SIGKILL.
//      Logind sees our DBus connection drop, calls ReleaseControl
//      itself, and triggers a VT switch back to the text console. That
//      replaces the "frozen last frame" hang with a proper redraw.
//
// Scope: examples only. Not part of the main drm-cxx library (the
// sdbus-c++ dependency stays out of src/).

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <sys/types.h>

namespace drm::examples {

/// Thin RAII wrapper around an `org.freedesktop.login1.Session`
/// DBus proxy. Call `open()` once at startup; destruct at shutdown
/// (dtor releases control and all taken devices).
class LogindSession {
 public:
  /// Handle returned by `take_device`. The `fd` is the revocable DRM
  /// (or input) fd handed back by logind and owned by the session
  /// (closed on `release_device` / dtor). `paused=true` means the
  /// session was inactive at take-time — caller should treat this as
  /// an initial pause and wait for a ResumeDevice signal.
  struct DeviceHandle {
    int fd;
    bool paused;
    dev_t device;
  };

  /// `on_pause(device, type)` fires on `PauseDevice`. `type` is one of:
  ///   "pause" — you must stop rendering, then call `ack_pause` to let
  ///             logind drop master. Fd remains open but revoked.
  ///   "force" — already revoked; no ack needed, stop rendering.
  ///   "gone"  — a device removed entirely.
  using PauseCallback = std::function<void(dev_t device, std::string_view type)>;

  /// `on_resume(device, new_fd)` fires on `ResumeDevice`. The new_fd
  /// replaces the previous fd (the old one is already revoked). The
  /// DeviceHandle ownership transfer — caller is responsible for
  /// re-seeding any per-fd state (master, framebuffers, etc.). The
  /// session retains the new fd and will close it on release_device.
  using ResumeCallback = std::function<void(dev_t device, int new_fd)>;

  /// Factory. Returns nullopt if we're not in a logind-managed session
  /// (e.g., running from a bare VT with no systemd, or inside a
  /// container without a seat). Callers should fall back to opening
  /// the DRM device directly in that case.
  [[nodiscard]] static std::optional<LogindSession> open();

  LogindSession(LogindSession&&) noexcept;
  LogindSession& operator=(LogindSession&&) noexcept;
  LogindSession(const LogindSession&) = delete;
  LogindSession& operator=(const LogindSession&) = delete;
  ~LogindSession();

  /// TakeDevice for a stat'd device node. Throws on DBus error.
  /// Returns nullopt if logind refused (e.g. seat mismatch).
  [[nodiscard]] std::optional<DeviceHandle> take_device(dev_t device) const;
  [[nodiscard]] std::optional<DeviceHandle> take_device(std::string_view path);

  /// ReleaseDevice + close the stored fd.
  void release_device(dev_t device) const;

  /// Ack a "pause" signal so logind drops master on our behalf.
  void ack_pause_complete(dev_t device) const;

  void set_pause_callback(PauseCallback fn) const;
  void set_resume_callback(ResumeCallback fn) const;

  /// A fd suitable for adding to poll()/epoll; becomes readable when
  /// there's a pending DBus message to process. Returns -1 if the
  /// session isn't active.
  [[nodiscard]] int poll_fd() const;

  /// Drain pending DBus messages — call when poll_fd() signals ready.
  /// Callbacks fire synchronously from inside this call.
  void dispatch() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit LogindSession(std::unique_ptr<Impl> impl);
};

}  // namespace drm::examples
