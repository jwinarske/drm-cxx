// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "page_flip.hpp"

#include "../core/device.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <xf86drm.h>

#include <cerrno>
#include <cstdint>
#include <sys/epoll.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace drm {

void page_flip_handler(int /*fd*/, unsigned int /*sequence*/, unsigned int tv_sec,
                       unsigned int tv_usec, void* user_data) {
  auto* pf = static_cast<PageFlip*>(user_data);
  if (pf->handler_) {
    uint64_t const timestamp_ns = (static_cast<uint64_t>(tv_sec) * 1'000'000'000ULL) +
                                  (static_cast<uint64_t>(tv_usec) * 1'000ULL);
    // We pass 0 for crtc_id and sequence here; the v2 handler below is preferred
    pf->handler_(0, 0, timestamp_ns);
  }
}

void page_flip_handler_v2(int /*fd*/, unsigned int sequence, unsigned int tv_sec,
                          unsigned int tv_usec, unsigned int crtc_id, void* user_data) {
  auto* pf = static_cast<PageFlip*>(user_data);
  if (pf->handler_) {
    uint64_t const timestamp_ns = (static_cast<uint64_t>(tv_sec) * 1'000'000'000ULL) +
                                  (static_cast<uint64_t>(tv_usec) * 1'000ULL);
    pf->handler_(crtc_id, sequence, timestamp_ns);
  }
}

PageFlip::PageFlip(const Device& dev) : drm_fd_(dev.fd()) {}

PageFlip::~PageFlip() = default;

void PageFlip::set_handler(Handler handler) {
  handler_ = std::move(handler);
}

drm::expected<void, std::error_code> PageFlip::dispatch(int timeout_ms) const {
  if (drm_fd_ < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  // Use epoll for the wait
  int const epfd = epoll_create1(EPOLL_CLOEXEC);
  if (epfd < 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  struct epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = drm_fd_;
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, drm_fd_, &ev) < 0) {
    ::close(epfd);
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  struct epoll_event events[1];
  int const nfds = epoll_wait(epfd, events, 1, timeout_ms);
  ::close(epfd);

  if (nfds < 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  if (nfds == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::timed_out));
  }

  // Handle DRM events
  drmEventContext ctx{};
  ctx.version = 3;
  ctx.page_flip_handler = page_flip_handler;
  ctx.page_flip_handler2 = page_flip_handler_v2;

  if (drmHandleEvent(drm_fd_, &ctx) != 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  return {};
}

}  // namespace drm
