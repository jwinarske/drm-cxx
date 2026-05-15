// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "libcamera_nv12_source.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/scene/buffer_source.hpp>

#include <drm.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>

namespace drm::examples::camera {

namespace {

constexpr std::uint64_t k_mod_invalid = (1ULL << 56U) - 1U;  // DRM_FORMAT_MOD_INVALID

bool zerocopy_debug() {
  static const bool enabled = std::getenv("DRM_LIBCAMERA_NV12_DEBUG") != nullptr;
  return enabled;
}

void debug_step(const char* step, int saved_errno = 0) {
  if (!zerocopy_debug()) {
    return;
  }
  if (saved_errno != 0) {
    std::fprintf(stderr, "[libcamera_nv12_source] %s (errno=%d: %s)\n", step, saved_errno,
                 std::strerror(saved_errno));
  } else {
    std::fprintf(stderr, "[libcamera_nv12_source] %s\n", step);
  }
}

}  // namespace

std::unique_ptr<LibcameraNv12Source> LibcameraNv12Source::create(
    const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_fourcc,
    std::uint64_t modifier, NV12PlaneInfo y, NV12PlaneInfo uv) noexcept {
  if (width == 0 || height == 0 || drm_fourcc == 0 || dev.fd() < 0) {
    return nullptr;
  }
  if (y.pitch == 0 || uv.pitch == 0) {
    return nullptr;
  }
  // OOM at example-level allocations is fatal-by-design; matches the
  // pattern used by the library's own ExternalDmaBufSource::create.
  // NOLINTNEXTLINE(bugprone-unhandled-exception-at-new)
  auto src = std::unique_ptr<LibcameraNv12Source>(new LibcameraNv12Source());
  src->drm_fd_ = dev.fd();
  src->fmt_.drm_fourcc = drm_fourcc;
  src->fmt_.modifier = modifier;
  src->fmt_.width = width;
  src->fmt_.height = height;
  src->y_ = y;
  src->uv_ = uv;
  return src;
}

LibcameraNv12Source::~LibcameraNv12Source() {
  destroy_state();
}

void LibcameraNv12Source::destroy_state() noexcept {
  if (drm_fd_ < 0) {
    return;
  }
  for (auto& [_, e] : cache_) {
    if (e.fb_id != 0) {
      (void)drmModeRmFB(drm_fd_, e.fb_id);
      e.fb_id = 0;
    }
    if (e.gem_handle != 0) {
      drm_gem_close req{};
      req.handle = e.gem_handle;
      (void)::ioctl(drm_fd_, DRM_IOCTL_GEM_CLOSE, &req);
      e.gem_handle = 0;
    }
    if (e.duped_fd >= 0) {
      ::close(e.duped_fd);
      e.duped_fd = -1;
    }
  }
  cache_.clear();
  current_fb_id_ = 0;
  drm_fd_ = -1;
}

bool LibcameraNv12Source::register_fd(int fd) noexcept {
  if (drm_fd_ < 0 || fd < 0) {
    return false;
  }
  if (cache_.find(fd) != cache_.end()) {
    return true;
  }

  FbEntry e{};
  e.duped_fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
  if (e.duped_fd < 0) {
    debug_step("fcntl F_DUPFD_CLOEXEC", errno);
    return false;
  }

  std::uint32_t handle = 0;
  if (drmPrimeFDToHandle(drm_fd_, e.duped_fd, &handle) != 0 || handle == 0) {
    debug_step("drmPrimeFDToHandle", errno);
    ::close(e.duped_fd);
    return false;
  }
  e.gem_handle = handle;

  // NV12 = 2 planes; both reference the same imported BO handle.
  std::array<std::uint32_t, 4> handles{handle, handle, 0, 0};
  std::array<std::uint32_t, 4> pitches{y_.pitch, uv_.pitch, 0, 0};
  std::array<std::uint32_t, 4> offsets{y_.offset, uv_.offset, 0, 0};
  std::array<std::uint64_t, 4> modifiers{fmt_.modifier, fmt_.modifier, 0, 0};

  const bool use_modifiers = fmt_.modifier != k_mod_invalid;
  if (drmModeAddFB2WithModifiers(drm_fd_, fmt_.width, fmt_.height, fmt_.drm_fourcc, handles.data(),
                                 pitches.data(), offsets.data(),
                                 use_modifiers ? modifiers.data() : nullptr, &e.fb_id,
                                 use_modifiers ? DRM_MODE_FB_MODIFIERS : 0U) != 0 ||
      e.fb_id == 0) {
    const int saved = errno;
    debug_step("drmModeAddFB2WithModifiers", saved);
    drm_gem_close req{};
    req.handle = e.gem_handle;
    (void)::ioctl(drm_fd_, DRM_IOCTL_GEM_CLOSE, &req);
    ::close(e.duped_fd);
    return false;
  }

  if (zerocopy_debug()) {
    std::fprintf(stderr, "[libcamera_nv12_source] minted fb_id=%u for fd=%d (handle=%u)\n", e.fb_id,
                 fd, e.gem_handle);
  }

  // Leave current_fb_id_ at 0 until drain_slot calls set_current_fd
  // after the first libcamera request completes. Pre-seeding it here
  // (so an early acquire returned a valid FB instead of EAGAIN) made
  // the slot scan out the freshly-allocated, zero-filled libcamera
  // buffer for a frame or two — and NV12 with Y=0 / UV=0 reads as
  // BT.601 green, so the viewfinder flashed a green tile before the
  // first real frame. LayerScene's commit path already treats
  // `resource_unavailable_try_again` as "skip this layer this
  // vblank", so the layer is simply absent until pixels arrive.
  cache_.emplace(fd, e);
  return true;
}

bool LibcameraNv12Source::set_current_fd(int fd) noexcept {
  const auto it = cache_.find(fd);
  if (it == cache_.end() || it->second.fb_id == 0) {
    return false;
  }
  current_fb_id_ = it->second.fb_id;
  return true;
}

drm::expected<drm::scene::AcquiredBuffer, std::error_code> LibcameraNv12Source::acquire() {
  if (current_fb_id_ == 0) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  }
  drm::scene::AcquiredBuffer out{};
  out.fb_id = current_fb_id_;
  out.opaque = nullptr;
  return out;
}

void LibcameraNv12Source::release(drm::scene::AcquiredBuffer /*acquired*/) noexcept {
  // libcamera owns the buffer lifecycle (it's still queued / re-queueable
  // until requestCompleted fires). The example re-queues the libcamera
  // Request in drain_slot when scene release fires.
}

drm::scene::BindingModel LibcameraNv12Source::binding_model() const noexcept {
  return drm::scene::BindingModel::SceneSubmitsFbId;
}

drm::scene::SourceFormat LibcameraNv12Source::format() const noexcept {
  return fmt_;
}

}  // namespace drm::examples::camera
