// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "hotplug_monitor.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <libudev.h>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace drm::display {

namespace {

auto* ud(void* p) {
  return static_cast<struct udev*>(p);
}
auto* um(void* p) {
  return static_cast<struct udev_monitor*>(p);
}

// Parse the kernel's CONNECTOR=<id> uevent property. Absent or
// malformed → nullopt; caller re-enumerates everything.
std::optional<uint32_t> parse_connector_id(const char* value) {
  if (value == nullptr) {
    return std::nullopt;
  }
  const std::string_view sv(value);
  uint32_t id = 0;
  const auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), id);
  if (ec != std::errc{} || p != sv.data() + sv.size()) {
    return std::nullopt;
  }
  return id;
}

}  // namespace

HotplugMonitor::~HotplugMonitor() {
  if (monitor_ != nullptr) {
    udev_monitor_unref(um(monitor_));
  }
  if (udev_ != nullptr) {
    udev_unref(ud(udev_));
  }
}

HotplugMonitor::HotplugMonitor(HotplugMonitor&& other) noexcept
    : udev_(other.udev_),
      monitor_(other.monitor_),
      fd_(other.fd_),
      handler_(std::move(other.handler_)) {
  other.udev_ = nullptr;
  other.monitor_ = nullptr;
  other.fd_ = -1;
}

HotplugMonitor& HotplugMonitor::operator=(HotplugMonitor&& other) noexcept {
  if (this != &other) {
    if (monitor_ != nullptr) {
      udev_monitor_unref(um(monitor_));
    }
    if (udev_ != nullptr) {
      udev_unref(ud(udev_));
    }
    udev_ = other.udev_;
    monitor_ = other.monitor_;
    fd_ = other.fd_;
    handler_ = std::move(other.handler_);
    other.udev_ = nullptr;
    other.monitor_ = nullptr;
    other.fd_ = -1;
  }
  return *this;
}

drm::expected<HotplugMonitor, std::error_code> HotplugMonitor::open() {
  HotplugMonitor mon;

  mon.udev_ = udev_new();
  if (mon.udev_ == nullptr) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  mon.monitor_ = udev_monitor_new_from_netlink(ud(mon.udev_), "udev");
  if (mon.monitor_ == nullptr) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  if (udev_monitor_filter_add_match_subsystem_devtype(um(mon.monitor_), "drm", nullptr) != 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  if (udev_monitor_enable_receiving(um(mon.monitor_)) != 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  mon.fd_ = udev_monitor_get_fd(um(mon.monitor_));
  return mon;
}

void HotplugMonitor::set_handler(Handler handler) {
  handler_ = std::move(handler);
}

int HotplugMonitor::fd() const noexcept {
  return fd_;
}

drm::expected<void, std::error_code> HotplugMonitor::dispatch() {
  if (monitor_ == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  // Drain every pending event. udev_monitor_receive_device returns
  // nullptr once the socket is empty.
  while (auto* dev = udev_monitor_receive_device(um(monitor_))) {
    // The kernel-filter on the netlink socket only sees subsystem +
    // devtype; the "change" action + HOTPLUG=1 gate has to happen in
    // userspace. DRM's "change" fires for lease updates, property
    // blob swaps, etc.; real hotplug carries HOTPLUG=1.
    const char* hotplug = udev_device_get_property_value(dev, "HOTPLUG");
    const bool is_hotplug = hotplug != nullptr && std::string_view(hotplug) == "1";

    if (is_hotplug && handler_) {
      HotplugEvent ev;
      if (const char* devnode = udev_device_get_devnode(dev); devnode != nullptr) {
        ev.devnode = devnode;
      }
      ev.connector_id = parse_connector_id(udev_device_get_property_value(dev, "CONNECTOR"));
      handler_(ev);
    }

    udev_device_unref(dev);
  }
  return {};
}

}  // namespace drm::display