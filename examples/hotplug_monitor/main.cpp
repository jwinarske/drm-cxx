// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// hotplug_monitor — watches for DRM connector hotplug events via udev.
//
// Usage: hotplug_monitor [/dev/dri/cardN]
//
// Monitors the "drm" udev subsystem for add/remove/change events,
// then re-queries the DRM device to report connector status.

#include "../select_device.hpp"
#include "core/device.hpp"
#include "core/resources.hpp"
#include "log.hpp"
#include "modeset/mode.hpp"

#include <xf86drmMode.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <libudev.h>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <sys/epoll.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// RAII wrappers for libudev handles
// ---------------------------------------------------------------------------
struct UdevDeleter {
  void operator()(udev* u) const noexcept { udev_unref(u); }
};
using UdevPtr = std::unique_ptr<udev, UdevDeleter>;

struct UdevMonitorDeleter {
  void operator()(udev_monitor* m) const noexcept { udev_monitor_unref(m); }
};
using UdevMonitorPtr = std::unique_ptr<udev_monitor, UdevMonitorDeleter>;

struct UdevDeviceDeleter {
  void operator()(udev_device* d) const noexcept { udev_device_unref(d); }
};
using UdevDevicePtr = std::unique_ptr<udev_device, UdevDeviceDeleter>;

struct EpollFd {
  explicit EpollFd(const int fd) noexcept : fd_(fd) {}
  ~EpollFd() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  EpollFd(const EpollFd&) = delete;
  EpollFd& operator=(const EpollFd&) = delete;
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int get() const noexcept { return fd_; }

 private:
  int fd_;
};

// ---------------------------------------------------------------------------
// UdevHotplugMonitor — watches a set of udev subsystems and fires a callback
// ---------------------------------------------------------------------------
class UdevHotplugMonitor {
 public:
  UdevHotplugMonitor(
      std::vector<std::string> sub_systems,
      std::function<void(const char* action, const char* devnode, const char* subsystem)> callback)
      : sub_systems_(std::move(sub_systems)), callback_(std::move(callback)) {
    if (pipe(pipe_fds_) == -1) {
      drm::log_error("Failed to create pipe: {} ({})", std::system_category().message(errno),
                     errno);
      pipe_fds_[0] = -1;
      pipe_fds_[1] = -1;
      return;
    }
    worker_thread_ = std::thread(&UdevHotplugMonitor::run, this);
  }

  UdevHotplugMonitor(const UdevHotplugMonitor&) = delete;
  UdevHotplugMonitor& operator=(const UdevHotplugMonitor&) = delete;
  UdevHotplugMonitor(UdevHotplugMonitor&&) = delete;
  UdevHotplugMonitor& operator=(UdevHotplugMonitor&&) = delete;

  ~UdevHotplugMonitor() {
    stop();
    if (worker_thread_.joinable()) {
      worker_thread_.join();
    }
    if (pipe_fds_[0] != -1) {
      close(pipe_fds_[0]);
    }
    if (pipe_fds_[1] != -1) {
      close(pipe_fds_[1]);
    }
  }

  void stop() {
    if (bool expected = true; !is_running_.compare_exchange_strong(expected, false)) {
      return;
    }
    if (pipe_fds_[1] != -1) {
      if (write(pipe_fds_[1], "x", 1) == -1) {
        drm::log_error("Failed to write to stop pipe: {} ({})",
                       std::system_category().message(errno), errno);
      }
    }
  }

 private:
  std::vector<std::string> sub_systems_;
  std::atomic<bool> is_running_{true};
  int pipe_fds_[2]{-1, -1};
  std::function<void(const char*, const char*, const char*)> callback_;
  std::thread worker_thread_;

  void run() {
    const UdevPtr udev(udev_new());
    if (!udev) {
      drm::log_error("Failed to create udev context");
      is_running_ = false;
      return;
    }

    const UdevMonitorPtr mon(udev_monitor_new_from_netlink(udev.get(), "udev"));
    if (!mon) {
      drm::log_error("Failed to create udev monitor");
      is_running_ = false;
      return;
    }

    for (const auto& sub : sub_systems_) {
      if (int res =
              udev_monitor_filter_add_match_subsystem_devtype(mon.get(), sub.c_str(), nullptr);
          res != 0) {
        drm::log_error(
            "udev_monitor_filter_add_match_subsystem_devtype "
            "failed on {} = {}",
            sub, res);
      }
    }
    udev_monitor_enable_receiving(mon.get());
    const auto udev_fd = udev_monitor_get_fd(mon.get());

    const EpollFd epoll_fd(epoll_create1(0));
    if (!epoll_fd.valid()) {
      drm::log_error("Failed to create epoll: {} ({})", std::system_category().message(errno),
                     errno);
      is_running_ = false;
      return;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;

    ev.data.fd = udev_fd;
    if (epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, udev_fd, &ev) == -1) {
      drm::log_error("Failed to add udev fd to epoll: {} ({})",
                     std::system_category().message(errno), errno);
      is_running_ = false;
      return;
    }

    ev.data.fd = pipe_fds_[0];
    if (epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, pipe_fds_[0], &ev) == -1) {
      drm::log_error("Failed to add pipe fd to epoll: {} ({})",
                     std::system_category().message(errno), errno);
      is_running_ = false;
      return;
    }

    while (is_running_) {
      epoll_event events[2];
      const int n = epoll_wait(epoll_fd.get(), events, 2, -1);
      if (n == -1) {
        if (errno == EINTR) {
          continue;
        }
        drm::log_error("epoll_wait failed: {} ({})", std::system_category().message(errno), errno);
        break;
      }

      for (int i = 0; i < n; ++i) {
        if (events[i].data.fd == pipe_fds_[0]) {
          drm::log_debug("Stop signal received, exiting monitor loop");
          is_running_ = false;
          break;
        }

        if (events[i].data.fd == udev_fd) {
          if (UdevDevicePtr const dev(udev_monitor_receive_device(mon.get())); dev) {
            if (callback_) {
              const char* action = udev_device_get_action(dev.get());
              const char* devnode = udev_device_get_devnode(dev.get());
              const char* subsystem = udev_device_get_subsystem(dev.get());
              if ((action != nullptr) && (subsystem != nullptr)) {
                callback_(action, devnode, subsystem);
              } else {
                drm::log_debug(
                    "Skipping event with missing properties: "
                    "action={}, devnode={}, subsystem={}",
                    (action != nullptr) ? action : "null", (devnode != nullptr) ? devnode : "null",
                    (subsystem != nullptr) ? subsystem : "null");
              }
            }
          }
        }
      }
    }
    drm::log_debug("UdevHotplugMonitor worker thread exiting");
  }
};

// ---------------------------------------------------------------------------
// Global signal handling
// ---------------------------------------------------------------------------
namespace {
volatile std::sig_atomic_t g_quit = 0;
}

static void signal_handler(int /*sig*/) {
  g_quit = 1;
}

// ---------------------------------------------------------------------------
// Print connector status for a DRM device
// ---------------------------------------------------------------------------
static void print_connector_status(int drm_fd) {
  const auto res = drm::get_resources(drm_fd);
  if (!res) {
    drm::log_error("Failed to get DRM resources");
    return;
  }

  std::println("  Connectors ({}):", res->count_connectors);
  for (int i = 0; i < res->count_connectors; ++i) {
    auto conn = drm::get_connector(drm_fd, res->connectors[i]);
    if (!conn) {
      continue;
    }

    const char* status = "unknown";
    switch (conn->connection) {
      case DRM_MODE_CONNECTED:
        status = "connected";
        break;
      case DRM_MODE_DISCONNECTED:
        status = "disconnected";
        break;
      default:
        break;
    }

    std::println("    [{}] connector-{}: {} ({} modes)", i, conn->connector_id, status,
                 conn->count_modes);

    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
      const auto modes = std::span<const drmModeModeInfo>(conn->modes, conn->count_modes);
      if (const auto pref = drm::select_preferred_mode(modes)) {
        std::println("      preferred: {}x{}@{}Hz", pref->width(), pref->height(), pref->refresh());
      }
    }
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(const int argc, char* argv[]) {
  const auto path = drm::examples::select_device(argc, argv);
  if (!path) {
    return EXIT_FAILURE;
  }

  auto dev_result = drm::Device::open(*path);
  if (!dev_result) {
    std::println(stderr, "Failed to open {}", *path);
    return EXIT_FAILURE;
  }
  auto& dev = *dev_result;

  std::println("Monitoring hotplug events on {}  (Ctrl-C to quit)", *path);
  std::println("Current state:");
  print_connector_status(dev.fd());

  // Install signal handlers for graceful shutdown
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  UdevHotplugMonitor monitor(
      {"drm"}, [&dev](const char* action, const char* devnode, const char* subsystem) {
        std::println("\n[hotplug] action={}, devnode={}, subsystem={}", action,
                     devnode ? devnode : "(none)", subsystem);

        // On any DRM event, re-query connector status
        if (std::string_view(action) == "change") {
          std::println("Connector status after hotplug:");
          print_connector_status(dev.fd());
        }
      });

  // Spin until Ctrl-C
  while (!g_quit) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::println("\nShutting down...");
  monitor.stop();

  return EXIT_SUCCESS;
}
