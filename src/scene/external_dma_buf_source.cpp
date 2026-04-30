// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "external_dma_buf_source.hpp"

#include "buffer_source.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace drm::scene {

namespace {

constexpr std::uint64_t k_mod_invalid = (1ULL << 56U) - 1U;  // DRM_FORMAT_MOD_INVALID

[[nodiscard]] bool modifier_is_linear(std::uint64_t modifier) noexcept {
  return modifier == DRM_FORMAT_MOD_LINEAR || modifier == k_mod_invalid;
}

[[nodiscard]] std::error_code last_errno_or(std::errc fallback) noexcept {
  const int e = errno;
  return {e != 0 ? e : static_cast<int>(fallback), std::system_category()};
}

}  // namespace

drm::expected<std::unique_ptr<ExternalDmaBufSource>, std::error_code> ExternalDmaBufSource::create(
    const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format,
    std::uint64_t modifier, drm::span<const ExternalPlaneInfo> planes,
    std::function<void()> on_release) {
  if (width == 0 || height == 0 || drm_format == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  if (planes.empty() || planes.size() > k_max_planes) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  // PR-A scope: single plane, linear-or-invalid modifier. Multi-plane
  // and tiled layouts ship in PR-B; reject explicitly so callers don't
  // silently get partial behavior.
  if (planes.size() != 1) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::operation_not_supported));
  }
  if (!modifier_is_linear(modifier)) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::operation_not_supported));
  }
  for (const auto& p : planes) {
    if (p.fd < 0 || p.pitch == 0) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
    }
  }

  const int fd = dev.fd();
  if (fd < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  auto src = std::unique_ptr<ExternalDmaBufSource>(new ExternalDmaBufSource());
  src->fd_ = fd;
  src->plane_count_ = planes.size();
  src->format_.drm_fourcc = drm_format;
  src->format_.modifier = modifier;
  src->format_.width = width;
  src->format_.height = height;
  src->on_release_ = std::move(on_release);

  // Step 1: dup the caller's fds so our lifetime is independent of theirs.
  for (std::size_t i = 0; i < planes.size(); ++i) {
    const int duped = ::fcntl(planes[i].fd, F_DUPFD_CLOEXEC, 0);
    if (duped < 0) {
      const auto ec = last_errno_or(std::errc::bad_file_descriptor);
      return drm::unexpected<std::error_code>(ec);
    }
    auto& dst = src->planes_.at(i);
    dst.duped_fd = duped;
    dst.offset = planes[i].offset;
    dst.pitch = planes[i].pitch;
  }

  // Step 2: drmPrimeFDToHandle for each plane. The kernel deduplicates
  // identical fds into the same GEM handle automatically (per-fd handle
  // table), so importing the same fd twice for two planes is correct.
  for (std::size_t i = 0; i < src->plane_count_; ++i) {
    auto& rec = src->planes_.at(i);
    std::uint32_t handle = 0;
    const int rc = drmPrimeFDToHandle(fd, rec.duped_fd, &handle);
    if (rc != 0 || handle == 0) {
      const auto ec = last_errno_or(std::errc::io_error);
      return drm::unexpected<std::error_code>(ec);
    }
    rec.gem_handle = handle;
  }

  // Step 3: drmModeAddFB2WithModifiers. Pass MODIFIERS only when the
  // caller advertised one — passing INVALID through DRM_MODE_FB_MODIFIERS
  // would be rejected by drivers that haven't taken the ADDFB2_MODIFIERS
  // capability path.
  std::array<std::uint32_t, k_max_planes> handles{};
  std::array<std::uint32_t, k_max_planes> pitches{};
  std::array<std::uint32_t, k_max_planes> offsets{};
  std::array<std::uint64_t, k_max_planes> modifiers{};
  for (std::size_t i = 0; i < src->plane_count_; ++i) {
    const auto& rec = src->planes_.at(i);
    handles.at(i) = rec.gem_handle;
    pitches.at(i) = rec.pitch;
    offsets.at(i) = rec.offset;
    modifiers.at(i) = modifier;
  }

  const bool use_modifiers = modifier != k_mod_invalid;
  const int rc =
      drmModeAddFB2WithModifiers(fd, width, height, drm_format, handles.data(), pitches.data(),
                                 offsets.data(), use_modifiers ? modifiers.data() : nullptr,
                                 &src->fb_id_, use_modifiers ? DRM_MODE_FB_MODIFIERS : 0U);
  if (rc != 0 || src->fb_id_ == 0) {
    const auto ec = last_errno_or(std::errc::io_error);
    return drm::unexpected<std::error_code>(ec);
  }

  return src;
}

ExternalDmaBufSource::~ExternalDmaBufSource() {
  teardown_kernel_state();
  close_duped_fds();
  fire_on_release_once();
}

drm::expected<AcquiredBuffer, std::error_code> ExternalDmaBufSource::acquire() {
  if (fb_id_ == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  AcquiredBuffer acq;
  acq.fb_id = fb_id_;
  acq.acquire_fence_fd = -1;
  acq.opaque = nullptr;
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
  fb_id_ = 0;
  for (std::size_t i = 0; i < plane_count_; ++i) {
    planes_.at(i).gem_handle = 0;
  }
  fd_ = new_fd;

  // Re-import each duped fd into the new device.
  for (std::size_t i = 0; i < plane_count_; ++i) {
    auto& rec = planes_.at(i);
    std::uint32_t handle = 0;
    const int rc = drmPrimeFDToHandle(new_fd, rec.duped_fd, &handle);
    if (rc != 0 || handle == 0) {
      const auto ec = last_errno_or(std::errc::io_error);
      return drm::unexpected<std::error_code>(ec);
    }
    rec.gem_handle = handle;
  }

  // Re-add the FB.
  std::array<std::uint32_t, k_max_planes> handles{};
  std::array<std::uint32_t, k_max_planes> pitches{};
  std::array<std::uint32_t, k_max_planes> offsets{};
  std::array<std::uint64_t, k_max_planes> modifiers{};
  for (std::size_t i = 0; i < plane_count_; ++i) {
    const auto& rec = planes_.at(i);
    handles.at(i) = rec.gem_handle;
    pitches.at(i) = rec.pitch;
    offsets.at(i) = rec.offset;
    modifiers.at(i) = format_.modifier;
  }
  const bool use_modifiers = format_.modifier != k_mod_invalid;
  const int rc = drmModeAddFB2WithModifiers(
      new_fd, format_.width, format_.height, format_.drm_fourcc, handles.data(), pitches.data(),
      offsets.data(), use_modifiers ? modifiers.data() : nullptr, &fb_id_,
      use_modifiers ? DRM_MODE_FB_MODIFIERS : 0U);
  if (rc != 0 || fb_id_ == 0) {
    const auto ec = last_errno_or(std::errc::io_error);
    return drm::unexpected<std::error_code>(ec);
  }
  return {};
}

void ExternalDmaBufSource::teardown_kernel_state() noexcept {
  if (fd_ < 0) {
    return;
  }
  if (fb_id_ != 0) {
    drmModeRmFB(fd_, fb_id_);
    fb_id_ = 0;
  }
  for (std::size_t i = 0; i < plane_count_; ++i) {
    auto& rec = planes_.at(i);
    if (rec.gem_handle != 0) {
      drm_gem_close gc{};
      gc.handle = rec.gem_handle;
      ::ioctl(fd_, DRM_IOCTL_GEM_CLOSE, &gc);
      rec.gem_handle = 0;
    }
  }
}

void ExternalDmaBufSource::close_duped_fds() noexcept {
  for (std::size_t i = 0; i < plane_count_; ++i) {
    auto& rec = planes_.at(i);
    if (rec.duped_fd >= 0) {
      ::close(rec.duped_fd);
      rec.duped_fd = -1;
    }
  }
  plane_count_ = 0;
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