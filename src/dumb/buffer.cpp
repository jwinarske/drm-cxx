// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "buffer.hpp"

#include "../core/device.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>  // NOLINT(misc-include-cleaner) — canonical home of off_t
#include <system_error>

namespace drm::dumb {

namespace {

std::error_code last_errno() noexcept {
  return {errno, std::system_category()};
}

// Result of CREATE_DUMB + MAP_DUMB + mmap. AddFB2 is the caller's
// responsibility (different per single- vs multi-plane FB layout).
struct CreateMapped {
  std::uint32_t gem_handle{0};
  std::uint32_t stride{0};
  std::size_t size_bytes{0};
  std::uint8_t* mapped{nullptr};
};

[[nodiscard]] drm::expected<CreateMapped, std::error_code> create_and_map_dumb(int fd,
                                                                               std::uint32_t width,
                                                                               std::uint32_t height,
                                                                               std::uint32_t bpp) {
  drm_mode_create_dumb create{};
  create.width = width;
  create.height = height;
  create.bpp = bpp;
  if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
    return drm::unexpected<std::error_code>(last_errno());
  }
  CreateMapped out{};
  out.gem_handle = create.handle;
  out.stride = create.pitch;
  out.size_bytes = create.size;

  drm_mode_map_dumb map_req{};
  map_req.handle = out.gem_handle;
  if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
    const auto ec = last_errno();
    drm_mode_destroy_dumb destroy{};
    destroy.handle = out.gem_handle;
    ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    return drm::unexpected<std::error_code>(ec);
  }

  // NOLINTBEGIN(misc-include-cleaner) — off_t arrives via <sys/types.h>
  void* ptr = mmap(nullptr, out.size_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                   static_cast<off_t>(map_req.offset));
  // NOLINTEND(misc-include-cleaner)
  if (ptr == MAP_FAILED) {
    const auto ec = last_errno();
    drm_mode_destroy_dumb destroy{};
    destroy.handle = out.gem_handle;
    ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    return drm::unexpected<std::error_code>(ec);
  }
  out.mapped = static_cast<std::uint8_t*>(ptr);
  std::memset(out.mapped, 0, out.size_bytes);
  return out;
}

// Geometry table for the semi-planar YUV formats `create_planar`
// recognizes. `bpp` is what `DRM_IOCTL_MODE_CREATE_DUMB` wants for
// the Y plane (the kernel computes pitch from this); `extra_rows_*`
// describe the UV plane's height as a fraction of the image height,
// so 4:2:0 (NV12 / P0xx) gives extra_num=1 / extra_den=2 (UV plane
// is half height) and 4:2:2 (NV16 / NV61) gives 1/1 (full height,
// not currently in the table — see deferred list).
struct PlanarGeometry {
  std::uint32_t bpp;
  std::uint32_t extra_rows_num;
  std::uint32_t extra_rows_den;
};

[[nodiscard]] std::optional<PlanarGeometry> planar_geometry(std::uint32_t drm_format) noexcept {
  switch (drm_format) {
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV21:
      // 8-bit semi-planar 4:2:0. Y row = width bytes; UV row =
      // width bytes (Cb/Cr interleaved at half horizontal,
      // covering the same byte count); UV plane is height/2 rows.
      return PlanarGeometry{8, 1, 2};
    case DRM_FORMAT_P010:
    case DRM_FORMAT_P012:
    case DRM_FORMAT_P016:
      // 16-bit semi-planar 4:2:0. Same row arrangement as NV12 but
      // with u16 samples — the bpp difference is what the kernel
      // uses to size the pitch; the UV-plane height ratio is
      // unchanged.
      return PlanarGeometry{16, 1, 2};
    default:
      return std::nullopt;
  }
}

}  // namespace

Buffer::Buffer(int fd, std::uint32_t gem_handle, std::uint32_t fb_id, std::uint8_t* mapped,
               std::size_t size_bytes, std::uint32_t width, std::uint32_t height,
               std::uint32_t stride) noexcept
    : fd_(fd),
      gem_handle_(gem_handle),
      fb_id_(fb_id),
      mapped_(mapped),
      size_bytes_(size_bytes),
      width_(width),
      height_(height),
      stride_(stride) {}

Buffer::~Buffer() {
  destroy();
}

Buffer::Buffer(Buffer&& other) noexcept
    : fd_(other.fd_),
      gem_handle_(other.gem_handle_),
      fb_id_(other.fb_id_),
      mapped_(other.mapped_),
      size_bytes_(other.size_bytes_),
      width_(other.width_),
      height_(other.height_),
      stride_(other.stride_) {
  other.fd_ = -1;
  other.gem_handle_ = 0;
  other.fb_id_ = 0;
  other.mapped_ = nullptr;
  other.size_bytes_ = 0;
  other.width_ = 0;
  other.height_ = 0;
  other.stride_ = 0;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this != &other) {
    destroy();
    fd_ = other.fd_;
    gem_handle_ = other.gem_handle_;
    fb_id_ = other.fb_id_;
    mapped_ = other.mapped_;
    size_bytes_ = other.size_bytes_;
    width_ = other.width_;
    height_ = other.height_;
    stride_ = other.stride_;
    other.fd_ = -1;
    other.gem_handle_ = 0;
    other.fb_id_ = 0;
    other.mapped_ = nullptr;
    other.size_bytes_ = 0;
    other.width_ = 0;
    other.height_ = 0;
    other.stride_ = 0;
  }
  return *this;
}

void Buffer::destroy() noexcept {
  if (fb_id_ != 0 && fd_ >= 0) {
    drmModeRmFB(fd_, fb_id_);
  }
  fb_id_ = 0;

  if (mapped_ != nullptr) {
    munmap(mapped_, size_bytes_);
    mapped_ = nullptr;
  }

  if (gem_handle_ != 0 && fd_ >= 0) {
    drm_mode_destroy_dumb destroy{};
    destroy.handle = gem_handle_;
    ioctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
  }
  gem_handle_ = 0;

  size_bytes_ = 0;
  width_ = 0;
  height_ = 0;
  stride_ = 0;
  fd_ = -1;
}

void Buffer::forget() noexcept {
  // Tear down the CPU mapping — munmap is fd-independent.
  if (mapped_ != nullptr) {
    munmap(mapped_, size_bytes_);
    mapped_ = nullptr;
  }
  // Drop the GEM handle + FB ID without ioctls. The kernel's
  // per-process table reclaims these when the fd is closed, which by
  // definition has already happened if the caller is invoking forget().
  gem_handle_ = 0;
  fb_id_ = 0;
  size_bytes_ = 0;
  width_ = 0;
  height_ = 0;
  stride_ = 0;
  fd_ = -1;
}

drm::BufferMapping Buffer::map(drm::MapAccess access) noexcept {
  // Dumb buffers are mmap'd at create() time onto a kernel-coherent
  // region; the guard is a thin view, no per-scope work to do. A
  // null unmap pointer signals "destructor is a no-op" to BufferMapping.
  if (mapped_ == nullptr || size_bytes_ == 0) {
    return {};
  }
  return {mapped_,           size_bytes_,    stride_, width_, height_, access,
          /*unmap=*/nullptr, /*ctx=*/nullptr};
}

drm::expected<Buffer, std::error_code> Buffer::create(const drm::Device& dev, const Config& cfg) {
  if (cfg.width == 0 || cfg.height == 0 || cfg.drm_format == 0 || cfg.bpp == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  const int fd = dev.fd();
  if (fd < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  auto cm = create_and_map_dumb(fd, cfg.width, cfg.height, cfg.bpp);
  if (!cm) {
    return drm::unexpected<std::error_code>(cm.error());
  }

  std::uint32_t fb_id = 0;
  if (cfg.add_fb) {
    std::array<std::uint32_t, 4> handles{cm->gem_handle, 0, 0, 0};
    std::array<std::uint32_t, 4> strides{cm->stride, 0, 0, 0};
    std::array<std::uint32_t, 4> offsets{};
    if (drmModeAddFB2(fd, cfg.width, cfg.height, cfg.drm_format, handles.data(), strides.data(),
                      offsets.data(), &fb_id, 0) != 0) {
      const auto ec = last_errno();
      munmap(cm->mapped, cm->size_bytes);
      drm_mode_destroy_dumb destroy{};
      destroy.handle = cm->gem_handle;
      ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
      return drm::unexpected<std::error_code>(ec);
    }
  }

  return Buffer{fd,        cm->gem_handle, fb_id,     cm->mapped, cm->size_bytes,
                cfg.width, cfg.height,     cm->stride};
}

drm::expected<Buffer, std::error_code> Buffer::create_planar(const drm::Device& dev,
                                                             std::uint32_t drm_format,
                                                             std::uint32_t width,
                                                             std::uint32_t height) {
  if (width == 0 || height == 0 || drm_format == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  const auto geom = planar_geometry(drm_format);
  if (!geom) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_supported));
  }
  const int fd = dev.fd();
  if (fd < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  // Over-allocate so the UV plane fits in the same linear region.
  // The kernel's CREATE_DUMB sees one tall buffer; AddFB2 below
  // points the chroma plane at the second half via per-plane offset.
  const std::uint32_t total_rows = height + (height * geom->extra_rows_num / geom->extra_rows_den);

  auto cm = create_and_map_dumb(fd, width, total_rows, geom->bpp);
  if (!cm) {
    return drm::unexpected<std::error_code>(cm.error());
  }

  // Multi-plane AddFB2: same gem handle reused for every plane;
  // per-plane pitches identical (both Y and UV rows are
  // `stride` bytes wide for semi-planar 4:2:0); UV plane offset is
  // `stride * height` bytes (the Y plane's footprint, computed
  // against the kernel-reported stride to honor any alignment
  // padding the driver added — amdgpu DC's 256-byte rule applies
  // here, and using the kernel stride means we don't have to know
  // about it).
  const std::uint32_t stride = cm->stride;
  std::array<std::uint32_t, 4> handles{cm->gem_handle, cm->gem_handle, 0, 0};
  std::array<std::uint32_t, 4> pitches{stride, stride, 0, 0};
  std::array<std::uint32_t, 4> offsets{0, stride * height, 0, 0};

  std::uint32_t fb_id = 0;
  if (drmModeAddFB2(fd, width, height, drm_format, handles.data(), pitches.data(), offsets.data(),
                    &fb_id, 0) != 0) {
    const auto ec = last_errno();
    munmap(cm->mapped, cm->size_bytes);
    drm_mode_destroy_dumb destroy{};
    destroy.handle = cm->gem_handle;
    ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    return drm::unexpected<std::error_code>(ec);
  }

  // The Buffer's `width` / `height` track the *image* dimensions,
  // not the over-allocated `total_rows` — consumers reading
  // `height()` see the visible image, matching the AddFB2 framing.
  return Buffer{fd, cm->gem_handle, fb_id, cm->mapped, cm->size_bytes, width, height, cm->stride};
}

}  // namespace drm::dumb
