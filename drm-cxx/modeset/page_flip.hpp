// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <system_error>

namespace drm {

class Device;

class PageFlip {
 public:
  using Handler =
      std::move_only_function<void(uint32_t crtc_id, uint64_t sequence, uint64_t timestamp_ns)>;

  explicit PageFlip(const Device& dev);

  void set_handler(Handler handler);

  // Wait for and dispatch a page flip event.
  // timeout_ms: -1 = block forever, 0 = non-blocking, >0 = timeout in ms.
  std::expected<void, std::error_code> dispatch(int timeout_ms = -1) const;

  ~PageFlip();

 private:
  // Allow the C callback trampolines to invoke handler_
  friend void page_flip_handler(int /*unused*/, unsigned int /*unused*/, unsigned int /*tv_sec*/,
                                unsigned int /*tv_usec*/, void* /*user_data*/);
  friend void page_flip_handler_v2(int /*unused*/, unsigned int /*sequence*/,
                                   unsigned int /*tv_sec*/, unsigned int /*tv_usec*/,
                                   unsigned int /*crtc_id*/, void* /*user_data*/);

  int drm_fd_{-1};
  Handler handler_;
};

}  // namespace drm
