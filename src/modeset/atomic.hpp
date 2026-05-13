// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <system_error>

namespace drm {

class Device;

class AtomicRequest {
 public:
  explicit AtomicRequest(const Device& dev);

  // Returns true if the underlying atomic request was allocated successfully.
  [[nodiscard]] bool valid() const noexcept;

  drm::expected<void, std::error_code> add_property(uint32_t object_id, uint32_t property_id,
                                                    uint64_t value);

  drm::expected<void, std::error_code> test(uint32_t flags = DRM_MODE_ATOMIC_TEST_ONLY);

  drm::expected<void, std::error_code> commit(uint32_t flags, void* user_data = nullptr);

  /// Underlying libdrm handle. Exposed for EGL Streams: NVIDIA's
  /// `eglStreamConsumerAcquireAttribEXT` accepts a
  /// `drmModeAtomicReq*` via the `EGL_DRM_ATOMIC_REQUEST_NV`
  /// attribute and submits the commit itself — there is no
  /// equivalent libdrm-side hook. Returns null when the request
  /// failed to allocate.
  [[nodiscard]] drmModeAtomicReq* native_handle() const noexcept { return req_; }

  ~AtomicRequest();

  AtomicRequest(AtomicRequest&& /*other*/) noexcept;
  AtomicRequest& operator=(AtomicRequest&& /*other*/) noexcept;
  AtomicRequest(const AtomicRequest&) = delete;
  AtomicRequest& operator=(const AtomicRequest&) = delete;

 private:
  drmModeAtomicReq* req_{};
  int drm_fd_{-1};
};

}  // namespace drm
