// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <system_error>

namespace drm {

class Device;

class PageFlip {
public:
  using Handler = std::move_only_function<void(uint32_t crtc_id, uint64_t sequence, uint64_t timestamp_ns)>;

  explicit PageFlip(const Device& dev);

  void set_handler(Handler handler);

  std::expected<void, std::error_code> dispatch(int timeout_ms = -1);

  ~PageFlip();

private:
  int drm_fd_{-1};
  Handler handler_;
};

} // namespace drm
