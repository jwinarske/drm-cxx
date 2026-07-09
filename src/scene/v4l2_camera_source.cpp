// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "v4l2_camera_source.hpp"

#include "buffer_source.hpp"
#include "v4l2_plane_layout.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/dumb/buffer.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <memory>
#include <optional>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace drm::scene {

namespace {

constexpr std::uint32_t k_min_buffers = 2U;
constexpr std::uint32_t k_max_buffers = 32U;

// V4L2-CAPTURE-echo -> DRM plane-layout derivation is shared with the decoder
// source (single-V4L2-plane NV12/P0xx and YUV420/YVU420 splits live there).
using detail::derive_drm_plane_layout;
using detail::DrmPlaneLayout;
using detail::k_drm_max_planes;

constexpr int k_max_ioctl_retries = 8;

[[nodiscard]] int xioctl(int fd, unsigned long request, void* arg) noexcept {
  for (int attempt = 0; attempt < k_max_ioctl_retries; ++attempt) {
    int const r = ::ioctl(fd, request, arg);
    if (r >= 0) {
      return 0;
    }
    if (errno != EINTR) {
      return errno;
    }
  }
  return EINTR;
}

[[nodiscard]] std::error_code make_errno(int err) noexcept {
  return {err, std::generic_category()};
}

[[nodiscard]] std::uint32_t capture_buf_type(bool is_mplane) noexcept {
  return is_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
}

[[nodiscard]] std::error_code validate_config(const char* device_path,
                                              const V4l2CameraConfig& cfg) noexcept {
  if (device_path == nullptr || std::strlen(device_path) == 0U) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (cfg.pixel_fourcc == 0U) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (cfg.width == 0U || cfg.height == 0U) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (cfg.buffer_count < k_min_buffers || cfg.buffer_count > k_max_buffers) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (cfg.pixel_fourcc != V4L2_PIX_FMT_NV12 && cfg.pixel_fourcc != V4L2_PIX_FMT_YUYV) {
    return std::make_error_code(std::errc::not_supported);
  }
  return {};
}

[[nodiscard]] std::uint32_t v4l2_to_drm_fourcc(std::uint32_t v4l2_fourcc) noexcept {
  switch (v4l2_fourcc) {
    case V4L2_PIX_FMT_NV12:
      return DRM_FORMAT_NV12;
    case V4L2_PIX_FMT_YUYV:
      return DRM_FORMAT_YUYV;
    default:
      return 0;
  }
}

[[nodiscard]] bool is_semiplanar_nv12(std::uint32_t drm_fourcc) noexcept {
  return drm_fourcc == DRM_FORMAT_NV12;
}

// Per-CAPTURE-buffer state. RAII-cleaned: the destructor releases the
// per-buffer DRM framebuffer + GEM handles, munmaps the per-plane CPU
// mappings, and closes the per-plane DMA-BUF fds the kernel handed
// back from VIDIOC_EXPBUF. Move-only so the std::vector can grow.
struct V4l2CaptureBuffer {
  std::array<void*, VIDEO_MAX_PLANES> mapped_ptr{};
  std::array<std::size_t, VIDEO_MAX_PLANES> mapped_len{};
  std::array<int, VIDEO_MAX_PLANES> dmabuf_fd{};
  std::uint32_t num_planes{0};

  int drm_fd{-1};
  std::uint32_t fb_id{0};
  std::array<std::uint32_t, k_drm_max_planes> drm_handles{};

  V4l2CaptureBuffer() noexcept { dmabuf_fd.fill(-1); }
  ~V4l2CaptureBuffer() {
    if (drm_fd >= 0) {
      if (fb_id != 0) {
        drmModeRmFB(drm_fd, fb_id);
      }
      for (auto h : drm_handles) {
        if (h != 0) {
          drmCloseBufferHandle(drm_fd, h);
        }
      }
    }
    for (std::uint32_t p = 0; p < num_planes; ++p) {
      if (mapped_ptr.at(p) != nullptr && mapped_ptr.at(p) != MAP_FAILED) {
        ::munmap(mapped_ptr.at(p), mapped_len.at(p));
      }
      if (dmabuf_fd.at(p) >= 0) {
        ::close(dmabuf_fd.at(p));
      }
    }
  }
  V4l2CaptureBuffer(const V4l2CaptureBuffer&) = delete;
  V4l2CaptureBuffer& operator=(const V4l2CaptureBuffer&) = delete;
  V4l2CaptureBuffer(V4l2CaptureBuffer&& o) noexcept
      : mapped_ptr(o.mapped_ptr),
        mapped_len(o.mapped_len),
        dmabuf_fd(o.dmabuf_fd),
        num_planes(o.num_planes),
        drm_fd(o.drm_fd),
        fb_id(o.fb_id),
        drm_handles(o.drm_handles) {
    o.mapped_ptr.fill(nullptr);
    o.dmabuf_fd.fill(-1);
    o.num_planes = 0;
    o.drm_fd = -1;
    o.fb_id = 0;
    o.drm_handles.fill(0);
  }
  V4l2CaptureBuffer& operator=(V4l2CaptureBuffer&&) = delete;
};

[[nodiscard]] std::error_code import_capture_buffer_to_drm(
    int drm_fd, std::uint32_t width, std::uint32_t height, std::uint32_t drm_fourcc,
    std::uint64_t modifier, const DrmPlaneLayout& layout, V4l2CaptureBuffer& slot) noexcept {
  std::array<std::uint32_t, k_drm_max_planes> handles{};
  std::array<std::uint32_t, k_drm_max_planes> pitches{};
  std::array<std::uint32_t, k_drm_max_planes> offsets{};
  std::array<std::uint64_t, k_drm_max_planes> modifiers{};
  for (std::uint32_t p = 0; p < layout.num_drm_planes; ++p) {
    std::uint8_t const v4l2_idx = layout.v4l2_plane_idx.at(p);
    if (v4l2_idx >= slot.num_planes || slot.dmabuf_fd.at(v4l2_idx) < 0) {
      return std::make_error_code(std::errc::invalid_argument);
    }
    std::uint32_t handle = 0;
    if (drmPrimeFDToHandle(drm_fd, slot.dmabuf_fd.at(v4l2_idx), &handle) != 0 || handle == 0) {
      for (std::uint32_t prev = 0; prev < p; ++prev) {
        if (handles.at(prev) != 0) {
          drmCloseBufferHandle(drm_fd, handles.at(prev));
        }
      }
      return make_errno(errno != 0 ? errno : EIO);
    }
    handles.at(p) = handle;
    pitches.at(p) = layout.pitch.at(p);
    offsets.at(p) = layout.offset.at(p);
    modifiers.at(p) = modifier;
  }

  std::uint32_t fb_id = 0;
  if (drmModeAddFB2WithModifiers(drm_fd, width, height, drm_fourcc, handles.data(), pitches.data(),
                                 offsets.data(), modifiers.data(), &fb_id,
                                 DRM_MODE_FB_MODIFIERS) != 0 ||
      fb_id == 0) {
    int const saved = errno;
    for (auto h : handles) {
      if (h != 0) {
        drmCloseBufferHandle(drm_fd, h);
      }
    }
    return make_errno(saved != 0 ? saved : EIO);
  }

  slot.drm_fd = drm_fd;
  slot.fb_id = fb_id;
  for (std::uint32_t p = 0; p < layout.num_drm_planes; ++p) {
    slot.drm_handles.at(p) = handles.at(p);
  }
  return {};
}

void release_queue(int fd, std::uint32_t type) noexcept {
  if (fd < 0) {
    return;
  }
  v4l2_requestbuffers req{};
  req.count = 0;
  req.type = type;
  req.memory = V4L2_MEMORY_MMAP;
  (void)xioctl(fd, VIDIOC_REQBUFS, &req);
}

[[nodiscard]] std::error_code streamon(int fd, std::uint32_t type) noexcept {
  int t = static_cast<int>(type);
  if (int const e = xioctl(fd, VIDIOC_STREAMON, &t); e != 0) {
    return make_errno(e);
  }
  return {};
}

void streamoff(int fd, std::uint32_t type) noexcept {
  if (fd < 0) {
    return;
  }
  int t = static_cast<int>(type);
  (void)xioctl(fd, VIDIOC_STREAMOFF, &t);
}

[[nodiscard]] std::error_code queue_capture_buffer(int fd, bool is_mplane, std::uint32_t index,
                                                   std::uint32_t num_planes) noexcept {
  v4l2_buffer buf{};
  std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
  buf.type = capture_buf_type(is_mplane);
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = index;
  if (is_mplane) {
    buf.length = num_planes;
    buf.m.planes = planes.data();
  }
  if (int const e = xioctl(fd, VIDIOC_QBUF, &buf); e != 0) {
    return make_errno(e);
  }
  return {};
}

// REQBUFS + per-buffer QUERYBUF + per-plane MMAP. EXPBUF is conditional
// — MmapCopy skips it because the dma-buf export is dead weight when
// the buffers are CPU-copied into a dumb-buffer pair.
//
// MPLANE devices report per-plane length / mem_offset in the
// `v4l2_plane` array attached to the buffer; single-plane devices keep
// the per-buffer length / m.offset in the buffer itself.
[[nodiscard]] std::error_code allocate_capture_buffers(int fd, bool is_mplane,
                                                       std::uint32_t fmt_num_planes,
                                                       std::uint32_t requested_count,
                                                       bool want_dmabuf_export,
                                                       std::vector<V4l2CaptureBuffer>& out) {
  v4l2_requestbuffers req{};
  req.count = requested_count;
  req.type = capture_buf_type(is_mplane);
  req.memory = V4L2_MEMORY_MMAP;
  if (int const e = xioctl(fd, VIDIOC_REQBUFS, &req); e != 0) {
    return make_errno(e);
  }
  if (req.count == 0) {
    return std::make_error_code(std::errc::not_enough_memory);
  }

  out.reserve(req.count);
  for (std::uint32_t i = 0; i < req.count; ++i) {
    V4l2CaptureBuffer slot;
    v4l2_buffer buf{};
    std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
    buf.type = req.type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (is_mplane) {
      buf.length = VIDEO_MAX_PLANES;
      buf.m.planes = planes.data();
    }
    if (int const e = xioctl(fd, VIDIOC_QUERYBUF, &buf); e != 0) {
      return make_errno(e);
    }

    slot.num_planes = fmt_num_planes;
    for (std::uint32_t p = 0; p < fmt_num_planes; ++p) {
      std::size_t const length = is_mplane ? planes.at(p).length : buf.length;
      std::uint32_t const offset = is_mplane ? planes.at(p).m.mem_offset : buf.m.offset;
      void* const mapped = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
      if (mapped == MAP_FAILED) {
        slot.num_planes = p;  // only release what we successfully mapped
        out.push_back(std::move(slot));
        return make_errno(errno);
      }
      slot.mapped_ptr.at(p) = mapped;
      slot.mapped_len.at(p) = length;

      if (want_dmabuf_export) {
        v4l2_exportbuffer exp{};
        exp.type = req.type;
        exp.index = i;
        exp.plane = p;
        exp.flags = O_CLOEXEC;
        if (int const e = xioctl(fd, VIDIOC_EXPBUF, &exp); e != 0) {
          slot.num_planes = p + 1;  // current plane is mapped, ensure it's munmap'd
          out.push_back(std::move(slot));
          return make_errno(e);
        }
        slot.dmabuf_fd.at(p) = exp.fd;
      }
    }
    out.push_back(std::move(slot));
  }
  return {};
}

[[nodiscard]] drm::expected<drm::dumb::Buffer, std::error_code> allocate_dumb_for_format(
    const drm::Device& dev, std::uint32_t width, std::uint32_t height,
    std::uint32_t drm_fourcc) noexcept {
  if (is_semiplanar_nv12(drm_fourcc)) {
    return drm::dumb::Buffer::create_planar(dev, drm_fourcc, width, height);
  }
  drm::dumb::Config cfg;
  cfg.width = width;
  cfg.height = height;
  cfg.drm_format = drm_fourcc;
  cfg.bpp = (drm_fourcc == DRM_FORMAT_YUYV) ? 16 : 32;
  cfg.add_fb = true;
  return drm::dumb::Buffer::create(dev, cfg);
}

// Copy the most-recently-DQ'd V4L2 buffer's bytes into a dumb buffer.
// Row-by-row because V4L2's bytesperline and the dumb buffer's stride
// can differ (kernel alignment quirks). Three V4L2 shapes feed in here:
//   * Single V4L2 plane carrying NV12 (Y + interleaved UV via offset
//     math): one mmap, Y at [0..src_pitch * h), UV at [src_pitch * h..).
//   * MPLANE NV12 with num_planes == 2: two mmaps, Y in plane 0, UV in
//     plane 1; each with its own bytesperline (often equal in practice).
//   * Packed YUYV (single plane regardless of API flavor): one mmap.
void copy_v4l2_to_dumb(const V4l2CaptureBuffer& slot, const v4l2_format& fmt_echo, bool is_mplane,
                       std::uint32_t drm_fourcc, drm::dumb::Buffer& dst) noexcept {
  if (dst.empty() || dst.data() == nullptr) {
    return;
  }
  std::uint8_t* dst_bytes = dst.data();
  std::uint32_t const dst_stride = dst.stride();
  std::uint32_t const height = is_mplane ? fmt_echo.fmt.pix_mp.height : fmt_echo.fmt.pix.height;

  auto pitch_of = [&](std::uint32_t plane) -> std::uint32_t {
    if (!is_mplane) {
      return fmt_echo.fmt.pix.bytesperline;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    return fmt_echo.fmt.pix_mp.plane_fmt[plane].bytesperline;
  };

  bool const nv12_split = is_semiplanar_nv12(drm_fourcc) && slot.num_planes >= 2;
  bool const nv12_packed = is_semiplanar_nv12(drm_fourcc) && slot.num_planes == 1;

  // Y plane (or full packed plane for YUYV).
  std::uint32_t const y_pitch = pitch_of(0);
  std::uint32_t const y_row_bytes = std::min(y_pitch, dst_stride);
  const auto* y_src = static_cast<const std::uint8_t*>(slot.mapped_ptr.at(0));
  for (std::uint32_t y = 0; y < height; ++y) {
    std::memcpy(dst_bytes + (static_cast<std::size_t>(y) * dst_stride),
                y_src + (static_cast<std::size_t>(y) * y_pitch), y_row_bytes);
  }

  if (nv12_packed) {
    // UV plane co-located inside the same V4L2 buffer, at offset
    // src_pitch * height. Dumb buffer keeps the UV at dst_stride *
    // height per drm::dumb::Buffer::create_planar.
    std::uint32_t const uv_h = height / 2U;
    const std::uint8_t* src_uv = y_src + (static_cast<std::size_t>(y_pitch) * height);
    std::uint8_t* dst_uv = dst_bytes + (static_cast<std::size_t>(dst_stride) * height);
    for (std::uint32_t y = 0; y < uv_h; ++y) {
      std::memcpy(dst_uv + (static_cast<std::size_t>(y) * dst_stride),
                  src_uv + (static_cast<std::size_t>(y) * y_pitch), y_row_bytes);
    }
  } else if (nv12_split) {
    // UV in its own V4L2 plane (mmap[1]). bytesperline is independent
    // of plane 0 — read it from the format echo.
    std::uint32_t const uv_h = height / 2U;
    std::uint32_t const uv_pitch = pitch_of(1);
    std::uint32_t const uv_row_bytes = std::min(uv_pitch, dst_stride);
    const auto* uv_src = static_cast<const std::uint8_t*>(slot.mapped_ptr.at(1));
    std::uint8_t* dst_uv = dst_bytes + (static_cast<std::size_t>(dst_stride) * height);
    for (std::uint32_t y = 0; y < uv_h; ++y) {
      std::memcpy(dst_uv + (static_cast<std::size_t>(y) * dst_stride),
                  uv_src + (static_cast<std::size_t>(y) * uv_pitch), uv_row_bytes);
    }
  }
}

}  // namespace

struct V4l2CameraSource::Impl {
  int v4l2_fd{-1};
  int drm_fd{-1};
  V4l2CameraConfig cfg{};
  V4l2CameraBufferMode active_mode{V4l2CameraBufferMode::Auto};
  bool is_mplane{false};
  std::uint32_t capture_num_planes{0};
  SourceFormat fmt{};
  v4l2_format format_echo{};
  std::vector<V4l2CaptureBuffer> buffers;
  bool streaming{false};

  // DMABUF-mode latest-frame-wins state. The dequeued slot index is
  // parked here until acquire() claims it. ready_idx is at most one
  // (drive() drops older un-acquired ready frames in favor of fresher
  // ones — see the requeue branch in drive()).
  int ready_idx{-1};
  // Tracks which buffers are currently acquired by the scene (bit
  // set per buffer index). A scene with deferred-release semantics
  // (LayerScene's prev/prev_prev ring) can hold multiple in flight
  // simultaneously, so a single int won't do — release(buf) uses
  // buf.opaque to identify the specific buffer to QBUF, and this
  // bitset guards against double-release. Sized at REQBUFS time to
  // match impl_->buffers.size().
  std::vector<bool> acquired_mask;

  // MMAP-mode double-buffered dumb-buffer state. front_idx selects
  // which dumb buffer scans out; the other is the producer's write
  // target. any_published gates the first acquire() until drive() has
  // copied at least one V4L2 frame in.
  std::array<drm::dumb::Buffer, 2> dumb_pair{};
  std::size_t front_idx{0};
  bool any_published{false};

  // First non-EAGAIN error drive() hit on a requeue (VIDIOC_QBUF). Once
  // set, every subsequent acquire() surfaces it — the ring is past the
  // point where another DQBUF round-trip can fix itself, so the source
  // is effectively dead and the caller needs to tear down and rebuild.
  // EAGAIN is excluded because the queue temporarily refusing a QBUF
  // isn't a terminal condition.
  std::error_code pending_error;
};

V4l2CameraSource::V4l2CameraSource() : impl_(std::make_unique<Impl>()) {}

V4l2CameraSource::~V4l2CameraSource() {
  if (!impl_) {
    return;
  }
  if (impl_->streaming) {
    streamoff(impl_->v4l2_fd, capture_buf_type(impl_->is_mplane));
    impl_->streaming = false;
  }
  impl_->buffers.clear();
  release_queue(impl_->v4l2_fd, capture_buf_type(impl_->is_mplane));
  if (impl_->v4l2_fd >= 0) {
    ::close(impl_->v4l2_fd);
    impl_->v4l2_fd = -1;
  }
}

drm::expected<std::unique_ptr<V4l2CameraSource>, std::error_code> V4l2CameraSource::create(
    const drm::Device& dev, const char* device_path, const V4l2CameraConfig& cfg) {
  if (auto ec = validate_config(device_path, cfg); ec) {
    return drm::unexpected<std::error_code>(ec);
  }
  // The DRM fd is checked just before import (DMABUF mode) / dumb-pair
  // allocation (MMAP mode), not here — so the most-immediate error
  // surfaces first (a bad path or non-V4L2 device produces ENOENT /
  // not_supported, not bad_file_descriptor).
  int const drm_fd = dev.fd();

  int const fd = ::open(device_path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    return drm::unexpected<std::error_code>(make_errno(errno));
  }

  auto fail = [fd](std::error_code ec) {
    ::close(fd);
    return drm::unexpected<std::error_code>(ec);
  };

  v4l2_capability cap{};
  if (int const e = xioctl(fd, VIDIOC_QUERYCAP, &cap); e != 0) {
    return fail(e == ENOTTY ? std::make_error_code(std::errc::not_supported) : make_errno(e));
  }
  std::uint32_t const device_caps =
      ((cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U) ? cap.device_caps : cap.capabilities;
  bool const is_mplane = (device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0U;
  bool const is_single = (device_caps & V4L2_CAP_VIDEO_CAPTURE) != 0U;
  if (!is_mplane && !is_single) {
    return fail(std::make_error_code(std::errc::not_supported));
  }
  if ((device_caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)) != 0U) {
    return fail(std::make_error_code(std::errc::not_supported));
  }
  if ((device_caps & V4L2_CAP_STREAMING) == 0U) {
    return fail(std::make_error_code(std::errc::not_supported));
  }

  v4l2_format fmt{};
  if (is_mplane) {
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.pixelformat = cfg.pixel_fourcc;
    fmt.fmt.pix_mp.width = cfg.width;
    fmt.fmt.pix_mp.height = cfg.height;
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
  } else {
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.pixelformat = cfg.pixel_fourcc;
    fmt.fmt.pix.width = cfg.width;
    fmt.fmt.pix.height = cfg.height;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
  }
  if (int const e = xioctl(fd, VIDIOC_S_FMT, &fmt); e != 0) {
    return fail(make_errno(e));
  }
  std::uint32_t const echoed_fourcc =
      is_mplane ? fmt.fmt.pix_mp.pixelformat : fmt.fmt.pix.pixelformat;
  if (echoed_fourcc != cfg.pixel_fourcc) {
    return fail(std::make_error_code(std::errc::invalid_argument));
  }
  std::uint32_t const w = is_mplane ? fmt.fmt.pix_mp.width : fmt.fmt.pix.width;
  std::uint32_t const h = is_mplane ? fmt.fmt.pix_mp.height : fmt.fmt.pix.height;
  std::uint32_t const cap_num_planes = is_mplane ? fmt.fmt.pix_mp.num_planes : 1U;
  if (cap_num_planes == 0 || cap_num_planes > VIDEO_MAX_PLANES) {
    return fail(std::make_error_code(std::errc::not_supported));
  }
  std::uint32_t const drm_fourcc = v4l2_to_drm_fourcc(cfg.pixel_fourcc);
  if (drm_fourcc == 0) {
    return fail(std::make_error_code(std::errc::not_supported));
  }
  std::uint64_t const modifier = (cfg.modifier == 0) ? DRM_FORMAT_MOD_LINEAR : cfg.modifier;

  // Allocate V4L2 buffers. EXPBUF is conditional on whether we might
  // need dma-buf import: yes for explicit DmaBufZeroCopy and for the
  // Auto probe; skip in explicit MmapCopy.
  bool const try_dmabuf = cfg.mode != V4l2CameraBufferMode::MmapCopy;
  std::vector<V4l2CaptureBuffer> buffers;
  if (auto ec = allocate_capture_buffers(fd, is_mplane, cap_num_planes, cfg.buffer_count,
                                         try_dmabuf, buffers);
      ec) {
    buffers.clear();
    release_queue(fd, capture_buf_type(is_mplane));
    return fail(ec);
  }
  if (buffers.size() < k_min_buffers) {
    buffers.clear();
    release_queue(fd, capture_buf_type(is_mplane));
    return fail(std::make_error_code(std::errc::not_enough_memory));
  }

  // Mode resolution. DmaBufZeroCopy fails on any AddFB2 EINVAL. Auto
  // probes buffer 0 first; on failure drops dma-buf fds and switches
  // to MMAP. MmapCopy never imports.
  V4l2CameraBufferMode resolved_mode = cfg.mode;
  std::array<drm::dumb::Buffer, 2> dumb_pair{};

  auto unwind_v4l2 = [&]() {
    buffers.clear();
    release_queue(fd, capture_buf_type(is_mplane));
  };

  auto alloc_dumb_pair = [&]() -> std::optional<std::error_code> {
    auto a = allocate_dumb_for_format(dev, w, h, drm_fourcc);
    if (!a) {
      return a.error();
    }
    auto b = allocate_dumb_for_format(dev, w, h, drm_fourcc);
    if (!b) {
      return b.error();
    }
    dumb_pair[0] = std::move(*a);
    dumb_pair[1] = std::move(*b);
    return std::nullopt;
  };

  if (cfg.mode == V4l2CameraBufferMode::MmapCopy) {
    if (auto ec = alloc_dumb_pair(); ec.has_value()) {
      unwind_v4l2();
      return fail(*ec);
    }
  } else {
    DrmPlaneLayout layout{};
    if (auto ec = derive_drm_plane_layout(fmt, is_mplane, drm_fourcc, layout); ec) {
      unwind_v4l2();
      return fail(ec);
    }

    if (auto ec =
            import_capture_buffer_to_drm(drm_fd, w, h, drm_fourcc, modifier, layout, buffers.at(0));
        ec) {
      if (cfg.mode == V4l2CameraBufferMode::DmaBufZeroCopy) {
        unwind_v4l2();
        return fail(ec);
      }
      // Auto: drop dma-buf fds (V4L2 mmaps stay), allocate dumb pair.
      for (auto& slot : buffers) {
        for (auto& fd_v : slot.dmabuf_fd) {
          if (fd_v >= 0) {
            ::close(fd_v);
            fd_v = -1;
          }
        }
      }
      if (auto dec = alloc_dumb_pair(); dec.has_value()) {
        unwind_v4l2();
        return fail(*dec);
      }
      resolved_mode = V4l2CameraBufferMode::MmapCopy;
    } else {
      for (std::size_t i = 1; i < buffers.size(); ++i) {
        if (auto ec2 = import_capture_buffer_to_drm(drm_fd, w, h, drm_fourcc, modifier, layout,
                                                    buffers.at(i));
            ec2) {
          unwind_v4l2();
          return fail(ec2);
        }
      }
      resolved_mode = V4l2CameraBufferMode::DmaBufZeroCopy;
    }
  }

  for (std::uint32_t i = 0; i < buffers.size(); ++i) {
    if (auto ec = queue_capture_buffer(fd, is_mplane, i, cap_num_planes); ec) {
      unwind_v4l2();
      return fail(ec);
    }
  }
  if (auto ec = streamon(fd, capture_buf_type(is_mplane)); ec) {
    unwind_v4l2();
    return fail(ec);
  }

  std::unique_ptr<V4l2CameraSource> src(new V4l2CameraSource());
  src->impl_->v4l2_fd = fd;
  src->impl_->drm_fd = drm_fd;
  src->impl_->cfg = cfg;
  src->impl_->active_mode = resolved_mode;
  src->impl_->is_mplane = is_mplane;
  src->impl_->capture_num_planes = cap_num_planes;
  std::uint64_t const advertised_modifier =
      (resolved_mode == V4l2CameraBufferMode::MmapCopy) ? DRM_FORMAT_MOD_LINEAR : modifier;
  src->impl_->fmt = SourceFormat{drm_fourcc, advertised_modifier, w, h};
  src->impl_->format_echo = fmt;
  src->impl_->buffers = std::move(buffers);
  src->impl_->acquired_mask.assign(src->impl_->buffers.size(), false);
  src->impl_->dumb_pair = std::move(dumb_pair);
  src->impl_->streaming = true;
  return src;
}

int V4l2CameraSource::fd() const noexcept {
  return impl_ ? impl_->v4l2_fd : -1;
}

drm::expected<void, std::error_code> V4l2CameraSource::drive() noexcept {
  if (!impl_) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  int const fd = impl_->v4l2_fd;
  bool const is_mplane = impl_->is_mplane;

  if (impl_->active_mode == V4l2CameraBufferMode::DmaBufZeroCopy) {
    while (true) {
      v4l2_buffer buf{};
      std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
      buf.type = capture_buf_type(is_mplane);
      buf.memory = V4L2_MEMORY_MMAP;
      if (is_mplane) {
        buf.length = VIDEO_MAX_PLANES;
        buf.m.planes = planes.data();
      }
      if (int const e = xioctl(fd, VIDIOC_DQBUF, &buf); e != 0) {
        if (e == EAGAIN) {
          break;
        }
        return drm::unexpected<std::error_code>(make_errno(e));
      }
      if (buf.index >= impl_->buffers.size()) {
        continue;
      }
      if (impl_->ready_idx >= 0) {
        if (auto const requeue_ec =
                queue_capture_buffer(fd, is_mplane, static_cast<std::uint32_t>(impl_->ready_idx),
                                     impl_->capture_num_planes);
            requeue_ec && requeue_ec != std::errc::resource_unavailable_try_again &&
            !impl_->pending_error) {
          impl_->pending_error = requeue_ec;
        }
      }
      impl_->ready_idx = static_cast<int>(buf.index);
    }
    return {};
  }

  // MmapCopy
  std::optional<std::uint32_t> latest_idx;
  std::vector<std::uint32_t> dq_indices;
  while (true) {
    v4l2_buffer buf{};
    std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
    buf.type = capture_buf_type(is_mplane);
    buf.memory = V4L2_MEMORY_MMAP;
    if (is_mplane) {
      buf.length = VIDEO_MAX_PLANES;
      buf.m.planes = planes.data();
    }
    if (int const e = xioctl(fd, VIDIOC_DQBUF, &buf); e != 0) {
      if (e == EAGAIN) {
        break;
      }
      return drm::unexpected<std::error_code>(make_errno(e));
    }
    if (buf.index >= impl_->buffers.size()) {
      continue;
    }
    if (latest_idx.has_value()) {
      dq_indices.push_back(latest_idx.value());
    }
    latest_idx = buf.index;
  }
  if (latest_idx.has_value()) {
    auto const& slot = impl_->buffers.at(latest_idx.value());
    auto& back = impl_->dumb_pair.at(1U - impl_->front_idx);
    copy_v4l2_to_dumb(slot, impl_->format_echo, is_mplane, impl_->fmt.drm_fourcc, back);
    impl_->front_idx = 1U - impl_->front_idx;
    impl_->any_published = true;
    dq_indices.push_back(latest_idx.value());
  }
  for (auto idx : dq_indices) {
    if (auto const requeue_ec = queue_capture_buffer(fd, is_mplane, idx, impl_->capture_num_planes);
        requeue_ec && requeue_ec != std::errc::resource_unavailable_try_again &&
        !impl_->pending_error) {
      impl_->pending_error = requeue_ec;
    }
  }
  return {};
}

V4l2CameraBufferMode V4l2CameraSource::active_mode() const noexcept {
  return impl_ ? impl_->active_mode : V4l2CameraBufferMode::Auto;
}

drm::expected<AcquiredBuffer, std::error_code> V4l2CameraSource::acquire() {
  if (!impl_) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  // Sticky error from drive(): once a requeue failed terminally the
  // ring is past self-recovery; surface it on every acquire() so the
  // caller sees a real error code instead of an indefinite EAGAIN
  // stall waiting for frames that will never come.
  if (impl_->pending_error) {
    return drm::unexpected<std::error_code>(impl_->pending_error);
  }

  if (impl_->active_mode == V4l2CameraBufferMode::DmaBufZeroCopy) {
    if (impl_->ready_idx < 0) {
      return drm::unexpected<std::error_code>(
          std::make_error_code(std::errc::resource_unavailable_try_again));
    }
    auto const idx = static_cast<std::uint32_t>(impl_->ready_idx);
    if (idx >= impl_->buffers.size()) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
    }
    std::uint32_t const fb_id = impl_->buffers.at(idx).fb_id;
    if (fb_id == 0) {
      return drm::unexpected<std::error_code>(
          std::make_error_code(std::errc::resource_unavailable_try_again));
    }
    impl_->acquired_mask.at(idx) = true;
    impl_->ready_idx = -1;
    AcquiredBuffer acq;
    acq.fb_id = fb_id;
    // Encode the V4L2 buffer index in opaque so release() can QBUF
    // the specific buffer the caller is returning. Offset by 1 so
    // index 0 is distinguishable from a default-constructed
    // (nullptr) opaque.
    // NOLINTNEXTLINE(performance-no-int-to-ptr) — opaque cookie, not a real address.
    acq.opaque = reinterpret_cast<void*>(static_cast<std::uintptr_t>(idx) + 1U);
    return acq;
  }

  // MmapCopy
  if (!impl_->any_published) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  }
  auto const& front = impl_->dumb_pair.at(impl_->front_idx);
  if (front.empty() || front.fb_id() == 0) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  }
  AcquiredBuffer acq;
  acq.fb_id = front.fb_id();
  acq.opaque = nullptr;
  return acq;
}

void V4l2CameraSource::release(AcquiredBuffer acquired) noexcept {
  if (!impl_) {
    return;
  }
  if (impl_->active_mode == V4l2CameraBufferMode::DmaBufZeroCopy) {
    // Decode the V4L2 buffer index encoded by acquire() in opaque
    // (index + 1; nullptr means "no buffer"). Defensive against
    // double-release and out-of-range opaque values.
    auto const raw = reinterpret_cast<std::uintptr_t>(acquired.opaque);
    if (raw == 0) {
      return;
    }
    auto const idx = static_cast<std::uint32_t>(raw - 1U);
    if (idx >= impl_->buffers.size() || !impl_->acquired_mask.at(idx)) {
      return;
    }
    impl_->acquired_mask.at(idx) = false;
    auto const requeue_ec =
        queue_capture_buffer(impl_->v4l2_fd, impl_->is_mplane, idx, impl_->capture_num_planes);
    (void)requeue_ec;
    return;
  }
  // MmapCopy: dumb buffers are permanently owned.
}

SourceFormat V4l2CameraSource::format() const noexcept {
  return impl_ ? impl_->fmt : SourceFormat{};
}

drm::expected<drm::BufferMapping, std::error_code> V4l2CameraSource::map(drm::MapAccess access) {
  if (!impl_) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::function_not_supported));
  }
  if (impl_->active_mode != V4l2CameraBufferMode::MmapCopy) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::function_not_supported));
  }
  std::size_t const idx =
      (access == drm::MapAccess::Read) ? impl_->front_idx : (1U - impl_->front_idx);
  auto& target = impl_->dumb_pair.at(idx);
  if (target.empty() || target.data() == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  return target.map(access);
}

void V4l2CameraSource::on_session_paused() noexcept {
  if (!impl_) {
    return;
  }
  for (auto& slot : impl_->buffers) {
    slot.drm_fd = -1;
    slot.fb_id = 0;
    slot.drm_handles.fill(0);
  }
  for (auto& b : impl_->dumb_pair) {
    b.forget();
  }
  impl_->drm_fd = -1;
}

drm::expected<void, std::error_code> V4l2CameraSource::on_session_resumed(
    const drm::Device& new_dev) {
  if (!impl_) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  int const new_drm_fd = new_dev.fd();
  if (new_drm_fd < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  std::uint32_t const drm_fourcc = v4l2_to_drm_fourcc(impl_->cfg.pixel_fourcc);
  if (drm_fourcc == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_supported));
  }

  if (impl_->active_mode == V4l2CameraBufferMode::DmaBufZeroCopy) {
    DrmPlaneLayout layout{};
    if (auto ec = derive_drm_plane_layout(impl_->format_echo, impl_->is_mplane, drm_fourcc, layout);
        ec) {
      return drm::unexpected<std::error_code>(ec);
    }
    std::uint64_t const modifier =
        (impl_->cfg.modifier == 0) ? DRM_FORMAT_MOD_LINEAR : impl_->cfg.modifier;
    // Per-slot rollback on partial failure: if slot K's import fails,
    // slots 0..K-1 already hold valid fb_ids and GEM handles on
    // new_drm_fd. Without rollback they linger as kernel-side refs
    // until libseat closes new_drm_fd, and the source is left in
    // mixed state where acquire() returns EAGAIN against
    // impl_->drm_fd == -1. Walk back and tear down the imports we
    // made before returning the error so the source ends up in the
    // same shape as on_session_paused.
    for (std::size_t k = 0; k < impl_->buffers.size(); ++k) {
      auto& slot = impl_->buffers.at(k);
      if (auto ec = import_capture_buffer_to_drm(new_drm_fd, impl_->fmt.width, impl_->fmt.height,
                                                 drm_fourcc, modifier, layout, slot);
          ec) {
        for (std::size_t j = 0; j < k; ++j) {
          auto& prior = impl_->buffers.at(j);
          if (prior.fb_id != 0) {
            drmModeRmFB(new_drm_fd, prior.fb_id);
          }
          for (auto h : prior.drm_handles) {
            if (h != 0) {
              drmCloseBufferHandle(new_drm_fd, h);
            }
          }
          prior.drm_fd = -1;
          prior.fb_id = 0;
          prior.drm_handles.fill(0);
        }
        return drm::unexpected<std::error_code>(ec);
      }
    }
  } else {
    auto a = allocate_dumb_for_format(new_dev, impl_->fmt.width, impl_->fmt.height, drm_fourcc);
    if (!a) {
      return drm::unexpected<std::error_code>(a.error());
    }
    auto b = allocate_dumb_for_format(new_dev, impl_->fmt.width, impl_->fmt.height, drm_fourcc);
    if (!b) {
      return drm::unexpected<std::error_code>(b.error());
    }
    impl_->dumb_pair[0] = std::move(*a);
    impl_->dumb_pair[1] = std::move(*b);
    impl_->front_idx = 0;
    impl_->any_published = false;
  }
  impl_->drm_fd = new_drm_fd;
  return {};
}

}  // namespace drm::scene