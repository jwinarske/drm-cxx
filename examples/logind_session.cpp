// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "logind_session.hpp"

#include "drm-cxx/detail/format.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <system_error>
#include <unordered_map>
#include <utility>

#if DRM_CXX_EXAMPLES_HAVE_LOGIND
// The sdbus-c++ umbrella header pulls in every type this TU uses
// (IConnection, IProxy, Slot, Error, UnixFd, ObjectPath, ServiceName,
// createSystemBusConnection, createProxy, return_slot). It is the
// library's public entrypoint; suppress IWYU's per-symbol nags.
#include <sdbus-c++/sdbus-c++.h>  // NOLINT(misc-include-cleaner)
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#endif

namespace drm::examples {

#if !DRM_CXX_EXAMPLES_HAVE_LOGIND

// ---------------------------------------------------------------------------
// Stub implementation used when the build wasn't linked against sdbus-c++.
// Every method behaves as if no logind session were available; callers see
// std::nullopt from open() and fall back to opening the DRM device directly.
// ---------------------------------------------------------------------------

struct LogindSession::Impl {};

LogindSession::LogindSession(std::unique_ptr<Impl> /*unused*/) {}
LogindSession::LogindSession(LogindSession&&) noexcept = default;
LogindSession& LogindSession::operator=(LogindSession&&) noexcept = default;
LogindSession::~LogindSession() = default;

std::optional<LogindSession> LogindSession::open() {
  return std::nullopt;
}

std::optional<LogindSession::DeviceHandle> LogindSession::take_device(dev_t /*device*/) {
  return std::nullopt;
}
std::optional<LogindSession::DeviceHandle> LogindSession::take_device(std::string_view /*path*/) {
  return std::nullopt;
}
void LogindSession::release_device(dev_t /*device*/) {}
void LogindSession::ack_pause_complete(dev_t /*device*/) {}
void LogindSession::set_pause_callback(PauseCallback /*fn*/) {}
void LogindSession::set_resume_callback(ResumeCallback /*fn*/) {}
int LogindSession::poll_fd() const {
  return -1;
}
void LogindSession::dispatch() {}

}  // namespace drm::examples

#else

// sdbus-c++ ships a single umbrella header; its individual type headers
// aren't meant to be included directly. Disable misc-include-cleaner for
// the rest of this TU so it doesn't flag every sdbus::* name as
// "no direct include".
// NOLINTBEGIN(misc-include-cleaner)

namespace {

constexpr auto login_bus = "org.freedesktop.login1";
constexpr auto manager_path = "/org/freedesktop/login1";
constexpr auto manager_iface = "org.freedesktop.login1.Manager";
constexpr auto session_iface = "org.freedesktop.login1.Session";

// Pack a dev_t into logind's (uint32 major, uint32 minor) signal key.
uint64_t pack(const uint32_t major, const uint32_t minor) {
  return (static_cast<uint64_t>(major) << 32) | minor;
}
uint64_t pack(dev_t d) {
  return pack(major(d), minor(d));
}

}  // namespace

struct LogindSession::Impl {
  std::unique_ptr<sdbus::IConnection> connection;
  std::unique_ptr<sdbus::IProxy> session_proxy;
  std::string session_path;
  sdbus::Slot pause_slot;
  sdbus::Slot resume_slot;
  std::unordered_map<uint64_t, int> owned_fds;  // key: packed (major,minor)
  PauseCallback pause_cb;
  ResumeCallback resume_cb;
  bool control_taken = false;
};

LogindSession::LogindSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LogindSession::LogindSession(LogindSession&&) noexcept = default;
LogindSession& LogindSession::operator=(LogindSession&&) noexcept = default;

LogindSession::~LogindSession() {
  if (!impl_) {
    return;
  }
  // Close + release every device we took.
  for (auto& [key, fd] : impl_->owned_fds) {
    if (fd >= 0) {
      ::close(fd);
    }
    const auto major_v = static_cast<uint32_t>(key >> 32);
    const auto minor_v = static_cast<uint32_t>(key & 0xffffffffU);
    try {
      impl_->session_proxy->callMethod("ReleaseDevice")
          .onInterface(session_iface)
          .withArguments(major_v, minor_v);
    } catch (const sdbus::Error&) {  // NOLINT(bugprone-empty-catch) — teardown
    }
  }
  if (impl_->control_taken) {
    try {
      impl_->session_proxy->callMethod("ReleaseControl").onInterface(session_iface);
    } catch (const sdbus::Error&) {  // NOLINT(bugprone-empty-catch) — teardown
    }
  }
}

std::optional<LogindSession> LogindSession::open() {
  try {
    auto connection = sdbus::createSystemBusConnection();

    // Resolve our session: prefer XDG_SESSION_ID if set, else fall back
    // to GetSessionByPID(getpid()).
    const auto manager = sdbus::createProxy(*connection, sdbus::ServiceName{login_bus},
                                            sdbus::ObjectPath{manager_path});
    sdbus::ObjectPath session_path;
    if (const char* xdg = std::getenv("XDG_SESSION_ID"); xdg != nullptr && *xdg != '\0') {
      manager->callMethod("GetSession")
          .onInterface(manager_iface)
          .withArguments(std::string(xdg))
          .storeResultsTo(session_path);
    } else {
      manager->callMethod("GetSessionByPID")
          .onInterface(manager_iface)
          .withArguments(static_cast<uint32_t>(::getpid()))
          .storeResultsTo(session_path);
    }

    auto session_proxy =
        sdbus::createProxy(*connection, sdbus::ServiceName{login_bus}, session_path);

    // TakeControl(force=false). Fails if another session controller exists.
    session_proxy->callMethod("TakeControl").onInterface(session_iface).withArguments(false);

    auto impl = std::make_unique<Impl>();
    impl->connection = std::move(connection);
    impl->session_proxy = std::move(session_proxy);
    impl->session_path = std::string(session_path);
    impl->control_taken = true;

    // Subscribe to pause/resume. Use raw pointer capture (safe because
    // slot outlives callbacks only while impl_ is alive, and dtor drops
    // slots before impl_).
    Impl* p = impl.get();
    impl->pause_slot =
        impl->session_proxy->uponSignal("PauseDevice")
            .onInterface(session_iface)
            .call(
                [p](const uint32_t major_v, const uint32_t minor_v, const std::string& type) {
                  if (p->pause_cb) {
                    p->pause_cb(makedev(major_v, minor_v), type);
                  }
                },
                sdbus::return_slot);

    impl->resume_slot =
        impl->session_proxy->uponSignal("ResumeDevice")
            .onInterface(session_iface)
            .call(
                [p](const uint32_t major_v, const uint32_t minor_v, sdbus::UnixFd new_fd) {
                  const dev_t dev = makedev(major_v, minor_v);
                  const int released = new_fd.release();
                  if (const auto it = p->owned_fds.find(pack(major_v, minor_v));
                      it != p->owned_fds.end()) {
                    if (it->second >= 0) {
                      ::close(it->second);
                    }
                    it->second = released;
                  } else {
                    // Resume for a device we don't own — close to avoid leak.
                    ::close(released);
                    return;
                  }
                  if (p->resume_cb) {
                    p->resume_cb(dev, released);
                  }
                },
                sdbus::return_slot);

    return LogindSession(std::move(impl));
  } catch (const sdbus::Error& e) {
    drm::println(stderr, "LogindSession::open: {} ({})", e.getMessage(), e.getName());
    return std::nullopt;
  } catch (const std::exception& e) {
    drm::println(stderr, "LogindSession::open: {}", e.what());
    return std::nullopt;
  }
}

std::optional<LogindSession::DeviceHandle> LogindSession::take_device(const dev_t device) const {
  if (!impl_) {
    return std::nullopt;
  }
  try {
    sdbus::UnixFd ufd;
    bool inactive = false;
    impl_->session_proxy->callMethod("TakeDevice")
        .onInterface(session_iface)
        .withArguments(static_cast<uint32_t>(major(device)), static_cast<uint32_t>(minor(device)))
        .storeResultsTo(ufd, inactive);

    const int fd = ufd.release();
    impl_->owned_fds[pack(device)] = fd;
    return DeviceHandle{fd, inactive, device};
  } catch (const sdbus::Error& e) {
    drm::println(stderr, "TakeDevice({}:{}): {}", major(device), minor(device), e.getMessage());
    return std::nullopt;
  }
}

std::optional<LogindSession::DeviceHandle> LogindSession::take_device(const std::string_view path) {
  struct stat st{};
  const std::string path_str(path);
  if (::stat(path_str.c_str(), &st) != 0) {
    drm::println(stderr, "stat {}: {}", path_str, std::system_category().message(errno));
    return std::nullopt;
  }
  return take_device(st.st_rdev);
}

void LogindSession::release_device(const dev_t device) const {
  if (!impl_) {
    return;
  }
  const auto key = pack(device);
  const auto it = impl_->owned_fds.find(key);
  if (it == impl_->owned_fds.end()) {
    return;
  }
  if (it->second >= 0) {
    ::close(it->second);
  }
  impl_->owned_fds.erase(it);
  try {
    impl_->session_proxy->callMethod("ReleaseDevice")
        .onInterface(session_iface)
        .withArguments(static_cast<uint32_t>(major(device)), static_cast<uint32_t>(minor(device)));
  } catch (const sdbus::Error&) {  // NOLINT(bugprone-empty-catch) — teardown
  }
}

void LogindSession::ack_pause_complete(const dev_t device) const {
  if (!impl_) {
    return;
  }
  try {
    impl_->session_proxy->callMethod("PauseDeviceComplete")
        .onInterface(session_iface)
        .withArguments(static_cast<uint32_t>(major(device)), static_cast<uint32_t>(minor(device)));
  } catch (const sdbus::Error& e) {
    drm::println(stderr, "PauseDeviceComplete({}:{}): {}", major(device), minor(device),
                 e.getMessage());
  }
}

void LogindSession::set_pause_callback(PauseCallback fn) const {
  if (impl_) {
    impl_->pause_cb = std::move(fn);
  }
}

void LogindSession::set_resume_callback(ResumeCallback fn) const {
  if (impl_) {
    impl_->resume_cb = std::move(fn);
  }
}

int LogindSession::poll_fd() const {
  if (!impl_) {
    return -1;
  }
  return impl_->connection->getEventLoopPollData().fd;
}

void LogindSession::dispatch() const {
  if (!impl_) {
    return;
  }
  while (impl_->connection->processPendingEvent()) {
    // drain
  }
}

// NOLINTEND(misc-include-cleaner)

}  // namespace drm::examples

#endif  // DRM_CXX_EXAMPLES_HAVE_LOGIND
