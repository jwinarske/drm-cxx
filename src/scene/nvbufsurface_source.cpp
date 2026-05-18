// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "nvbufsurface_source.hpp"

#if DRM_CXX_HAS_NVBUFSURFACE

#include "../core/device.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>

#include <nvbufsurface.h>

namespace drm::scene {

namespace {

// DRM fourcc → NvBufSurfaceColorFormat. The reverse mapping is in the
// Impl::format_ population; we keep one table to avoid drift. Pre-mul
// alpha doesn't apply at allocation; NvBufSurface treats every 32-bpp
// ARGB-or-similar layout as straight-channel storage. The byte order
// is identical to DRM's little-endian word convention on aarch64.
struct FormatMapping {
  std::uint32_t drm_fourcc;
  NvBufSurfaceColorFormat nvbuf;
};
constexpr FormatMapping k_format_map[] = {
    {DRM_FORMAT_XRGB8888, NVBUF_COLOR_FORMAT_BGRx},
    {DRM_FORMAT_ARGB8888, NVBUF_COLOR_FORMAT_BGRA},
    {DRM_FORMAT_RGBA8888, NVBUF_COLOR_FORMAT_ABGR},
    {DRM_FORMAT_ABGR8888, NVBUF_COLOR_FORMAT_RGBA},
};

[[nodiscard]] NvBufSurfaceColorFormat drm_to_nvbuf(std::uint32_t fourcc) noexcept {
  for (const auto& m : k_format_map) {
    if (m.drm_fourcc == fourcc) {
      return m.nvbuf;
    }
  }
  return NVBUF_COLOR_FORMAT_INVALID;
}

}  // namespace

// Pimpl: keeps the NvBufSurface* and the imported DRM handles together
// so destruction order is deterministic.
struct NvBufSurfaceSource::Impl {
  NvBufSurface* surf{nullptr};
  // The dma-buf fd that backs the NvBufSurface allocation. Owned by the
  // surface itself; we never close it.
  int dmabuf_fd{-1};
  // DRM-side handles. Owned by us — destroyed in ~Impl.
  int drm_fd{-1};
  std::uint32_t gem_handle{0};
  std::uint32_t fb_id{0};
  // Cached mapping. NvBufSurfaceMap is called once at create() time and
  // held for the source's lifetime; map(MapAccess) reuses this pointer
  // and only updates sync state.
  std::uint8_t* mapped_ptr{nullptr};
  std::uint32_t pitch{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t data_size{0};
  std::uint32_t drm_fourcc{0};
  // Per-mapping sync state. Set when map() hands out a BufferMapping
  // with write access; cleared when the unmap callback fires. Lets us
  // bookend SyncForDevice without holding extra state in BufferMapping.
  bool needs_device_sync{false};

  ~Impl() {
    if (fb_id != 0 && drm_fd >= 0) {
      drmModeRmFB(drm_fd, fb_id);
    }
    if (gem_handle != 0 && drm_fd >= 0) {
      drm_gem_close gc{};
      gc.handle = gem_handle;
      drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gc);
    }
    if (surf != nullptr) {
      // UnMap is best-effort: if Map was never called this is a no-op
      // on the driver side. The surface destructor handles closing the
      // dma-buf fd.
      (void)NvBufSurfaceUnMap(surf, 0, 0);
      NvBufSurfaceDestroy(surf);
    }
  }
};

NvBufSurfaceSource::NvBufSurfaceSource(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

NvBufSurfaceSource::~NvBufSurfaceSource() = default;

drm::expected<std::unique_ptr<NvBufSurfaceSource>, std::error_code> NvBufSurfaceSource::create(
    const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format) {
  if (width == 0U || height == 0U) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  const NvBufSurfaceColorFormat nvbuf_fmt = drm_to_nvbuf(drm_format);
  if (nvbuf_fmt == NVBUF_COLOR_FORMAT_INVALID) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_supported));
  }

  NvBufSurfaceCreateParams params{};
  params.gpuId = 0;
  params.width = width;
  params.height = height;
  params.size = 0;
  params.isContiguous = false;
  params.colorFormat = nvbuf_fmt;
  params.layout = NVBUF_LAYOUT_PITCH;
  params.memType = NVBUF_MEM_SURFACE_ARRAY;

  NvBufSurface* surf = nullptr;
  if (NvBufSurfaceCreate(&surf, 1, &params) != 0 || surf == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_enough_memory));
  }
  // Build the impl up-front so any error along the way frees the
  // surface via Impl's destructor.
  auto impl = std::make_unique<Impl>();
  impl->surf = surf;
  impl->drm_fd = dev.fd();
  const auto& sp = surf->surfaceList[0];
  impl->dmabuf_fd = static_cast<int>(sp.bufferDesc);
  impl->pitch = sp.pitch;
  impl->width = sp.width;
  impl->height = sp.height;
  impl->data_size = sp.dataSize;
  impl->drm_fourcc = drm_format;
  if (impl->dmabuf_fd < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::no_such_device));
  }

  // Establish the cached CPU mapping once; subsequent map() calls just
  // hand out the cached pointer with the right sync bookend.
  if (NvBufSurfaceMap(surf, 0, 0, NVBUF_MAP_READ_WRITE) != 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::io_error));
  }
  impl->mapped_ptr = static_cast<std::uint8_t*>(sp.mappedAddr.addr[0]);
  if (impl->mapped_ptr == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::io_error));
  }

  // PRIME-import to DRM. drmPrimeFDToHandle duplicates the kernel-side
  // handle; we keep the resulting GEM handle as the lifetime owner of
  // the FB-side reference. The original dma-buf fd stays with the
  // NvBufSurface.
  if (drmPrimeFDToHandle(impl->drm_fd, impl->dmabuf_fd, &impl->gem_handle) != 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  std::uint32_t handles[4] = {impl->gem_handle, 0, 0, 0};
  std::uint32_t pitches[4] = {impl->pitch, 0, 0, 0};
  std::uint32_t offsets[4] = {0, 0, 0, 0};
  if (drmModeAddFB2(impl->drm_fd, impl->width, impl->height, drm_format, handles, pitches, offsets,
                    &impl->fb_id, 0) != 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  // Populate SourceFormat for the allocator. Modifier stays LINEAR
  // because NVBUF_LAYOUT_PITCH is what we asked for above; if a future
  // caller wants block-linear, the modifier has to flow through too.
  auto src = std::unique_ptr<NvBufSurfaceSource>(new NvBufSurfaceSource(std::move(impl)));
  src->format_.width = width;
  src->format_.height = height;
  src->format_.drm_fourcc = drm_format;
  src->format_.modifier = DRM_FORMAT_MOD_LINEAR;
  return src;
}

drm::expected<AcquiredBuffer, std::error_code> NvBufSurfaceSource::acquire() {
  if (!impl_ || impl_->fb_id == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  AcquiredBuffer out{};
  out.fb_id = impl_->fb_id;
  return out;
}

void NvBufSurfaceSource::release(AcquiredBuffer /*acquired*/) noexcept {
  // Single-buffer source: nothing to hand back.
}

extern "C" void nvbuf_unmap_sync_device(void* ctx) noexcept {
  auto* impl = static_cast<NvBufSurfaceSource::Impl*>(ctx);
  if (impl == nullptr || impl->surf == nullptr) {
    return;
  }
  if (impl->needs_device_sync) {
    (void)NvBufSurfaceSyncForDevice(impl->surf, 0, 0);
    impl->needs_device_sync = false;
  }
}

drm::expected<drm::BufferMapping, std::error_code> NvBufSurfaceSource::map(drm::MapAccess access) {
  if (!impl_ || impl_->mapped_ptr == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_supported));
  }
  // Invalidate CPU caches before any read so we observe whatever the
  // device may have written between commits. Writes don't need this —
  // they overwrite the cache anyway.
  if (access == drm::MapAccess::Read || access == drm::MapAccess::ReadWrite) {
    (void)NvBufSurfaceSyncForCpu(impl_->surf, 0, 0);
  }
  if (access == drm::MapAccess::Write || access == drm::MapAccess::ReadWrite) {
    impl_->needs_device_sync = true;
  }
  return drm::BufferMapping(impl_->mapped_ptr, impl_->data_size, impl_->pitch, impl_->width,
                            impl_->height, access, &nvbuf_unmap_sync_device, impl_.get());
}

void NvBufSurfaceSource::on_session_paused() noexcept {
  // libseat revoked the DRM fd; drop the FB ID + GEM handle without
  // touching the (now-invalid) fd. The NvBufSurface stays mapped — it
  // belongs to NVMM, not DRM, and survives the seat pause.
  if (!impl_) {
    return;
  }
  impl_->fb_id = 0;
  impl_->gem_handle = 0;
  impl_->drm_fd = -1;
}

drm::expected<void, std::error_code> NvBufSurfaceSource::on_session_resumed(
    const drm::Device& new_dev) {
  if (!impl_) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  impl_->drm_fd = new_dev.fd();
  if (drmPrimeFDToHandle(impl_->drm_fd, impl_->dmabuf_fd, &impl_->gem_handle) != 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  std::uint32_t handles[4] = {impl_->gem_handle, 0, 0, 0};
  std::uint32_t pitches[4] = {impl_->pitch, 0, 0, 0};
  std::uint32_t offsets[4] = {0, 0, 0, 0};
  if (drmModeAddFB2(impl_->drm_fd, impl_->width, impl_->height, impl_->drm_fourcc, handles, pitches,
                    offsets, &impl_->fb_id, 0) != 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  return {};
}

}  // namespace drm::scene

#endif  // DRM_CXX_HAS_NVBUFSURFACE
