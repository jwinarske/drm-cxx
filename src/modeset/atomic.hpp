// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <system_error>
#include <vector>

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

  /// Log every (object, property, value) this request carries, resolving
  /// property ids to names. No-op unless DRM_ATOMIC_DEBUG or DRM_ALLOC_DEBUG
  /// is set, and nothing is recorded when they are not.
  ///
  /// An atomic TEST that returns EINVAL says only that the kernel disliked
  /// *something*. libdrm keeps the request opaque, so without this the next
  /// step is bisecting a property set by hand against a driver that accepts
  /// the same plane under a smaller one. @p why labels the dump.
  void dump(const char* why) const;

  ~AtomicRequest();

  AtomicRequest(AtomicRequest&& /*other*/) noexcept;
  AtomicRequest& operator=(AtomicRequest&& /*other*/) noexcept;
  AtomicRequest(const AtomicRequest&) = delete;
  AtomicRequest& operator=(const AtomicRequest&) = delete;

 private:
  struct Entry {
    uint32_t object_id;
    uint32_t property_id;
    uint64_t value;
  };

  drmModeAtomicReq* req_{};
  int drm_fd_{-1};
  // Populated only while debugging; see dump().
  std::vector<Entry> trace_;
};

}  // namespace drm
