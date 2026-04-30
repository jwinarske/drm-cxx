// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "page_flip.hpp"

#include "../core/device.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <xf86drm.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <sys/epoll.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace drm {

namespace {

constexpr std::size_t k_max_dispatch_events = 8;

}  // namespace

void page_flip_handler(int /*fd*/, unsigned int /*sequence*/, const unsigned int tv_sec,
                       const unsigned int tv_usec, void* user_data) {
  const auto* pf = static_cast<PageFlip*>(user_data);
  if (pf == nullptr || !pf->handler_) {
    return;
  }
  uint64_t const timestamp_ns = (static_cast<uint64_t>(tv_sec) * 1'000'000'000ULL) +
                                (static_cast<uint64_t>(tv_usec) * 1'000ULL);
  // We pass 0 for crtc_id and sequence here; the v2 handler below is preferred
  pf->handler_(0, 0, timestamp_ns);
}

void page_flip_handler_v2(int /*fd*/, const unsigned int sequence, const unsigned int tv_sec,
                          const unsigned int tv_usec, const unsigned int crtc_id, void* user_data) {
  const auto* pf = static_cast<PageFlip*>(user_data);
  if (pf == nullptr || !pf->handler_) {
    return;
  }
  uint64_t const timestamp_ns = (static_cast<uint64_t>(tv_sec) * 1'000'000'000ULL) +
                                (static_cast<uint64_t>(tv_usec) * 1'000ULL);
  pf->handler_(crtc_id, sequence, timestamp_ns);
}

PageFlip::PageFlip(const Device& dev) : drm_fd_(dev.fd()), epfd_(::epoll_create1(EPOLL_CLOEXEC)) {
  // Persistent epoll fd held for the PageFlip's lifetime — even with
  // no foreign sources we use it so dispatch() doesn't pay a
  // create/close round-trip per call. If epoll_create1 fails the
  // member stays -1; dispatch() then surfaces an error rather than
  // crashing.
  if (epfd_ >= 0 && drm_fd_ >= 0) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = drm_fd_;
    // Failure here is non-fatal: dispatch() will surface DRM-fd
    // readiness failures, and foreign sources still work via the
    // separate sources_ map.
    (void)::epoll_ctl(epfd_, EPOLL_CTL_ADD, drm_fd_, &ev);
  }
}

PageFlip::~PageFlip() {
  close_epfd();
}

PageFlip::PageFlip(PageFlip&& other) noexcept
    : drm_fd_(other.drm_fd_),
      epfd_(other.epfd_),
      handler_(std::move(other.handler_)),
      sources_(std::move(other.sources_)) {
  other.drm_fd_ = -1;
  other.epfd_ = -1;
}

PageFlip& PageFlip::operator=(PageFlip&& other) noexcept {
  if (this != &other) {
    close_epfd();
    drm_fd_ = other.drm_fd_;
    epfd_ = other.epfd_;
    handler_ = std::move(other.handler_);
    sources_ = std::move(other.sources_);
    other.drm_fd_ = -1;
    other.epfd_ = -1;
  }
  return *this;
}

void PageFlip::set_handler(Handler handler) {
  handler_ = std::move(handler);
}

drm::expected<void, std::error_code> PageFlip::add_source(int fd, SourceCallback cb) {
  if (fd < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  if (epfd_ < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  if (sources_.find(fd) != sources_.end()) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = fd;
  if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  sources_.emplace(fd, std::move(cb));
  return {};
}

void PageFlip::remove_source(int fd) noexcept {
  const auto it = sources_.find(fd);
  if (it == sources_.end()) {
    return;
  }
  if (epfd_ >= 0) {
    // EPOLL_CTL_DEL can fail with ENOENT if the kernel already
    // dropped the fd (e.g. the caller closed it before remove_source).
    // Either way the slot is gone post-call, so we don't propagate.
    ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
  }
  sources_.erase(it);
}

drm::expected<void, std::error_code> PageFlip::dispatch(const int timeout_ms) {
  if (epfd_ < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  // No DRM fd and no foreign sources → nothing to wait on. Surface
  // bad_file_descriptor up front rather than silently blocking on an
  // empty epoll.
  if (drm_fd_ < 0 && sources_.empty()) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  std::array<epoll_event, k_max_dispatch_events> events{};
  const int nfds = ::epoll_wait(epfd_, events.data(), static_cast<int>(events.size()), timeout_ms);
  if (nfds < 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  if (nfds == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::timed_out));
  }

  // Route each ready event. DRM fd → drmHandleEvent; any other fd is
  // a registered foreign source. Dispatch DRM and foreign callbacks
  // in arrival order — neither side should depend on the other's
  // ordering, but if a caller is racing a libcamera completion against
  // a page-flip we hand them whatever the kernel surfaced first.
  drmEventContext ctx{};
  ctx.version = 3;
  ctx.page_flip_handler = page_flip_handler;
  ctx.page_flip_handler2 = page_flip_handler_v2;

  for (int i = 0; i < nfds; ++i) {
    const int fd = events.at(static_cast<std::size_t>(i)).data.fd;
    if (fd == drm_fd_) {
      if (drmHandleEvent(drm_fd_, &ctx) != 0) {
        return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
      }
      continue;
    }
    const auto it = sources_.find(fd);
    if (it == sources_.end()) {
      // Registration was removed mid-dispatch (callback called
      // remove_source on its own fd, say). Skip; the next dispatch
      // won't see this fd.
      continue;
    }
    if (it->second) {
      it->second();
    }
  }
  return {};
}

void PageFlip::close_epfd() noexcept {
  if (epfd_ >= 0) {
    ::close(epfd_);
    epfd_ = -1;
  }
}

}  // namespace drm
