// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "buffer.hpp"

#include "../core/device.hpp"

#include <drm.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

namespace drm::dumb {

namespace {

std::error_code last_errno() noexcept {
  return {errno, std::system_category()};
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

drm::expected<Buffer, std::error_code> Buffer::create(const drm::Device& dev, const Config& cfg) {
  if (cfg.width == 0 || cfg.height == 0 || cfg.drm_format == 0 || cfg.bpp == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  const int fd = dev.fd();
  if (fd < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  drm_mode_create_dumb create{};
  create.width = cfg.width;
  create.height = cfg.height;
  create.bpp = cfg.bpp;
  if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
    return drm::unexpected<std::error_code>(last_errno());
  }

  const std::uint32_t gem_handle = create.handle;
  const std::uint32_t stride = create.pitch;
  const std::size_t size_bytes = create.size;

  drm_mode_map_dumb map_req{};
  map_req.handle = gem_handle;
  if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
    const auto ec = last_errno();
    drm_mode_destroy_dumb destroy{};
    destroy.handle = gem_handle;
    ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    return drm::unexpected<std::error_code>(ec);
  }

  void* ptr = mmap(nullptr, size_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                   static_cast<off_t>(map_req.offset));
  if (ptr == MAP_FAILED) {
    const auto ec = last_errno();
    drm_mode_destroy_dumb destroy{};
    destroy.handle = gem_handle;
    ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    return drm::unexpected<std::error_code>(ec);
  }
  auto* mapped = static_cast<std::uint8_t*>(ptr);
  std::memset(mapped, 0, size_bytes);

  std::uint32_t fb_id = 0;
  if (cfg.add_fb) {
    std::uint32_t handles[4] = {gem_handle};
    std::uint32_t strides[4] = {stride};
    std::uint32_t offsets[4] = {0};
    if (drmModeAddFB2(fd, cfg.width, cfg.height, cfg.drm_format, handles, strides, offsets, &fb_id,
                      0) != 0) {
      const auto ec = last_errno();
      munmap(mapped, size_bytes);
      drm_mode_destroy_dumb destroy{};
      destroy.handle = gem_handle;
      ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
      return drm::unexpected<std::error_code>(ec);
    }
  }

  return Buffer{fd, gem_handle, fb_id, mapped, size_bytes, cfg.width, cfg.height, stride};
}

}  // namespace drm::dumb
