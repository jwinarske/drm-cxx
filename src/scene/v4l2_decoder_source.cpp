// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "v4l2_decoder_source.hpp"

#include "buffer_source.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <memory>
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

[[nodiscard]] std::error_code validate_config(const char* device_path,
                                              const V4l2DecoderConfig& cfg) noexcept {
  if (device_path == nullptr || std::strlen(device_path) == 0U) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (cfg.codec_fourcc == 0U || cfg.capture_fourcc == 0U) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (cfg.coded_width == 0U || cfg.coded_height == 0U) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (cfg.output_buffer_count < k_min_buffers || cfg.output_buffer_count > k_max_buffers) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (cfg.capture_buffer_count < k_min_buffers || cfg.capture_buffer_count > k_max_buffers) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  return {};
}

// V4L2 ioctls can return EINTR if a signal interrupts the call before
// the kernel commits any state; the canonical idiom is to retry the
// ioctl until it succeeds, fails with a non-EINTR errno, or we give
// up. The retry budget is small because legitimate EINTR loops resolve
// in one or two iterations; a runaway here would be a kernel bug.
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

// errno values from POSIX-defined system calls map 1:1 onto std::errc;
// using generic_category here keeps assertions of the form
// `EXPECT_EQ(r.error(), std::make_error_code(std::errc::foo))` working,
// which is the convention used elsewhere in the test suite.
[[nodiscard]] std::error_code make_errno(int err) noexcept {
  return {err, std::generic_category()};
}

[[nodiscard]] std::error_code subscribe_event(int fd, std::uint32_t event_type) noexcept {
  v4l2_event_subscription sub{};
  sub.type = event_type;
  if (int const e = xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub); e != 0) {
    return make_errno(e);
  }
  return {};
}

// DRM AddFB2 takes up to 4 plane handles/pitches/offsets.
constexpr std::size_t k_drm_max_planes = 4;

// Per-CAPTURE-buffer state. RAII-cleaned: the destructor releases the
// per-buffer DRM framebuffer + GEM handles, munmaps the per-plane CPU
// mappings, and closes the per-plane DMA-BUF fds the kernel handed
// back from VIDIOC_EXPBUF. That makes failure mid-way through
// allocate_capture_buffers() unwind correctly when the local
// std::vector unwinds. Move-only so the vector can grow.
struct V4l2CaptureBuffer {
  std::array<void*, VIDEO_MAX_PLANES> mapped_ptr{};
  std::array<std::size_t, VIDEO_MAX_PLANES> mapped_len{};
  std::array<int, VIDEO_MAX_PLANES> dmabuf_fd{};
  std::uint32_t num_planes{0};

  // DRM-side state, populated by import_capture_buffer_to_drm. drm_fd
  // is borrowed from the caller's drm::Device for the source's
  // lifetime; the destructor uses it to drop GEM refs cleanly.
  // Cleared (drm_fd = -1, fb_id = 0, all handles 0) on session pause
  // so re-import against the new DRM fd happens without double-free.
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

// Per-DRM-plane layout derived from the V4L2 CAPTURE format echo. A
// single V4L2 buffer sometimes maps to multiple DRM planes (e.g.
// V4L2_PIX_FMT_NV12 packs Y and UV into one buffer with offset math
// the caller has to compute), so each DRM plane records which V4L2
// dmabuf_fd it imports from and where in that fd's address space the
// plane starts.
struct DrmPlaneLayout {
  std::uint32_t num_drm_planes{0};
  std::array<std::uint32_t, k_drm_max_planes> pitch{};
  std::array<std::uint32_t, k_drm_max_planes> offset{};
  std::array<std::uint8_t, k_drm_max_planes> v4l2_plane_idx{};
};

// Decide how the V4L2 CAPTURE format echo maps onto DRM AddFB2's
// per-plane handle/pitch/offset arrays.
//
// The two shapes that come up in practice with V4L2 stateful decoders:
//   * Single-V4L2-plane NV12: Y at offset 0, UV at offset
//     bytesperline * height. Both DRM planes share V4L2 plane 0's
//     dmabuf fd (the kernel dedups on prime-import).
//   * MPLANE V4L2 with num_planes matching the DRM fourcc's plane
//     count: 1:1 mapping, each DRM plane reads its own V4L2 plane.
//
// Other shapes (single-V4L2-plane YUV420, MPLANE with mismatched
// counts, RGB packed in MPLANE-with-1-plane) collapse into the 1:1
// default. The default is also correct for any single-DRM-plane
// format (RGB, packed YUYV, etc.).
[[nodiscard]] std::error_code derive_drm_plane_layout(const v4l2_format& cap_fmt, bool is_mplane,
                                                      std::uint32_t drm_fourcc,
                                                      DrmPlaneLayout& out) noexcept {
  // Special case: single-V4L2-plane NV12 -> 2 DRM planes via offset math.
  if (drm_fourcc == DRM_FORMAT_NV12 && !is_mplane) {
    std::uint32_t const bpl = cap_fmt.fmt.pix.bytesperline;
    std::uint32_t const h = cap_fmt.fmt.pix.height;
    if (bpl == 0 || h == 0) {
      return std::make_error_code(std::errc::invalid_argument);
    }
    out.num_drm_planes = 2;
    out.pitch.at(0) = bpl;
    out.pitch.at(1) = bpl;
    out.offset.at(0) = 0;
    out.offset.at(1) = bpl * h;
    out.v4l2_plane_idx.at(0) = 0;
    out.v4l2_plane_idx.at(1) = 0;
    return {};
  }

  // Default: 1 DRM plane per V4L2 plane (1:1). For single-V4L2-plane
  // formats this collapses to a single-DRM-plane import. For MPLANE
  // formats we read num_planes / per-plane bytesperline out of pix_mp.
  if (is_mplane) {
    std::uint32_t const n = cap_fmt.fmt.pix_mp.num_planes;
    if (n == 0 || n > k_drm_max_planes) {
      return std::make_error_code(std::errc::not_supported);
    }
    out.num_drm_planes = n;
    for (std::uint32_t i = 0; i < n; ++i) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      out.pitch.at(i) = cap_fmt.fmt.pix_mp.plane_fmt[i].bytesperline;
      out.offset.at(i) = 0;
      out.v4l2_plane_idx.at(i) = static_cast<std::uint8_t>(i);
    }
    return {};
  }

  out.num_drm_planes = 1;
  out.pitch.at(0) = cap_fmt.fmt.pix.bytesperline;
  out.offset.at(0) = 0;
  out.v4l2_plane_idx.at(0) = 0;
  return {};
}

// drmPrimeFDToHandle every DRM plane's source dmabuf into `drm_fd`,
// then drmModeAddFB2WithModifiers to a stable per-buffer fb_id. The
// kernel's prime-import GEM dedup means importing the same dmabuf
// twice (NV12 single-V4L2-plane case) returns the same handle and
// the second drmCloseBufferHandle decref is correct.
//
// On any failure, partial state is rolled back so the caller's
// V4l2CaptureBuffer dtor doesn't double-free.
[[nodiscard]] std::error_code import_capture_buffer_to_drm(int drm_fd, std::uint32_t width,
                                                           std::uint32_t height,
                                                           std::uint32_t drm_fourcc,
                                                           const DrmPlaneLayout& layout,
                                                           V4l2CaptureBuffer& slot) noexcept {
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
      // Roll back any handles we already imported for this slot. The
      // dtor would double-close otherwise because we haven't yet
      // committed the slot's drm_fd.
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
    modifiers.at(p) = DRM_FORMAT_MOD_LINEAR;
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

  // Commit. Storing drm_fd here arms the dtor's KMS-side cleanup.
  slot.drm_fd = drm_fd;
  slot.fb_id = fb_id;
  for (std::uint32_t p = 0; p < layout.num_drm_planes; ++p) {
    slot.drm_handles.at(p) = handles.at(p);
  }
  return {};
}

// Issue REQBUFS(count=0) on the named queue so the kernel reclaims
// its bookkeeping. Best-effort on shutdown -- callers must already
// have munmap'd / closed every plane.
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

[[nodiscard]] std::uint32_t output_buf_type(bool is_mplane) noexcept {
  return is_mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
}
[[nodiscard]] std::uint32_t capture_buf_type(bool is_mplane) noexcept {
  return is_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
}

// VIDIOC_STREAMON / STREAMOFF wrappers. The kernel takes the buffer
// type by pointer-to-int.
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

// VIDIOC_QBUF a CAPTURE buffer back to the kernel so it can write the
// next decoded frame into it. Used both in create() (initial bulk
// queue) and after release() (re-queue once the consumer is done).
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

// Per-OUTPUT-buffer state. Lighter than V4l2CaptureBuffer because
// OUTPUT buffers are bitstream input -- caller memcpys into the
// mmap, kernel reads, no DMA-BUF export, no DRM import.
struct V4l2OutputBuffer {
  std::array<void*, VIDEO_MAX_PLANES> mapped_ptr{};
  std::array<std::size_t, VIDEO_MAX_PLANES> mapped_len{};
  std::uint32_t num_planes{0};

  V4l2OutputBuffer() = default;
  ~V4l2OutputBuffer() {
    for (std::uint32_t p = 0; p < num_planes; ++p) {
      if (mapped_ptr.at(p) != nullptr && mapped_ptr.at(p) != MAP_FAILED) {
        ::munmap(mapped_ptr.at(p), mapped_len.at(p));
      }
    }
  }
  V4l2OutputBuffer(const V4l2OutputBuffer&) = delete;
  V4l2OutputBuffer& operator=(const V4l2OutputBuffer&) = delete;
  V4l2OutputBuffer(V4l2OutputBuffer&& o) noexcept
      : mapped_ptr(o.mapped_ptr), mapped_len(o.mapped_len), num_planes(o.num_planes) {
    o.mapped_ptr.fill(nullptr);
    o.num_planes = 0;
  }
  V4l2OutputBuffer& operator=(V4l2OutputBuffer&&) = delete;
};

// REQBUFS + per-buffer QUERYBUF + per-plane MMAP for the OUTPUT queue.
// No EXPBUF -- OUTPUT is bitstream input, never displayed. Same
// failure-unwinds-via-vector pattern as allocate_capture_buffers.
[[nodiscard]] std::error_code allocate_output_buffers(int fd, bool is_mplane,
                                                      std::uint32_t requested_count,
                                                      std::vector<V4l2OutputBuffer>& out) {
  v4l2_requestbuffers req{};
  req.count = requested_count;
  req.type = output_buf_type(is_mplane);
  req.memory = V4L2_MEMORY_MMAP;
  if (int const e = xioctl(fd, VIDIOC_REQBUFS, &req); e != 0) {
    return make_errno(e);
  }
  if (req.count == 0) {
    return std::make_error_code(std::errc::not_enough_memory);
  }

  out.reserve(req.count);
  for (std::uint32_t i = 0; i < req.count; ++i) {
    V4l2OutputBuffer slot;
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

    std::uint32_t const fmt_num_planes = is_mplane ? buf.length : 1;
    if (fmt_num_planes == 0 || fmt_num_planes > VIDEO_MAX_PLANES) {
      return std::make_error_code(std::errc::not_supported);
    }
    slot.num_planes = fmt_num_planes;
    for (std::uint32_t p = 0; p < fmt_num_planes; ++p) {
      std::size_t const length = is_mplane ? planes.at(p).length : buf.length;
      std::uint32_t const offset = is_mplane ? planes.at(p).m.mem_offset : buf.m.offset;
      void* const mapped = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
      if (mapped == MAP_FAILED) {
        slot.num_planes = p;
        out.push_back(std::move(slot));
        return make_errno(errno);
      }
      slot.mapped_ptr.at(p) = mapped;
      slot.mapped_len.at(p) = length;
    }
    out.push_back(std::move(slot));
  }
  return {};
}

// Allocate `requested_count` CAPTURE buffers via REQBUFS + per-buffer
// QUERYBUF + per-plane MMAP + EXPBUF. On any failure, `out` retains
// the buffers we did manage to allocate; their destructors release
// the kernel-side state on scope exit so callers don't have to wind
// the failure path manually.
//
// Returns the actually-granted count via `out.size()` (the kernel
// may hand us fewer buffers than requested if memory is tight); the
// caller is responsible for checking against `k_min_buffers` before
// committing the allocation to Impl.
[[nodiscard]] std::error_code allocate_capture_buffers(int fd, bool is_mplane,
                                                       std::uint32_t fmt_num_planes,
                                                       std::uint32_t requested_count,
                                                       std::vector<V4l2CaptureBuffer>& out) {
  v4l2_requestbuffers req{};
  req.count = requested_count;
  req.type = is_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
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
      // mmap's last parameter is off_t; the V4L2 mem_offset / m.offset
      // fields are __u32, and the implicit conversion at the mmap call
      // boundary is safe because V4L2 buffer offsets fit in 32 bits by
      // definition. Naming off_t directly here would force a
      // <sys/types.h> include that include-cleaner then flags as
      // duplicating the declaration libc already pulls in via <fcntl.h>.
      std::uint32_t const offset = is_mplane ? planes.at(p).m.mem_offset : buf.m.offset;
      // PROT_READ + PROT_WRITE: write is needed for the sysmem fallback
      // path that copies decoded NV12 into a CPU-visible buffer when no
      // dmabuf import is wired up; read is needed for that same memcpy
      // and for diagnostics. MAP_SHARED is required by V4L2.
      void* const mapped = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
      if (mapped == MAP_FAILED) {
        slot.num_planes = p;  // only release what we successfully mapped
        out.push_back(std::move(slot));
        return make_errno(errno);
      }
      slot.mapped_ptr.at(p) = mapped;
      slot.mapped_len.at(p) = length;

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
    out.push_back(std::move(slot));
  }
  return {};
}

}  // namespace

// Pimpl carrying the full V4L2 + DRM-side state. Hidden so the public
// header doesn't need <linux/videodev2.h>. Step 2 populates the
// CAPTURE-side buffer ring; OUTPUT-side buffers and the dup'd DRM fd
// that turns each CAPTURE plane's dmabuf into a KMS FB land in
// follow-ups.
struct V4l2DecoderSource::Impl {
  int v4l2_fd{-1};
  // drm_fd is borrowed (not owned) from the caller's drm::Device; we
  // never close it. Per-buffer fb_id and GEM handles ARE owned and
  // released in V4l2CaptureBuffer's destructor.
  int drm_fd{-1};
  V4l2DecoderConfig cfg{};
  bool is_mplane{false};
  std::uint32_t capture_num_planes{0};
  SourceFormat capture_format{};
  // Cached post-S_FMT echo (bytesperline / sizeimage / per-plane
  // metadata) is the input on_session_resumed needs to redo
  // derive_drm_plane_layout against the new drm_fd without another
  // S_FMT round-trip.
  v4l2_format capture_format_echo{};
  std::vector<V4l2CaptureBuffer> capture_buffers;

  // OUTPUT side. submit_bitstream pulls a slot index off output_free,
  // memcpys the coded chunk into the corresponding mmap, QBUFs to the
  // kernel; drive() pushes completed slots back onto output_free as
  // it dequeues them. STREAMON OUTPUT happens lazily on the first
  // submit_bitstream call so the kernel always sees a queued buffer
  // when it transitions into the streaming state.
  std::vector<V4l2OutputBuffer> output_buffers;
  std::vector<std::uint32_t> output_free;
  bool output_streaming{false};
  bool capture_streaming{false};

  // CAPTURE-side latest-frame-wins state. capture_ready_idx is the
  // most-recently-decoded slot waiting for acquire(); -1 sentinel
  // means no frame is ready. capture_acquired_idx is held by the
  // consumer between acquire() and release().
  int capture_ready_idx{-1};
  int capture_acquired_idx{-1};

  // Sticky decoder-state flags surfaced by drive() and queried by
  // acquire(). source_change_seen disables the source permanently
  // (caller must destroy + recreate per the design contract);
  // eos_seen is informational for callers that care about end-of-
  // stream.
  bool source_change_seen{false};
  bool eos_seen{false};
};

V4l2DecoderSource::V4l2DecoderSource() : impl_(std::make_unique<Impl>()) {}

V4l2DecoderSource::~V4l2DecoderSource() {
  if (!impl_) {
    return;
  }
  // STREAMOFF first so the kernel returns any queued buffers and
  // stops touching the mappings; then per-buffer cleanup (mmaps +
  // dmabuf fds + DRM state) before REQBUFS(count=0) per queue, since
  // the kernel rejects REQBUFS=0 while pages are still mapped.
  if (impl_->capture_streaming) {
    streamoff(impl_->v4l2_fd, capture_buf_type(impl_->is_mplane));
    impl_->capture_streaming = false;
  }
  if (impl_->output_streaming) {
    streamoff(impl_->v4l2_fd, output_buf_type(impl_->is_mplane));
    impl_->output_streaming = false;
  }
  impl_->capture_buffers.clear();
  release_queue(impl_->v4l2_fd, capture_buf_type(impl_->is_mplane));
  impl_->output_buffers.clear();
  release_queue(impl_->v4l2_fd, output_buf_type(impl_->is_mplane));
  if (impl_->v4l2_fd >= 0) {
    ::close(impl_->v4l2_fd);
    impl_->v4l2_fd = -1;
  }
}

drm::expected<std::unique_ptr<V4l2DecoderSource>, std::error_code> V4l2DecoderSource::create(
    const drm::Device& dev, const char* device_path, const V4l2DecoderConfig& cfg) {
  if (auto ec = validate_config(device_path, cfg); ec) {
    return drm::unexpected<std::error_code>(ec);
  }
  // The DRM fd is checked AFTER V4L2 negotiation so the most-immediate
  // error surfaces first (a bad path or non-V4L2 device should produce
  // ENOENT / not_supported, not bad_file_descriptor).
  int const drm_fd = dev.fd();

  // O_NONBLOCK so drive() can drain DQBUF/DQEVENT without stalling the
  // caller's commit thread; CAPTURE EAGAIN is the in-band "no frame
  // ready" signal that LayerScene already understands as flow control.
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
    // ENOTTY here means the fd isn't a V4L2 device at all (e.g. caller
    // pointed at /dev/null). Surface as "not supported" so callers can
    // distinguish it from path-not-found.
    return fail(e == ENOTTY ? std::make_error_code(std::errc::not_supported) : make_errno(e));
  }

  // Per-device caps live in `device_caps` when V4L2_CAP_DEVICE_CAPS is
  // set in the overall `capabilities` mask; otherwise the union is
  // reported in `capabilities`. M2M decoders advertise either the
  // single-plane (V4L2_CAP_VIDEO_M2M) or multi-plane
  // (V4L2_CAP_VIDEO_M2M_MPLANE) flavor; both also need
  // V4L2_CAP_STREAMING because we use the mmap+QBUF path.
  std::uint32_t const device_caps =
      ((cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U) ? cap.device_caps : cap.capabilities;
  bool const is_mplane = (device_caps & V4L2_CAP_VIDEO_M2M_MPLANE) != 0U;
  bool const is_single = (device_caps & V4L2_CAP_VIDEO_M2M) != 0U;
  if (!is_mplane && !is_single) {
    return fail(std::make_error_code(std::errc::not_supported));
  }
  if ((device_caps & V4L2_CAP_STREAMING) == 0U) {
    return fail(std::make_error_code(std::errc::not_supported));
  }

  // S_FMT on OUTPUT — the codec the decoder is being asked to consume.
  // The kernel echoes the negotiated values back; drivers may snap
  // sizeimage up to a hardware-imposed minimum, but the fourcc and
  // dimensions stay as requested for compliant decoders.
  v4l2_format out_fmt{};
  if (is_mplane) {
    out_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    out_fmt.fmt.pix_mp.pixelformat = cfg.codec_fourcc;
    out_fmt.fmt.pix_mp.width = cfg.coded_width;
    out_fmt.fmt.pix_mp.height = cfg.coded_height;
    out_fmt.fmt.pix_mp.num_planes = 1;
    if (cfg.output_buffer_size > 0) {
      out_fmt.fmt.pix_mp.plane_fmt[0].sizeimage =
          static_cast<std::uint32_t>(cfg.output_buffer_size);
    }
  } else {
    out_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    out_fmt.fmt.pix.pixelformat = cfg.codec_fourcc;
    out_fmt.fmt.pix.width = cfg.coded_width;
    out_fmt.fmt.pix.height = cfg.coded_height;
    if (cfg.output_buffer_size > 0) {
      out_fmt.fmt.pix.sizeimage = static_cast<std::uint32_t>(cfg.output_buffer_size);
    }
  }
  if (int const e = xioctl(fd, VIDIOC_S_FMT, &out_fmt); e != 0) {
    return fail(make_errno(e));
  }

  // S_FMT on CAPTURE — the decoded format the source will hand to the
  // scene. Kernel echoes back negotiated width/height and may inject a
  // bytesperline / sizeimage; we cache the post-S_FMT values into
  // capture_format so format() reflects what the decoder will actually
  // produce, not what the caller asked for.
  v4l2_format cap_fmt{};
  std::uint32_t cap_w = 0;
  std::uint32_t cap_h = 0;
  std::uint32_t cap_num_planes = 0;
  if (is_mplane) {
    cap_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cap_fmt.fmt.pix_mp.pixelformat = cfg.capture_fourcc;
    cap_fmt.fmt.pix_mp.width = cfg.coded_width;
    cap_fmt.fmt.pix_mp.height = cfg.coded_height;
  } else {
    cap_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cap_fmt.fmt.pix.pixelformat = cfg.capture_fourcc;
    cap_fmt.fmt.pix.width = cfg.coded_width;
    cap_fmt.fmt.pix.height = cfg.coded_height;
  }
  if (int const e = xioctl(fd, VIDIOC_S_FMT, &cap_fmt); e != 0) {
    return fail(make_errno(e));
  }
  if (is_mplane) {
    cap_w = cap_fmt.fmt.pix_mp.width;
    cap_h = cap_fmt.fmt.pix_mp.height;
    cap_num_planes = cap_fmt.fmt.pix_mp.num_planes;
  } else {
    cap_w = cap_fmt.fmt.pix.width;
    cap_h = cap_fmt.fmt.pix.height;
    cap_num_planes = 1;
  }
  if (cap_num_planes == 0 || cap_num_planes > VIDEO_MAX_PLANES) {
    return fail(std::make_error_code(std::errc::not_supported));
  }

  // Subscribe to events the decoder fires asynchronously: SOURCE_CHANGE
  // when the bitstream's resolution differs from the OUTPUT-side
  // configuration (drive() surfaces it as operation_canceled so the
  // caller destroys + recreates), and EOS when the decoder has flushed
  // the last frame out of the CAPTURE queue.
  if (auto ec = subscribe_event(fd, V4L2_EVENT_SOURCE_CHANGE); ec) {
    return fail(ec);
  }
  if (auto ec = subscribe_event(fd, V4L2_EVENT_EOS); ec) {
    return fail(ec);
  }

  // REQBUFS + per-buffer QUERYBUF + per-plane MMAP + EXPBUF on the
  // CAPTURE queue. capture_buffers is local so any failure midway
  // unwinds via std::vector destruction (each V4l2CaptureBuffer
  // munmaps + closes its own state). On failure we then issue
  // REQBUFS(count=0) so the kernel doesn't keep the partial allocation
  // pinned, and finally close the fd.
  std::vector<V4l2CaptureBuffer> capture_buffers;
  if (auto ec = allocate_capture_buffers(fd, is_mplane, cap_num_planes, cfg.capture_buffer_count,
                                         capture_buffers);
      ec) {
    capture_buffers.clear();
    release_queue(fd, capture_buf_type(is_mplane));
    return fail(ec);
  }
  if (capture_buffers.size() < k_min_buffers) {
    capture_buffers.clear();
    release_queue(fd, capture_buf_type(is_mplane));
    return fail(std::make_error_code(std::errc::not_enough_memory));
  }

  if (drm_fd < 0) {
    capture_buffers.clear();
    release_queue(fd, capture_buf_type(is_mplane));
    return fail(std::make_error_code(std::errc::bad_file_descriptor));
  }

  // drmPrimeFDToHandle + drmModeAddFB2WithModifiers per buffer. Each
  // V4l2CaptureBuffer arms its own KMS-side dtor on success, so any
  // failure mid-loop unwinds via std::vector destruction the same way
  // the V4L2 side does.
  DrmPlaneLayout layout{};
  if (auto ec = derive_drm_plane_layout(cap_fmt, is_mplane, cfg.capture_fourcc, layout); ec) {
    capture_buffers.clear();
    release_queue(fd, capture_buf_type(is_mplane));
    return fail(ec);
  }
  for (auto& slot : capture_buffers) {
    if (auto ec =
            import_capture_buffer_to_drm(drm_fd, cap_w, cap_h, cfg.capture_fourcc, layout, slot);
        ec) {
      capture_buffers.clear();
      release_queue(fd, capture_buf_type(is_mplane));
      return fail(ec);
    }
  }

  // OUTPUT-side allocation (REQBUFS + MMAP, no EXPBUF). Failure unwinds
  // CAPTURE alongside since both queues must be torn down together.
  std::vector<V4l2OutputBuffer> output_buffers;
  if (auto ec = allocate_output_buffers(fd, is_mplane, cfg.output_buffer_count, output_buffers);
      ec) {
    output_buffers.clear();
    release_queue(fd, output_buf_type(is_mplane));
    capture_buffers.clear();
    release_queue(fd, capture_buf_type(is_mplane));
    return fail(ec);
  }
  if (output_buffers.size() < k_min_buffers) {
    output_buffers.clear();
    release_queue(fd, output_buf_type(is_mplane));
    capture_buffers.clear();
    release_queue(fd, capture_buf_type(is_mplane));
    return fail(std::make_error_code(std::errc::not_enough_memory));
  }

  // QBUF every CAPTURE buffer so the kernel has destinations for the
  // first decoded frames, then STREAMON CAPTURE. Failure mid-loop
  // would have left some buffers queued; STREAMOFF on cleanup
  // (destructor path) returns them to userspace.
  for (std::uint32_t i = 0; i < capture_buffers.size(); ++i) {
    if (auto ec = queue_capture_buffer(fd, is_mplane, i, cap_num_planes); ec) {
      output_buffers.clear();
      release_queue(fd, output_buf_type(is_mplane));
      capture_buffers.clear();
      release_queue(fd, capture_buf_type(is_mplane));
      return fail(ec);
    }
  }
  if (auto ec = streamon(fd, capture_buf_type(is_mplane)); ec) {
    output_buffers.clear();
    release_queue(fd, output_buf_type(is_mplane));
    capture_buffers.clear();
    release_queue(fd, capture_buf_type(is_mplane));
    return fail(ec);
  }

  // OUTPUT slot indices populate the free list in REQBUFS order so
  // submit_bitstream's first calls reuse the cache-warm low-index
  // buffers; STREAMON OUTPUT is deferred until submit_bitstream has
  // a buffer ready to QBUF (some drivers reject STREAMON on an empty
  // queue).
  std::vector<std::uint32_t> output_free;
  output_free.reserve(output_buffers.size());
  for (std::uint32_t i = 0; i < output_buffers.size(); ++i) {
    output_free.push_back(i);
  }

  std::unique_ptr<V4l2DecoderSource> src(new V4l2DecoderSource());
  src->impl_->v4l2_fd = fd;
  src->impl_->drm_fd = drm_fd;
  src->impl_->cfg = cfg;
  src->impl_->is_mplane = is_mplane;
  src->impl_->capture_num_planes = cap_num_planes;
  // V4L2 surfaces in v1 are LINEAR (DRM_FORMAT_MOD_LINEAR == 0).
  src->impl_->capture_format = SourceFormat{cfg.capture_fourcc, 0, cap_w, cap_h};
  src->impl_->capture_format_echo = cap_fmt;
  src->impl_->capture_buffers = std::move(capture_buffers);
  src->impl_->output_buffers = std::move(output_buffers);
  src->impl_->output_free = std::move(output_free);
  src->impl_->capture_streaming = true;
  return src;
}

int V4l2DecoderSource::fd() const noexcept {
  return impl_ ? impl_->v4l2_fd : -1;
}

drm::expected<void, std::error_code> V4l2DecoderSource::drive() noexcept {
  if (!impl_) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  // Sticky once seen: a SOURCE_CHANGE event means the bitstream's
  // resolution doesn't match what create() negotiated, and the
  // contract is that the caller destroys + recreates with the new
  // dimensions. Keep returning operation_canceled so a polling loop
  // stops trying to feed the decoder.
  if (impl_->source_change_seen) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::operation_canceled));
  }

  int const fd = impl_->v4l2_fd;

  // Drain queued events. EAGAIN-equivalent (errno set by ioctl)
  // breaks the loop -- there's no portable "no more events" return,
  // so we rely on the kernel returning ENOENT once the queue is
  // empty. Treat any other error as transient and stop draining for
  // this drive() call.
  for (;;) {
    v4l2_event ev{};
    if (int const e = xioctl(fd, VIDIOC_DQEVENT, &ev); e != 0) {
      if (e == ENOENT || e == EAGAIN) {
        break;
      }
      return drm::unexpected<std::error_code>(make_errno(e));
    }
    if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
      impl_->source_change_seen = true;
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::operation_canceled));
    }
    if (ev.type == V4L2_EVENT_EOS) {
      impl_->eos_seen = true;
    }
  }

  // Drain completed OUTPUT buffers back to the free list. The
  // decoder copies bitstream out of these as it consumes them, so
  // dequeueing with EAGAIN simply means "kernel hasn't finished any
  // more this tick".
  while (true) {
    v4l2_buffer buf{};
    std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
    buf.type = output_buf_type(impl_->is_mplane);
    buf.memory = V4L2_MEMORY_MMAP;
    if (impl_->is_mplane) {
      buf.length = VIDEO_MAX_PLANES;
      buf.m.planes = planes.data();
    }
    if (int const e = xioctl(fd, VIDIOC_DQBUF, &buf); e != 0) {
      if (e == EAGAIN) {
        break;
      }
      return drm::unexpected<std::error_code>(make_errno(e));
    }
    if (buf.index < impl_->output_buffers.size()) {
      impl_->output_free.push_back(buf.index);
    }
  }

  // Drain newly-decoded CAPTURE buffers. Latest-frame-wins: if a
  // prior buffer was already pending in capture_ready_idx, re-queue
  // it to the kernel so it can be reused for the next decode (the
  // consumer only ever sees the freshest frame).
  while (true) {
    v4l2_buffer buf{};
    std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
    buf.type = capture_buf_type(impl_->is_mplane);
    buf.memory = V4L2_MEMORY_MMAP;
    if (impl_->is_mplane) {
      buf.length = VIDEO_MAX_PLANES;
      buf.m.planes = planes.data();
    }
    if (int const e = xioctl(fd, VIDIOC_DQBUF, &buf); e != 0) {
      if (e == EAGAIN) {
        break;
      }
      return drm::unexpected<std::error_code>(make_errno(e));
    }
    if (buf.index >= impl_->capture_buffers.size()) {
      continue;  // defensive -- index out of range shouldn't happen
    }
    if (impl_->capture_ready_idx >= 0) {
      // Drop the prior pending frame back to the kernel. If the
      // re-queue fails (rare; would mean the queue is in a degraded
      // state) the slot stays unaccounted-for for this call; future
      // drive() invocations will try again on subsequent dequeues.
      auto const requeue_ec = queue_capture_buffer(
          fd, impl_->is_mplane, static_cast<std::uint32_t>(impl_->capture_ready_idx),
          impl_->capture_num_planes);
      (void)requeue_ec;
    }
    impl_->capture_ready_idx = static_cast<int>(buf.index);
  }

  return {};
}

drm::expected<void, std::error_code> V4l2DecoderSource::submit_bitstream(
    drm::span<const std::uint8_t> coded, std::uint64_t timestamp_ns) {
  if (!impl_ || coded.empty()) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  if (impl_->source_change_seen) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::operation_canceled));
  }
  if (impl_->output_free.empty()) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  }

  // Pop a free slot, copy the bitstream in, QBUF. We restore the
  // slot to output_free on any failure path so the next call has the
  // same number of usable slots.
  std::uint32_t const idx = impl_->output_free.back();
  impl_->output_free.pop_back();
  if (idx >= impl_->output_buffers.size()) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  auto& slot = impl_->output_buffers.at(idx);
  if (slot.num_planes == 0 || slot.mapped_ptr.at(0) == nullptr) {
    impl_->output_free.push_back(idx);
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  if (coded.size() > slot.mapped_len.at(0)) {
    impl_->output_free.push_back(idx);
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::message_size));
  }
  std::memcpy(slot.mapped_ptr.at(0), coded.data(), coded.size());

  v4l2_buffer buf{};
  std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
  buf.type = output_buf_type(impl_->is_mplane);
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = idx;
  // V4L2 propagates timestamp from the OUTPUT buffer to the matching
  // CAPTURE buffer, so callers tracking PTS use it as a frame ID.
  // Split timestamp_ns into the timeval the kernel UAPI expects.
  buf.timestamp.tv_sec = static_cast<long>(timestamp_ns / 1'000'000'000ULL);
  buf.timestamp.tv_usec = static_cast<long>((timestamp_ns % 1'000'000'000ULL) / 1000ULL);
  if (impl_->is_mplane) {
    buf.length = slot.num_planes;
    buf.m.planes = planes.data();
    planes.at(0).bytesused = static_cast<std::uint32_t>(coded.size());
  } else {
    buf.bytesused = static_cast<std::uint32_t>(coded.size());
  }
  if (int const e = xioctl(impl_->v4l2_fd, VIDIOC_QBUF, &buf); e != 0) {
    impl_->output_free.push_back(idx);
    return drm::unexpected<std::error_code>(make_errno(e));
  }

  // Lazy STREAMON OUTPUT now that at least one buffer is queued. We
  // can't easily un-QBUF the slot if STREAMON fails, so the slot
  // stays with the kernel; subsequent calls would either dequeue it
  // (drive()) or fail similarly.
  if (!impl_->output_streaming) {
    if (auto ec = streamon(impl_->v4l2_fd, output_buf_type(impl_->is_mplane)); ec) {
      return drm::unexpected<std::error_code>(ec);
    }
    impl_->output_streaming = true;
  }
  return {};
}

drm::expected<AcquiredBuffer, std::error_code> V4l2DecoderSource::acquire() {
  if (!impl_) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  if (impl_->source_change_seen) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::operation_canceled));
  }
  if (impl_->capture_acquired_idx >= 0) {
    // The contract is "valid until matched by release()" -- a second
    // acquire without intervening release would hand back the same
    // slot and break the scene's pair-up bookkeeping. Surface as
    // device_or_resource_busy so callers can spot the misuse.
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::device_or_resource_busy));
  }
  if (impl_->capture_ready_idx < 0) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  }

  auto const idx = static_cast<std::uint32_t>(impl_->capture_ready_idx);
  if (idx >= impl_->capture_buffers.size()) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  std::uint32_t const fb_id = impl_->capture_buffers.at(idx).fb_id;
  if (fb_id == 0) {
    // fb_id is cleared during on_session_paused; the source can't
    // hand a real KMS framebuffer back until on_session_resumed
    // re-imports the buffer ring. Treat as "no frame ready right now"
    // so the scene's flow-control path retries cleanly.
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  }

  impl_->capture_acquired_idx = impl_->capture_ready_idx;
  impl_->capture_ready_idx = -1;
  AcquiredBuffer acq;
  acq.fb_id = fb_id;
  acq.acquire_fence_fd = -1;
  acq.opaque = nullptr;  // capture_acquired_idx is the source-side bookkeeping
  return acq;
}

void V4l2DecoderSource::release(AcquiredBuffer /*acquired*/) noexcept {
  if (!impl_ || impl_->capture_acquired_idx < 0) {
    return;
  }
  auto const idx = static_cast<std::uint32_t>(impl_->capture_acquired_idx);
  impl_->capture_acquired_idx = -1;
  // Best-effort re-QBUF. release() must be infallible per the
  // LayerBufferSource contract; if the kernel rejects the QBUF
  // (degraded queue state), the slot stays unqueued and the source
  // is down one buffer for the rest of its lifetime. drive() can't
  // re-introduce it because it was never with the kernel after we
  // dequeued it.
  auto const requeue_ec =
      queue_capture_buffer(impl_->v4l2_fd, impl_->is_mplane, idx, impl_->capture_num_planes);
  (void)requeue_ec;
}

SourceFormat V4l2DecoderSource::format() const noexcept {
  return impl_ ? impl_->capture_format : SourceFormat{};
}

void V4l2DecoderSource::on_session_paused() noexcept {
  if (!impl_) {
    return;
  }
  // The seat is losing master and the DRM fd is about to be revoked.
  // Drop every fb_id + GEM handle bound to it without ioctls -- those
  // would race against revocation. The V4L2 dmabuf fds are unaffected
  // (V4L2 isn't tied to DRM master), so the source's CPU-side buffer
  // ring stays intact for the next on_session_resumed re-import.
  for (auto& slot : impl_->capture_buffers) {
    slot.drm_fd = -1;  // disarm dtor's KMS-side cleanup
    slot.fb_id = 0;
    slot.drm_handles.fill(0);
  }
  impl_->drm_fd = -1;
}

drm::expected<void, std::error_code> V4l2DecoderSource::on_session_resumed(
    const drm::Device& new_dev) {
  if (!impl_) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  int const new_drm_fd = new_dev.fd();
  if (new_drm_fd < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  // Re-derive the DRM plane layout against the cached V4L2 format
  // echo (same fourcc + dimensions; only the importing fd changed).
  DrmPlaneLayout layout{};
  if (auto ec = derive_drm_plane_layout(impl_->capture_format_echo, impl_->is_mplane,
                                        impl_->cfg.capture_fourcc, layout);
      ec) {
    return drm::unexpected<std::error_code>(ec);
  }

  for (auto& slot : impl_->capture_buffers) {
    if (auto ec = import_capture_buffer_to_drm(new_drm_fd, impl_->capture_format.width,
                                               impl_->capture_format.height,
                                               impl_->cfg.capture_fourcc, layout, slot);
        ec) {
      return drm::unexpected<std::error_code>(ec);
    }
  }
  impl_->drm_fd = new_drm_fd;
  return {};
}

}  // namespace drm::scene