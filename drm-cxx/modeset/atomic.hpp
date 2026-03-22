// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <xf86drmMode.h>

#include <cstdint>
#include <expected>
#include <system_error>

namespace drm {

class Device;

class AtomicRequest {
 public:
  explicit AtomicRequest(const Device& dev);

  std::expected<void, std::error_code> add_property(uint32_t object_id, uint32_t property_id,
                                                    uint64_t value);

  std::expected<void, std::error_code> test(uint32_t flags = DRM_MODE_ATOMIC_TEST_ONLY);

  std::expected<void, std::error_code> commit(uint32_t flags, void* user_data = nullptr);

  ~AtomicRequest();

  AtomicRequest(AtomicRequest&&) noexcept;
  AtomicRequest& operator=(AtomicRequest&&) noexcept;
  AtomicRequest(const AtomicRequest&) = delete;
  AtomicRequest& operator=(const AtomicRequest&) = delete;

 private:
  drmModeAtomicReq* req_{};
  int drm_fd_{-1};
};

}  // namespace drm
