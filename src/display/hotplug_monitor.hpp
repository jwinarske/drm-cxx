// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// hotplug_monitor.hpp — pollable DRM connector hotplug watcher.
//
// Wraps libudev's netlink monitor with DRM-specific filtering so
// consumers don't have to rediscover the uevent shape every time they
// want to react to a monitor being plugged or unplugged. Events are
// pre-filtered to `ACTION=change` on `SUBSYSTEM=drm` with the
// `HOTPLUG=1` property set — the handler never fires for unrelated
// `change` actions (CRTC property blob swaps, leases, etc.).
//
// Matches the integration shape of drm::input::Seat: a pollable fd
// plus a dispatch() that fires a user-set handler synchronously. Add
// fd() to any poll/epoll set; call dispatch() when it's readable.

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <system_error>

namespace drm::display {

/// A single DRM hotplug uevent, after filtering.
struct HotplugEvent {
  /// The device node the kernel reported (typically `/dev/dri/cardN`).
  /// May be an empty view if the kernel omitted DEVNAME.
  std::string_view devnode;

  /// The specific connector that changed, when the kernel provides a
  /// `CONNECTOR=<id>` hint (≥4.16). When absent — older kernels or
  /// blanket hotplug events from GPU resets — the consumer should
  /// re-enumerate every connector via `drm::get_resources` +
  /// `drm::get_connector` rather than trust the hint.
  std::optional<uint32_t> connector_id;
};

/// Pollable DRM hotplug monitor. Non-copyable, move-only.
class HotplugMonitor {
 public:
  using Handler = std::function<void(const HotplugEvent&)>;

  /// Opens a libudev netlink monitor filtered to the `drm` subsystem.
  /// Returns on failure (udev_new, monitor creation, or enabling the
  /// filter) with the underlying errno wrapped in a system_category
  /// error code.
  static drm::expected<HotplugMonitor, std::error_code> open();

  /// Install or replace the handler. Called from `dispatch()` once per
  /// accepted event.
  void set_handler(Handler handler);

  /// Pollable fd — readable when a uevent is pending. Safe to add to
  /// poll() / epoll() alongside any other fds.
  [[nodiscard]] int fd() const noexcept;

  /// Drain every pending uevent. For each one that passes the DRM +
  /// `HOTPLUG=1` filter, the handler fires synchronously with the
  /// parsed `HotplugEvent`. Returns the first error encountered, or
  /// void on success. Safe to call with no handler installed — events
  /// are still drained (so the fd goes back to unreadable), just not
  /// delivered.
  drm::expected<void, std::error_code> dispatch();

  ~HotplugMonitor();
  HotplugMonitor(HotplugMonitor&& /*other*/) noexcept;
  HotplugMonitor& operator=(HotplugMonitor&& /*other*/) noexcept;
  HotplugMonitor(const HotplugMonitor&) = delete;
  HotplugMonitor& operator=(const HotplugMonitor&) = delete;

 private:
  HotplugMonitor() = default;

  // Opaque pointers — actual types from libudev.h. Kept as void* so
  // this header doesn't leak <libudev.h> into consumers.
  void* udev_{};
  void* monitor_{};
  int fd_{-1};
  Handler handler_;
};

}  // namespace drm::display