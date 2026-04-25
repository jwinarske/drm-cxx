// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "buffer.hpp"

#include "device.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <gbm.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <system_error>

namespace drm::gbm {

namespace {

std::error_code last_errno_or(std::errc fallback) noexcept {
  if (errno != 0) {
    return {errno, std::system_category()};
  }
  return std::make_error_code(fallback);
}

}  // namespace

Buffer::Buffer(struct gbm_bo* bo, struct gbm_surface* surf) noexcept : bo_(bo), surf_(surf) {
  if (bo_ != nullptr) {
    width_ = gbm_bo_get_width(bo_);
    height_ = gbm_bo_get_height(bo_);
    stride_ = gbm_bo_get_stride(bo_);
    format_ = gbm_bo_get_format(bo_);
    modifier_ = gbm_bo_get_modifier(bo_);
  }
}

Buffer::Buffer(int fd, struct gbm_bo* bo, std::uint32_t fb_id, void* map_ptr, void* map_data,
               std::size_t size_bytes, std::uint32_t width, std::uint32_t height,
               std::uint32_t stride, std::uint32_t format, std::uint64_t modifier) noexcept
    : fd_(fd),
      bo_(bo),
      fb_id_(fb_id),
      map_ptr_(map_ptr),
      map_data_(map_data),
      size_bytes_(size_bytes),
      width_(width),
      height_(height),
      stride_(stride),
      format_(format),
      modifier_(modifier) {}

Buffer::~Buffer() {
  destroy();
}

Buffer::Buffer(Buffer&& other) noexcept
    : fd_(other.fd_),
      bo_(other.bo_),
      surf_(other.surf_),
      fb_id_(other.fb_id_),
      map_ptr_(other.map_ptr_),
      map_data_(other.map_data_),
      size_bytes_(other.size_bytes_),
      width_(other.width_),
      height_(other.height_),
      stride_(other.stride_),
      format_(other.format_),
      modifier_(other.modifier_) {
  other.fd_ = -1;
  other.bo_ = nullptr;
  other.surf_ = nullptr;
  other.fb_id_ = 0;
  other.map_ptr_ = nullptr;
  other.map_data_ = nullptr;
  other.size_bytes_ = 0;
  other.width_ = 0;
  other.height_ = 0;
  other.stride_ = 0;
  other.format_ = 0;
  other.modifier_ = 0;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this != &other) {
    destroy();
    fd_ = other.fd_;
    bo_ = other.bo_;
    surf_ = other.surf_;
    fb_id_ = other.fb_id_;
    map_ptr_ = other.map_ptr_;
    map_data_ = other.map_data_;
    size_bytes_ = other.size_bytes_;
    width_ = other.width_;
    height_ = other.height_;
    stride_ = other.stride_;
    format_ = other.format_;
    modifier_ = other.modifier_;
    other.fd_ = -1;
    other.bo_ = nullptr;
    other.surf_ = nullptr;
    other.fb_id_ = 0;
    other.map_ptr_ = nullptr;
    other.map_data_ = nullptr;
    other.size_bytes_ = 0;
    other.width_ = 0;
    other.height_ = 0;
    other.stride_ = 0;
    other.format_ = 0;
    other.modifier_ = 0;
  }
  return *this;
}

void Buffer::destroy() noexcept {
  if (map_ptr_ != nullptr && bo_ != nullptr) {
    gbm_bo_unmap(bo_, map_data_);
  }
  map_ptr_ = nullptr;
  map_data_ = nullptr;

  if (fb_id_ != 0 && fd_ >= 0) {
    drmModeRmFB(fd_, fb_id_);
  }
  fb_id_ = 0;

  if (bo_ != nullptr && surf_ != nullptr) {
    gbm_surface_release_buffer(surf_, bo_);
  } else if (bo_ != nullptr) {
    gbm_bo_destroy(bo_);
  }
  bo_ = nullptr;
  surf_ = nullptr;

  size_bytes_ = 0;
  width_ = 0;
  height_ = 0;
  stride_ = 0;
  format_ = 0;
  modifier_ = 0;
  fd_ = -1;
}

void Buffer::forget() noexcept {
  // Every kernel-touching operation below (gbm_bo_unmap, drmModeRmFB,
  // gbm_bo_destroy) would go through the now-dead DRM fd. Drop the
  // handles without issuing any of them; the kernel reclaims on fd
  // close.
  bo_ = nullptr;
  surf_ = nullptr;
  fb_id_ = 0;
  map_ptr_ = nullptr;
  map_data_ = nullptr;
  size_bytes_ = 0;
  width_ = 0;
  height_ = 0;
  stride_ = 0;
  format_ = 0;
  modifier_ = 0;
  fd_ = -1;
}

std::uint32_t Buffer::handle() const noexcept {
  if (bo_ == nullptr) {
    return 0;
  }
  return gbm_bo_get_handle(bo_).u32;
}

std::uint8_t* Buffer::data() noexcept {
  return static_cast<std::uint8_t*>(map_ptr_);
}

const std::uint8_t* Buffer::data() const noexcept {
  return static_cast<const std::uint8_t*>(map_ptr_);
}

drm::expected<int, std::error_code> Buffer::fd() const {
  if (bo_ == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  int const dma_fd = gbm_bo_get_fd(bo_);
  if (dma_fd < 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  return dma_fd;
}

drm::expected<Buffer, std::error_code> Buffer::create(const GbmDevice& dev, const Config& cfg) {
  if (cfg.width == 0 || cfg.height == 0 || cfg.drm_format == 0 || cfg.usage == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  struct gbm_device* gdev = dev.raw();
  if (gdev == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  const int drm_fd = gbm_device_get_fd(gdev);
  if (drm_fd < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  errno = 0;
  struct gbm_bo* bo = nullptr;
  if (cfg.modifier.has_value()) {
    const std::uint64_t mod = *cfg.modifier;
    bo = gbm_bo_create_with_modifiers2(gdev, cfg.width, cfg.height, cfg.drm_format, &mod, 1,
                                       cfg.usage);
  } else {
    bo = gbm_bo_create(gdev, cfg.width, cfg.height, cfg.drm_format, cfg.usage);
  }
  if (bo == nullptr) {
    return drm::unexpected<std::error_code>(last_errno_or(std::errc::io_error));
  }

  const std::uint32_t stride = gbm_bo_get_stride(bo);
  const std::uint64_t resolved_modifier = gbm_bo_get_modifier(bo);
  const std::size_t size_bytes = static_cast<std::size_t>(stride) * cfg.height;

  void* map_ptr = nullptr;
  void* map_data = nullptr;
  if (cfg.map_cpu) {
    std::uint32_t map_stride = 0;
    errno = 0;
    map_ptr = gbm_bo_map(bo, 0, 0, cfg.width, cfg.height, GBM_BO_TRANSFER_READ_WRITE, &map_stride,
                         &map_data);
    if (map_ptr == nullptr) {
      const auto ec = last_errno_or(std::errc::io_error);
      gbm_bo_destroy(bo);
      return drm::unexpected<std::error_code>(ec);
    }
  }

  std::uint32_t fb_id = 0;
  if (cfg.add_fb) {
    std::uint32_t handles[4] = {gbm_bo_get_handle(bo).u32, 0, 0, 0};
    std::uint32_t strides[4] = {stride, 0, 0, 0};
    std::uint32_t offsets[4] = {0, 0, 0, 0};
    std::uint64_t modifiers[4] = {resolved_modifier, 0, 0, 0};
    const bool use_modifiers = resolved_modifier != DRM_FORMAT_MOD_INVALID;
    const int rc = drmModeAddFB2WithModifiers(
        drm_fd, cfg.width, cfg.height, cfg.drm_format, handles, strides, offsets,
        use_modifiers ? modifiers : nullptr, &fb_id, use_modifiers ? DRM_MODE_FB_MODIFIERS : 0);
    if (rc != 0) {
      const auto ec = std::error_code(errno != 0 ? errno : EIO, std::system_category());
      if (map_ptr != nullptr) {
        gbm_bo_unmap(bo, map_data);
      }
      gbm_bo_destroy(bo);
      return drm::unexpected<std::error_code>(ec);
    }
  }

  return Buffer{drm_fd,    bo,         fb_id,  map_ptr,        map_data,         size_bytes,
                cfg.width, cfg.height, stride, cfg.drm_format, resolved_modifier};
}

}  // namespace drm::gbm
