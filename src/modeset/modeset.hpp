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
class AtomicRequest;

namespace modeset {

// Owns the per-connector property IDs and the mode-blob lifetime needed
// to activate a CRTC+Connector pair in an atomic commit. Construct once
// at init with the target mode; call attach() on each atomic commit that
// asserts DRM_MODE_ATOMIC_ALLOW_MODESET (typically just the first).
//
// Plane-only atomic commits leave CRTC.MODE_ID / CRTC.ACTIVE / the
// connector's CRTC_ID unset. Without these the kernel accepts the
// commit but never activates the pipe — the display stays dark and no
// PAGE_FLIP_EVENT fires. attach() fills that gap.
class Modeset {
 public:
  static drm::expected<Modeset, std::error_code> create(const Device& dev, uint32_t crtc_id,
                                                        uint32_t connector_id,
                                                        const drmModeModeInfo& mode);

  ~Modeset();
  Modeset(Modeset&& other) noexcept;
  Modeset& operator=(Modeset&& other) noexcept;
  Modeset(const Modeset&) = delete;
  Modeset& operator=(const Modeset&) = delete;

  // Add MODE_ID, ACTIVE=1, and CRTC_ID(on connector) to the request.
  // Call on the first atomic commit of the session and whenever the
  // mode changes.
  [[nodiscard]] drm::expected<void, std::error_code> attach(AtomicRequest& req) const;

  // Swap to a new mode (allocates a new blob, frees the old).
  [[nodiscard]] drm::expected<void, std::error_code> set_mode(const drmModeModeInfo& mode);

  [[nodiscard]] uint32_t crtc_id() const noexcept { return crtc_id_; }
  [[nodiscard]] uint32_t connector_id() const noexcept { return connector_id_; }

 private:
  Modeset() = default;
  void release() noexcept;

  int fd_{-1};
  uint32_t crtc_id_{0};
  uint32_t connector_id_{0};
  uint32_t prop_mode_id_{0};         // CRTC property "MODE_ID"
  uint32_t prop_active_{0};          // CRTC property "ACTIVE"
  uint32_t prop_connector_crtc_{0};  // Connector property "CRTC_ID"
  uint32_t mode_blob_id_{0};         // KMS property blob for the mode
};

}  // namespace modeset
}  // namespace drm
