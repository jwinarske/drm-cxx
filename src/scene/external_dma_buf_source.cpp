// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "external_dma_buf_source.hpp"

#include "buffer_source.hpp"
#include "detail/dmabuf_slot.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

namespace drm::scene {

namespace {

bool ext_dmabuf_debug() {
  static const bool enabled = std::getenv("DRM_EXT_DMABUF_DEBUG") != nullptr;
  return enabled;
}

void debug_step(const char* step, int saved_errno = 0) {
  if (!ext_dmabuf_debug()) {
    return;
  }
  if (saved_errno != 0) {
    std::fprintf(stderr, "[drm-cxx] ExternalDmaBufSource::create: %s (errno=%d: %s)\n", step,
                 saved_errno, std::strerror(saved_errno));
  } else {
    std::fprintf(stderr, "[drm-cxx] ExternalDmaBufSource::create: %s\n", step);
  }
}

}  // namespace

drm::expected<std::unique_ptr<ExternalDmaBufSource>, std::error_code> ExternalDmaBufSource::create(
    const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format,
    std::uint64_t modifier, drm::span<const ExternalPlaneInfo> planes,
    std::function<void()> on_release) {
  if (width == 0 || height == 0 || drm_format == 0) {
    debug_step("validate args (width/height/drm_format must be non-zero)");
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  if (planes.empty() || planes.size() > detail::k_max_planes) {
    debug_step("validate plane count");
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  // Any modifier the caller knows is forwarded to
  // drmModeAddFB2WithModifiers; the kernel validates against driver
  // tables and rejects unsupported tilings at AddFB2 time. Producers
  // like VAAPI / V4L2 stateful decoders that allocate in a vendor tiled
  // layout pass that modifier through here verbatim — pre-restricting
  // to LINEAR would lock out every hw-decoded NV12 surface.
  for (const auto& p : planes) {
    if (p.fd < 0 || p.pitch == 0) {
      debug_step("validate plane fields (fd >= 0, pitch != 0)");
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
    }
  }

  const int fd = dev.fd();
  if (fd < 0) {
    debug_step("validate device fd");
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  auto src = std::unique_ptr<ExternalDmaBufSource>(new ExternalDmaBufSource());
  src->fd_ = fd;
  src->format_.drm_fourcc = drm_format;
  src->format_.modifier = modifier;
  src->format_.width = width;
  src->format_.height = height;
  src->on_release_ = std::move(on_release);

  // Dup the caller's fds so our lifetime is independent of theirs, then
  // PRIME-import + AddFB2 into the slot's cached fb_id (see detail/dmabuf_slot).
  // A partial failure leaves the half-built src for the destructor to clean up.
  src->slot_.modifier = modifier;
  if (auto r = detail::dup_planes(src->slot_, planes); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  if (auto r = detail::import_slot(fd, src->slot_, src->format_); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }

  return src;
}

ExternalDmaBufSource::~ExternalDmaBufSource() {
  teardown_kernel_state();
  close_duped_fds();
  fire_on_release_once();
}

drm::expected<AcquiredBuffer, std::error_code> ExternalDmaBufSource::acquire() {
  if (slot_.fb_id == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  AcquiredBuffer acq;
  acq.fb_id = slot_.fb_id;
  acq.opaque = nullptr;
  // Hand back the producer's render-done fence (if any) and clear the slot so a
  // re-acquire without a fresh render doesn't re-submit a stale/empty fence.
  acq.acquire_fence = std::exchange(pending_fence_, std::nullopt);
  return acq;
}

void ExternalDmaBufSource::release(AcquiredBuffer /*acquired*/) noexcept {
  // Single-use semantics: the source represents one upstream Request's
  // worth of pixels. The first time the scene retires the FB we fire
  // on_release so the upstream buffer can be re-queued; subsequent
  // releases are no-ops in case the source is still attached when the
  // scene tears down.
  fire_on_release_once();
}

void ExternalDmaBufSource::on_session_paused() noexcept {
  // No outstanding ioctls; acquire() is driven by the scene's commit
  // path, which won't run while the scene is paused. Kernel state
  // (FB ID, GEM handles) stays as-is until on_session_resumed
  // re-imports against the new fd.
}

drm::expected<void, std::error_code> ExternalDmaBufSource::on_session_resumed(
    const drm::Device& new_dev) {
  const int new_fd = new_dev.fd();
  if (new_fd < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  // The old fd is dead — its FB ID and GEM handles have been reclaimed
  // by the kernel on fd close. Drop our cached copies without ioctls;
  // calling drmModeRmFB / drm_gem_close against the dead fd would at
  // best be a no-op against a recycled fd, at worst hit somebody else's
  // resources. Keep the duped dma-buf fds — the buffer is the same,
  // we just need a fresh import on the new fd.
  slot_.fb_id = 0;
  for (std::size_t i = 0; i < slot_.plane_count; ++i) {
    slot_.planes.at(i).gem_handle = 0;
  }
  fd_ = new_fd;

  // Re-import the duped fds into the new device. On any failure roll back every
  // handle imported so far via teardown_kernel_state() — otherwise the source
  // holds GEM refs on new_fd until destruction and returns EAGAIN from every
  // acquire() (slot_.fb_id == 0). Rollback uses fd_, already pointed at new_fd.
  if (auto r = detail::import_slot(new_fd, slot_, format_); !r) {
    teardown_kernel_state();
    return drm::unexpected<std::error_code>(r.error());
  }
  return {};
}

void ExternalDmaBufSource::teardown_kernel_state() noexcept {
  if (fd_ < 0) {
    return;
  }
  detail::teardown_slot(fd_, slot_);
}

void ExternalDmaBufSource::close_duped_fds() noexcept {
  detail::close_slot_fds(slot_);
}

void ExternalDmaBufSource::fire_on_release_once() noexcept {
  if (on_release_fired_) {
    return;
  }
  on_release_fired_ = true;
  if (on_release_) {
    on_release_();
  }
}

}  // namespace drm::scene