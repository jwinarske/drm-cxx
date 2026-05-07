// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// uvc_capture.hpp — small, un-opinionated wrapper around a V4L2
// CAPTURE-only single-plane streaming endpoint (the UVC class shape).
//
// What it does:
//   * Walks /dev/video* for a single-plane CAPTURE-only device that
//     advertises a chosen FourCC (typically YUYV / V4L2_PIX_FMT_YUYV).
//     M2M / OUTPUT-only nodes (vicodec) and metadata / radio / SDR
//     nodes are skipped.
//   * Opens + S_FMT(width, height, fourcc) + REQBUFS + MMAP + QBUF
//     every buffer + STREAMON. Buffers are V4L2_MEMORY_MMAP so the
//     caller's read access is to a kernel-page-aligned mmap region;
//     no PRIME-import is involved.
//   * Per frame: `try_acquire_frame()` dequeues one buffer and hands
//     back a `Frame` view (pointer + V4L2 stride + dims + buffer
//     index). The frame stays valid until the caller calls
//     `release_frame(buffer_index)` to requeue it.
//
// Deliberately not opinionated:
//   * No conversion. The caller decides how to consume the YUYV (or
//     other) bytes — libyuv to ARGB, libyuv to NV12, software
//     converter, hand to a GPU shader. cluster_sim does YUY2->XRGB
//     into a DumbBufferSource; a future example might do YUY2->NV12
//     into a GBM buffer.
//   * No fixed FourCC. YUYV is the universal UVC fallback, but the
//     same plumbing works for MJPEG (decode separately), NV12-on-
//     CAPTURE, etc.
//   * No fixed buffer count. Callers pick — 4 is a sensible default.
//   * No DRM dependency. The helper compiles against libc + Linux V4L2
//     headers only.
//
// Header-only by intent, mirroring open_output.hpp / vt_switch.hpp.

#pragma once

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/format.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <linux/videodev2.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace drm::examples {

/// One dequeued frame from a `UvcCapture`. The data pointer is borrowed
/// from the kernel's mmap region; valid until the corresponding
/// `release_frame(buffer_index)` requeues the buffer.
struct UvcFrame {
  std::uint8_t const* data{nullptr};
  std::size_t bytes{0};
  std::uint32_t bytesperline{0};  // V4L2 stride; rows may be padded
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t buffer_index{0};
};

/// V4L2 single-plane CAPTURE-only streaming endpoint. RAII move-only.
/// `create()` opens + configures + STREAMON's; the destructor / `close()`
/// STREAMOFF's, REQBUFS=0's, munmap's, and closes the fd. Idempotent.
class UvcCapture {
 public:
  static constexpr std::size_t kMaxBuffers = 8;

  /// Walk /dev/video* for the first CAPTURE-only single-plane device
  /// that advertises `fourcc`. Returns the path or nullopt. Errors
  /// (EACCES on a node, etc.) are swallowed silently — the caller
  /// just sees "not found".
  [[nodiscard]] static std::optional<std::string> find_endpoint(std::uint32_t fourcc) {
    std::error_code ec;
    for (auto const& entry : std::filesystem::directory_iterator("/dev", ec)) {
      auto const& p = entry.path();
      std::string const name = p.filename().string();
      if (name.rfind("video", 0) != 0) {
        continue;
      }
      int const fd = ::open(p.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
      if (fd < 0) {
        continue;
      }
      v4l2_capability cap{};
      if (xioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
        ::close(fd);
        continue;
      }
      std::uint32_t const caps =
          ((cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U) ? cap.device_caps : cap.capabilities;
      bool const is_capture = (caps & V4L2_CAP_VIDEO_CAPTURE) != 0U;
      bool const is_m2m = (caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)) != 0U;
      bool const has_streaming = (caps & V4L2_CAP_STREAMING) != 0U;
      if (!is_capture || is_m2m || !has_streaming) {
        ::close(fd);
        continue;
      }
      bool advertises = false;
      for (std::uint32_t i = 0; i < 64; ++i) {
        v4l2_fmtdesc desc{};
        desc.index = i;
        desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd, VIDIOC_ENUM_FMT, &desc) != 0) {
          break;
        }
        if (desc.pixelformat == fourcc) {
          advertises = true;
          break;
        }
      }
      ::close(fd);
      if (advertises) {
        return p.string();
      }
    }
    return std::nullopt;
  }

  /// Open `path`, S_FMT(`fourcc`, `want_w`, `want_h`), REQBUFS
  /// `buffer_count` (capped at `kMaxBuffers`), MMAP and QBUF every
  /// buffer, STREAMON. The kernel may snap dimensions and stride;
  /// inspect `width()` / `height()` / `bytesperline()` on success.
  ///
  /// Returns an `errc::invalid_argument` failure if the kernel
  /// returned a different FourCC than requested (no automatic
  /// fallback — let the caller try a different format).
  [[nodiscard]] static drm::expected<UvcCapture, std::error_code> create(
      std::string_view path, std::uint32_t fourcc, std::uint32_t want_w, std::uint32_t want_h,
      std::size_t buffer_count = 4) {
    UvcCapture cap;
    cap.fd_ = ::open(std::string(path).c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (cap.fd_ < 0) {
      return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
    }
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.pixelformat = fourcc;
    fmt.fmt.pix.width = want_w;
    fmt.fmt.pix.height = want_h;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (int const e = xioctl(cap.fd_, VIDIOC_S_FMT, &fmt); e != 0) {
      return drm::unexpected<std::error_code>(std::error_code(e, std::system_category()));
    }
    if (fmt.fmt.pix.pixelformat != fourcc) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
    }
    cap.width_ = fmt.fmt.pix.width;
    cap.height_ = fmt.fmt.pix.height;
    cap.bytesperline_ = fmt.fmt.pix.bytesperline;
    cap.fourcc_ = fmt.fmt.pix.pixelformat;

    v4l2_requestbuffers req{};
    req.count = static_cast<std::uint32_t>(std::min<std::size_t>(buffer_count, kMaxBuffers));
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (int const e = xioctl(cap.fd_, VIDIOC_REQBUFS, &req); e != 0 || req.count == 0) {
      auto const ec = (e != 0) ? std::error_code(e, std::system_category())
                               : std::make_error_code(std::errc::no_buffer_space);
      return drm::unexpected<std::error_code>(ec);
    }
    cap.buffer_count_ = std::min<std::size_t>(req.count, kMaxBuffers);
    for (std::uint32_t i = 0; i < cap.buffer_count_; ++i) {
      v4l2_buffer qb{};
      qb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      qb.memory = V4L2_MEMORY_MMAP;
      qb.index = i;
      if (int const e = xioctl(cap.fd_, VIDIOC_QUERYBUF, &qb); e != 0) {
        return drm::unexpected<std::error_code>(std::error_code(e, std::system_category()));
      }
      void* const m =
          ::mmap(nullptr, qb.length, PROT_READ | PROT_WRITE, MAP_SHARED, cap.fd_, qb.m.offset);
      if (m == MAP_FAILED) {
        return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
      }
      cap.mapped_.at(i) = m;
      cap.mapped_len_.at(i) = qb.length;
      if (int const e = xioctl(cap.fd_, VIDIOC_QBUF, &qb); e != 0) {
        return drm::unexpected<std::error_code>(std::error_code(e, std::system_category()));
      }
    }
    int t = static_cast<int>(V4L2_BUF_TYPE_VIDEO_CAPTURE);
    if (int const e = xioctl(cap.fd_, VIDIOC_STREAMON, &t); e != 0) {
      return drm::unexpected<std::error_code>(std::error_code(e, std::system_category()));
    }
    cap.streaming_ = true;
    return cap;
  }

  UvcCapture() noexcept = default;

  UvcCapture(UvcCapture&& other) noexcept { steal(other); }
  UvcCapture& operator=(UvcCapture&& other) noexcept {
    if (this != &other) {
      close();
      steal(other);
    }
    return *this;
  }
  UvcCapture(UvcCapture const&) = delete;
  UvcCapture& operator=(UvcCapture const&) = delete;
  ~UvcCapture() { close(); }

  /// pollable fd; -1 if not open.
  [[nodiscard]] int fd() const noexcept { return fd_; }
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] std::uint32_t bytesperline() const noexcept { return bytesperline_; }
  [[nodiscard]] std::uint32_t fourcc() const noexcept { return fourcc_; }

  /// Try to dequeue one buffer. Returns nullopt when none ready
  /// (EAGAIN — caller's poll() beat us, or a transient ioctl error).
  /// The borrowed frame stays valid until `release_frame(index)` is
  /// called with the matching `buffer_index`.
  [[nodiscard]] std::optional<UvcFrame> try_acquire_frame() noexcept {
    if (fd_ < 0) {
      return std::nullopt;
    }
    v4l2_buffer dq{};
    dq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    dq.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_DQBUF, &dq) != 0) {
      return std::nullopt;
    }
    if (dq.index >= buffer_count_ || mapped_.at(dq.index) == nullptr) {
      // Inconsistent state — requeue and bail.
      release_frame(dq.index);
      return std::nullopt;
    }
    UvcFrame frame{};
    frame.data = static_cast<std::uint8_t const*>(mapped_.at(dq.index));
    frame.bytes = mapped_len_.at(dq.index);
    frame.bytesperline = bytesperline_;
    frame.width = width_;
    frame.height = height_;
    frame.buffer_index = dq.index;
    return frame;
  }

  /// Requeue the buffer identified by `buffer_index`. Errors are
  /// silently dropped — the next DQBUF will surface them.
  void release_frame(std::uint32_t buffer_index) noexcept {
    if (fd_ < 0) {
      return;
    }
    v4l2_buffer qb{};
    qb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    qb.memory = V4L2_MEMORY_MMAP;
    qb.index = buffer_index;
    (void)xioctl(fd_, VIDIOC_QBUF, &qb);
  }

  /// Idempotent teardown. The destructor calls this; callers can
  /// invoke it explicitly to release the device fd before the
  /// containing object goes out of scope.
  void close() noexcept {
    if (fd_ >= 0 && streaming_) {
      int t = static_cast<int>(V4L2_BUF_TYPE_VIDEO_CAPTURE);
      (void)xioctl(fd_, VIDIOC_STREAMOFF, &t);
      streaming_ = false;
    }
    for (std::size_t i = 0; i < kMaxBuffers; ++i) {
      if (mapped_.at(i) != nullptr) {
        ::munmap(mapped_.at(i), mapped_len_.at(i));
        mapped_.at(i) = nullptr;
        mapped_len_.at(i) = 0;
      }
    }
    if (fd_ >= 0) {
      v4l2_requestbuffers zero{};
      zero.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      zero.memory = V4L2_MEMORY_MMAP;
      (void)xioctl(fd_, VIDIOC_REQBUFS, &zero);
      ::close(fd_);
      fd_ = -1;
    }
    buffer_count_ = 0;
    width_ = 0;
    height_ = 0;
    bytesperline_ = 0;
    fourcc_ = 0;
  }

 private:
  /// EINTR-retrying ioctl. Returns 0 on success, errno on failure.
  /// Mirrors the helper inline in cluster_sim and V4l2DecoderSource;
  /// kept private so consumers don't reach in.
  static int xioctl(int fd, unsigned long request, void* arg) noexcept {
    constexpr int kMaxRetries = 8;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
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

  void steal(UvcCapture& other) noexcept {
    fd_ = other.fd_;
    mapped_ = other.mapped_;
    mapped_len_ = other.mapped_len_;
    buffer_count_ = other.buffer_count_;
    width_ = other.width_;
    height_ = other.height_;
    bytesperline_ = other.bytesperline_;
    fourcc_ = other.fourcc_;
    streaming_ = other.streaming_;
    other.fd_ = -1;
    other.mapped_ = {};
    other.mapped_len_ = {};
    other.buffer_count_ = 0;
    other.streaming_ = false;
  }

  int fd_{-1};
  std::array<void*, kMaxBuffers> mapped_{};
  std::array<std::size_t, kMaxBuffers> mapped_len_{};
  std::size_t buffer_count_{0};
  std::uint32_t width_{0};
  std::uint32_t height_{0};
  std::uint32_t bytesperline_{0};
  std::uint32_t fourcc_{0};
  bool streaming_{false};
};

}  // namespace drm::examples