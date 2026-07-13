// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "dmabuf_slot.hpp"

#include "../buffer_source.hpp"  // ExternalPlaneInfo, SourceFormat

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <drm.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>

namespace drm::scene::detail {

namespace {

[[nodiscard]] std::error_code last_errno_or(std::errc fallback) noexcept {
  const int e = errno;
  return {e != 0 ? e : static_cast<int>(fallback), std::system_category()};
}

bool debug_enabled() noexcept {
  static const bool enabled = std::getenv("DRM_EXT_DMABUF_DEBUG") != nullptr;
  return enabled;
}

void debug_step(const char* step, int saved_errno) noexcept {
  if (!debug_enabled()) {
    return;
  }
  if (saved_errno != 0) {
    std::fprintf(stderr, "[drm-cxx] dmabuf_slot: %s (errno=%d: %s)\n", step, saved_errno,
                 std::strerror(saved_errno));
  } else {
    std::fprintf(stderr, "[drm-cxx] dmabuf_slot: %s\n", step);
  }
}

}  // namespace

drm::expected<void, std::error_code> dup_planes(
    DmaBufSlot& slot, drm::span<const ExternalPlaneInfo> planes) noexcept {
  for (std::size_t i = 0; i < planes.size(); ++i) {
    const int duped = ::fcntl(planes[i].fd, F_DUPFD_CLOEXEC, 0);
    if (duped < 0) {
      const auto ec = last_errno_or(std::errc::bad_file_descriptor);
      debug_step("fcntl F_DUPFD_CLOEXEC", ec.value());
      return drm::unexpected<std::error_code>(ec);
    }
    auto& dst = slot.planes.at(i);
    dst.duped_fd = duped;
    dst.offset = planes[i].offset;
    dst.pitch = planes[i].pitch;
    slot.plane_count = i + 1;
  }
  return {};
}

drm::expected<void, std::error_code> import_slot(int fd, DmaBufSlot& slot,
                                                 const SourceFormat& fmt) noexcept {
  // PRIME-import each plane's duped fd into a GEM handle. The kernel dedups
  // identical fds into the same handle, so re-importing a shared fd is correct.
  for (std::size_t i = 0; i < slot.plane_count; ++i) {
    std::uint32_t handle = 0;
    const int rc = drmPrimeFDToHandle(fd, slot.planes.at(i).duped_fd, &handle);
    if (rc != 0 || handle == 0) {
      const auto ec = last_errno_or(std::errc::io_error);
      debug_step("drmPrimeFDToHandle", ec.value());
      return drm::unexpected<std::error_code>(ec);
    }
    slot.planes.at(i).gem_handle = handle;
  }

  std::array<std::uint32_t, k_max_planes> handles{};
  std::array<std::uint32_t, k_max_planes> pitches{};
  std::array<std::uint32_t, k_max_planes> offsets{};
  std::array<std::uint64_t, k_max_planes> modifiers{};
  for (std::size_t i = 0; i < slot.plane_count; ++i) {
    const auto& rec = slot.planes.at(i);
    handles.at(i) = rec.gem_handle;
    pitches.at(i) = rec.pitch;
    offsets.at(i) = rec.offset;
    modifiers.at(i) = slot.modifier;
  }

  // Pass MODIFIERS only when the caller advertised one; forwarding INVALID
  // through DRM_MODE_FB_MODIFIERS is rejected by drivers that never took the
  // ADDFB2_MODIFIERS capability path.
  const bool use_modifiers = slot.modifier != k_mod_invalid;
  const int rc = drmModeAddFB2WithModifiers(fd, fmt.width, fmt.height, fmt.drm_fourcc,
                                            handles.data(), pitches.data(), offsets.data(),
                                            use_modifiers ? modifiers.data() : nullptr, &slot.fb_id,
                                            use_modifiers ? DRM_MODE_FB_MODIFIERS : 0U);
  if (rc != 0 || slot.fb_id == 0) {
    const auto ec = last_errno_or(std::errc::io_error);
    if (debug_enabled()) {
      std::fprintf(stderr,
                   "[drm-cxx] dmabuf_slot: drmModeAddFB2WithModifiers (errno=%d: %s) — "
                   "w=%u h=%u fourcc=0x%08x mod=0x%016lx use_mod=%d planes=%zu\n",
                   ec.value(), std::strerror(ec.value()), fmt.width, fmt.height, fmt.drm_fourcc,
                   static_cast<unsigned long>(slot.modifier), use_modifiers ? 1 : 0,
                   slot.plane_count);
      for (std::size_t i = 0; i < slot.plane_count; ++i) {
        std::fprintf(stderr, "[drm-cxx]   plane[%zu] handle=%u pitch=%u offset=%u\n", i,
                     handles.at(i), pitches.at(i), offsets.at(i));
      }
    }
    return drm::unexpected<std::error_code>(ec);
  }
  return {};
}

void teardown_slot(int fd, DmaBufSlot& slot) noexcept {
  if (fd < 0) {
    return;
  }
  if (slot.fb_id != 0) {
    drmModeRmFB(fd, slot.fb_id);
    slot.fb_id = 0;
  }
  for (std::size_t i = 0; i < slot.plane_count; ++i) {
    auto& rec = slot.planes.at(i);
    if (rec.gem_handle != 0) {
      drm_gem_close gc{};
      gc.handle = rec.gem_handle;
      ::ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
      rec.gem_handle = 0;
    }
  }
}

void close_slot_fds(DmaBufSlot& slot) noexcept {
  for (std::size_t i = 0; i < slot.plane_count; ++i) {
    auto& rec = slot.planes.at(i);
    if (rec.duped_fd >= 0) {
      ::close(rec.duped_fd);
      rec.duped_fd = -1;
    }
  }
  slot.plane_count = 0;
}

}  // namespace drm::scene::detail
