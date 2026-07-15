// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "seat.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <memory>
#include <optional>

#if DRM_CXX_HAS_LIBSEAT
// libseat.h ships without extern "C" guards; the symbols have C
// linkage, but our compiler would otherwise mangle the declarations as
// C++.
extern "C" {
#include <libseat.h>
}

#include "drm-cxx/detail/format.hpp"
#include "input/seat.hpp"  // drm::input::InputDeviceOpener
#include "log.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#endif

namespace drm::session {

#if !DRM_CXX_HAS_LIBSEAT

// ---------------------------------------------------------------------------
// Stub implementation — libseat wasn't available at build time. Every
// method behaves as if no seat were present; callers see nullopt from
// open() and fall back to opening devices directly.
//
// The stub can't change method shape — signatures must match the
// public declarations in seat.hpp, which are also what the real
// branch below implements. That forces member-function-not-static
// and pass-by-value callback params even where the stub body doesn't
// touch them, so readability/performance lints fire spuriously.
// NOLINTBEGIN(readability-convert-member-functions-to-static,
//             performance-unnecessary-value-param)
// ---------------------------------------------------------------------------

struct Seat::Impl {};

Seat::Seat(std::unique_ptr<Impl> /*unused*/) {}
Seat::Seat(Seat&&) noexcept = default;
Seat& Seat::operator=(Seat&&) noexcept = default;
Seat::~Seat() = default;

std::optional<Seat> Seat::open() {
  return std::nullopt;
}

std::optional<Seat::DeviceHandle> Seat::take_device(std::string_view /*path*/,
                                                    TakeDeviceOpts /*opts*/) {
  return std::nullopt;
}
void Seat::release_device(std::string_view /*path*/) {}
void Seat::set_pause_callback(PauseCallback /*fn*/) {}
void Seat::set_resume_callback(ResumeCallback /*fn*/) {}
int Seat::poll_fd() const {
  return -1;
}
void Seat::dispatch() {}
drm::expected<void, std::error_code> Seat::switch_session(int /*session*/) {
  return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_supported));
}
drm::input::InputDeviceOpener Seat::input_opener() {
  return {};
}

// NOLINTEND(readability-convert-member-functions-to-static,
//           performance-unnecessary-value-param)

}  // namespace drm::session

#else

namespace {

// Tracked-device entry: the path is what gets re-opened on resume, and
// libseat's device_id is what we pass to close_device.
//
// `preserve_fd_across_resume` records the caller's TakeDeviceOpts choice
// so on_enable_trampoline knows whether to skip the close+reopen and
// keep the same fd integer alive across the pause cycle.
struct TrackedDevice {
  std::string path;
  int fd{-1};
  int device_id{-1};
  bool preserve_fd_across_resume{false};
};

// Route libseat's own diagnostics into drm::log, tagged `[libseat]`, so
// backend-selection failures and revocation errors follow set_log_sink
// like the rest of the library instead of escaping to stderr. libseat
// delivers printf-style + va_list, so the line is rendered here and
// handed to drm::log pre-formatted.
//
// libseat's handler is process-global (no context argument, no
// user_data), which is why this takes no per-instance callback — the
// destination is a process-global sink either way.
void seat_log_trampoline(libseat_log_level level, const char* format, va_list args) {
  if (format == nullptr) {
    return;
  }
  std::array<char, 1024> buf{};
  va_list copy;
  va_copy(copy, args);
  int const n = std::vsnprintf(buf.data(), buf.size(), format, copy);
  va_end(copy);
  if (n < 0) {
    return;
  }
  auto const len = std::min(static_cast<size_t>(n), buf.size() - 1);
  std::string_view msg(buf.data(), len);
  while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
    msg.remove_suffix(1);
  }
  switch (level) {
    case LIBSEAT_LOG_LEVEL_INFO:
      drm::log_info("[libseat] {}", msg);
      return;
    case LIBSEAT_LOG_LEVEL_DEBUG:
      drm::log_debug("[libseat] {}", msg);
      return;
    case LIBSEAT_LOG_LEVEL_ERROR:
      drm::log_error("[libseat] {}", msg);
      return;
    default:
      break;
  }
  // LIBSEAT_LOG_LEVEL_SILENT, or a level a future libseat adds: surface
  // it rather than drop it, but do not rank it as an error.
  drm::log_warn("[libseat] {}", msg);
}

}  // namespace

struct Seat::Impl {
  // Serializes every libseat_* call and every mutation of `devices`.
  // libseat is documented as not thread-safe; without this mutex a
  // concurrent dispatch thread can consume the sd-bus reply intended
  // for a take_device call on another thread, surfacing as -EBADMSG
  // or -ETIMEDOUT from logind.
  //
  // Recursive because the trampoline → user-callback → input-opener
  // path is intentionally re-entrant on the same thread: dispatch()
  // takes the lock, libseat_dispatch fires on_enable / on_disable,
  // the user's resume_cb commonly drives input::Seat::resume() which
  // calls libinput_resume(), which invokes the InputDeviceOpener
  // open lambda this file installs — and that lambda takes the same
  // lock to read impl->active and impl->devices. All on the same
  // thread, so recursive_mutex is correct here; std::mutex would
  // self-deadlock the first VT resume.
  std::recursive_mutex mu;
  libseat* seat{nullptr};
  libseat_seat_listener listener{};
  std::unordered_map<std::string, TrackedDevice> devices;  // key = path
  PauseCallback pause_cb;
  ResumeCallback resume_cb;
  // True between enable_seat and disable_seat. Gates take_device so it
  // can't race the backend's handshake, and is toggled by every
  // enable/disable pair.
  bool active{false};
  // Sticky once the initial enable_seat has fired. Distinguishes the
  // first enable (open_seat handshake — nothing to reopen) from a
  // later enable that follows a disable (VT resume — fd revoked,
  // reopen + fire resume_cb). Without this, every resume looks like
  // the initial enable because `active` was just cleared by disable.
  bool ever_enabled{false};
};

Seat::Seat(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Seat::Seat(Seat&&) noexcept = default;
Seat& Seat::operator=(Seat&&) noexcept = default;

Seat::~Seat() {
  if (!impl_ || impl_->seat == nullptr) {
    return;
  }
  std::lock_guard const lk(impl_->mu);
  for (auto& [path, dev] : impl_->devices) {
    if (dev.device_id >= 0) {
      libseat_close_device(impl_->seat, dev.device_id);
    }
  }
  libseat_close_seat(impl_->seat);
  impl_->seat = nullptr;
}

// --------------------------------------------------------------------------
// libseat listener trampolines — recover the Impl from userdata and
// dispatch into C++. libseat guarantees these fire on the thread that
// called libseat_dispatch, which already holds impl->mu; the
// trampolines don't take the lock explicitly (it's already held) but
// any user callback they invoke is free to re-enter the Seat API
// because impl->mu is a recursive_mutex.
// --------------------------------------------------------------------------
void Seat::on_enable_trampoline(libseat* seat, void* userdata) {
  auto* impl = static_cast<Seat::Impl*>(userdata);

  // First enable_seat after open_seat: nothing to re-open, just flip
  // the flag. Subsequent enables are genuine resumes — for every
  // device we had, close the (possibly revoked) id and re-open the
  // path to get a fresh fd, then notify the caller.
  const bool first = !impl->ever_enabled;
  impl->ever_enabled = true;
  impl->active = true;
  if (first) {
    return;
  }

  for (auto& [path, dev] : impl->devices) {
    if (dev.preserve_fd_across_resume) {
      // Capability-revoke backend (logind / seatd / builtin): the fd
      // integer is still valid in our process, the kernel just lifted
      // the revoke on the master capability. Skip the close+reopen
      // and fire resume_cb with the existing fd so the consumer can
      // re-modeset whatever pipe state the kernel reset.
      if (impl->resume_cb) {
        impl->resume_cb(path, dev.fd);
      }
      continue;
    }
    if (dev.device_id >= 0) {
      libseat_close_device(seat, dev.device_id);
      dev.device_id = -1;
      dev.fd = -1;
    }
    int new_fd = -1;
    const int new_id = libseat_open_device(seat, path.c_str(), &new_fd);
    if (new_id < 0) {
      drm::println(stderr, "Seat: reopen {} on resume failed: {}", path,
                   std::system_category().message(errno));
      continue;
    }
    dev.device_id = new_id;
    dev.fd = new_fd;
    if (impl->resume_cb) {
      impl->resume_cb(path, new_fd);
    }
  }
}

void Seat::on_disable_trampoline(libseat* seat, void* userdata) {
  auto* impl = static_cast<Seat::Impl*>(userdata);
  impl->active = false;
  if (impl->pause_cb) {
    impl->pause_cb();
  }
  // Must ack within the callback window per libseat's contract;
  // otherwise the backend force-revokes.
  libseat_disable_seat(seat);
}

std::optional<Seat> Seat::open() {
  // Install the log handler before libseat_open_seat so any
  // backend-selection errors surface with context. Process-global,
  // not per-instance — which is fine because it's idempotent and
  // every instance wants the same routing.
  libseat_set_log_handler(&seat_log_trampoline);
  libseat_set_log_level(LIBSEAT_LOG_LEVEL_ERROR);

  auto impl = std::make_unique<Impl>();
  impl->listener.enable_seat = &Seat::on_enable_trampoline;
  impl->listener.disable_seat = &Seat::on_disable_trampoline;

  impl->seat = libseat_open_seat(&impl->listener, impl.get());
  if (impl->seat == nullptr) {
    drm::println(stderr, "libseat_open_seat failed: {}", std::system_category().message(errno));
    return std::nullopt;
  }

  // Drain until the initial enable_seat fires (or an error surfaces).
  // libseat_dispatch with -1 blocks until an event; one is guaranteed
  // on successful open.
  while (!impl->active) {
    if (libseat_dispatch(impl->seat, -1) < 0) {
      drm::println(stderr, "libseat_dispatch (initial) failed: {}",
                   std::system_category().message(errno));
      libseat_close_seat(impl->seat);
      return std::nullopt;
    }
  }

  return Seat(std::move(impl));
}

std::optional<Seat::DeviceHandle> Seat::take_device(const std::string_view path,
                                                    TakeDeviceOpts opts) {
  if (!impl_ || impl_->seat == nullptr) {
    return std::nullopt;
  }
  std::lock_guard const lk(impl_->mu);
  if (!impl_->active) {
    return std::nullopt;
  }
  const std::string key(path);
  int fd = -1;
  const int device_id = libseat_open_device(impl_->seat, key.c_str(), &fd);
  if (device_id < 0) {
    drm::println(stderr, "libseat_open_device({}): {}", key, std::system_category().message(errno));
    return std::nullopt;
  }
  impl_->devices[key] = TrackedDevice{key, fd, device_id, opts.preserve_fd_across_resume};
  return DeviceHandle{fd, device_id};
}

void Seat::release_device(const std::string_view path) {
  if (!impl_ || impl_->seat == nullptr) {
    return;
  }
  std::lock_guard const lk(impl_->mu);
  const auto it = impl_->devices.find(std::string(path));
  if (it == impl_->devices.end()) {
    return;
  }
  if (it->second.device_id >= 0) {
    libseat_close_device(impl_->seat, it->second.device_id);
  }
  impl_->devices.erase(it);
}

void Seat::set_pause_callback(PauseCallback fn) {
  if (impl_) {
    std::lock_guard const lk(impl_->mu);
    impl_->pause_cb = std::move(fn);
  }
}

void Seat::set_resume_callback(ResumeCallback fn) {
  if (impl_) {
    std::lock_guard const lk(impl_->mu);
    impl_->resume_cb = std::move(fn);
  }
}

int Seat::poll_fd() const {
  if (!impl_ || impl_->seat == nullptr) {
    return -1;
  }
  std::lock_guard const lk(impl_->mu);
  return libseat_get_fd(impl_->seat);
}

void Seat::dispatch() {
  if (!impl_ || impl_->seat == nullptr) {
    return;
  }
  std::lock_guard const lk(impl_->mu);
  // timeout=0: non-blocking, drain everything ready. Trampolines fire
  // synchronously from here with the lock held — see Impl::mu doc.
  while (libseat_dispatch(impl_->seat, 0) > 0) {
    // keep draining
  }
}

drm::expected<void, std::error_code> Seat::switch_session(int session) {
  if (!impl_ || impl_->seat == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  std::lock_guard const lk(impl_->mu);
  if (libseat_switch_session(impl_->seat, session) < 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  return {};
}

drm::input::InputDeviceOpener Seat::input_opener() {
  // Route libinput's open_restricted/close_restricted through libseat
  // so input fds get revocation and resume handling along with the
  // DRM fd.
  //
  // libinput hands the close callback only an fd, so we need an
  // fd→path side-table to look up what to release. The table is
  // captured by shared_ptr into both lambdas so it outlives the
  // InputDeviceOpener copy that gets moved into drm::input::Seat.
  //
  // We capture `impl_.get()` rather than `this`: the Impl is on the
  // heap, so its address is stable across Seat moves, while `this`
  // would dangle if the Seat were ever move-constructed. Both
  // lambdas manipulate impl->devices / libseat_* directly rather
  // than re-entering take_device / release_device, which are member
  // functions that would also capture a potentially-stale `this`.
  auto tracked = std::make_shared<std::unordered_map<int, std::string>>();
  Impl* impl = impl_.get();
  return drm::input::InputDeviceOpener{
      [impl, tracked](const char* path, int /*flags*/) -> int {
        if (impl == nullptr || impl->seat == nullptr) {
          return -ENOENT;
        }
        std::lock_guard const lk(impl->mu);
        if (!impl->active) {
          return -ENOENT;
        }
        const std::string key(path);
        int fd = -1;
        const int device_id = libseat_open_device(impl->seat, key.c_str(), &fd);
        if (device_id < 0) {
          return -ENOENT;
        }
        impl->devices[key] = TrackedDevice{key, fd, device_id};
        (*tracked)[fd] = key;
        return fd;
      },
      [impl, tracked](int fd) {
        const auto it = tracked->find(fd);
        if (it == tracked->end()) {
          // Not a seat-managed fd — shouldn't happen since libinput
          // only closes fds we returned, but fall back defensively
          // to avoid leaking the kernel fd.
          ::close(fd);
          return;
        }
        const std::string path = it->second;
        tracked->erase(it);
        if (impl == nullptr || impl->seat == nullptr) {
          ::close(fd);
          return;
        }
        std::lock_guard const lk(impl->mu);
        const auto dit = impl->devices.find(path);
        if (dit != impl->devices.end()) {
          if (dit->second.device_id >= 0) {
            libseat_close_device(impl->seat, dit->second.device_id);
          }
          impl->devices.erase(dit);
        }
      },
  };
}

}  // namespace drm::session

#endif  // DRM_CXX_HAS_LIBSEAT