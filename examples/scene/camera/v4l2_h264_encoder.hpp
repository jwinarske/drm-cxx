// SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// v4l2_h264_encoder.hpp — hardware H.264 encode via a V4L2 stateful M2M
// encoder (e.g. the Raspberry Pi's bcm2835-codec, RK3588 rkvenc, etc.).
//
// The mirror of drm::scene::V4l2DecoderSource: an OUTPUT queue takes raw
// frames (YUYV / NV12) and a CAPTURE queue produces the H.264 elementary
// stream. Unlike the VA-API path, a V4L2 stateful encoder generates its own
// bitstream headers — SPS/PPS are inserted with each keyframe (via the
// repeat-sequence-header control) — so this class just feeds frames in and
// appends the coded chunks out, ready to write to a `.h264` file.
//
// Multiplanar M2M. The CAPTURE queue is MMAP (coded chunks copied out). The
// OUTPUT queue takes raw frames either by copy into an MMAP buffer (encode())
// or, when the source owns a dma-buf, by zero-copy import (encode_dmabuf(),
// enabled with Config::dmabuf_output). Baseline/no B-frames, so one coded frame
// comes out per raw frame in.
//
// No third-party dependency — this is plain V4L2, so it builds anywhere the
// kernel headers are present; whether a device actually exists is a runtime
// concern.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <vector>

namespace drm::examples::camera {

/// Hardware H.264 encoder over a V4L2 stateful M2M device.
class V4l2H264Encoder {
 public:
  struct Config {
    const char* device{"/dev/video11"};  // the bcm2835-codec encoder on a Pi
    std::uint32_t in_fourcc{0};          // V4L2_PIX_FMT_YUYV / _NV12 (0 => YUYV)
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t fps{30};
    std::uint32_t bitrate_bps{0};  // 0 => a resolution-scaled default
    std::uint32_t gop{0};          // I-frame period in frames; 0 => fps * 2
    // Import raw frames into the OUTPUT queue as dma-buf (V4L2_MEMORY_DMABUF)
    // instead of memcpy-ing into MMAP buffers — zero-copy when the capture
    // source already owns a dma-buf (e.g. a CSI camera's CMA buffer). Requires
    // encode_dmabuf() rather than encode(). Falls to MMAP if the driver rejects
    // DMABUF request at setup; a cross-device import a codec can't map (e.g. a
    // UVC camera's vmalloc buffer) fails later at encode_dmabuf() time instead.
    bool dmabuf_output{false};
  };

  /// Open + configure the encoder. Returns nullptr with `ec` set on any
  /// failure (missing device, unsupported format, ioctl error) — the caller
  /// should fall back or disable recording. `width`/`height` must be non-zero.
  [[nodiscard]] static std::unique_ptr<V4l2H264Encoder> create(const Config& cfg,
                                                               std::error_code* ec = nullptr);

  V4l2H264Encoder(const V4l2H264Encoder&) = delete;
  V4l2H264Encoder& operator=(const V4l2H264Encoder&) = delete;
  V4l2H264Encoder(V4l2H264Encoder&&) = delete;
  V4l2H264Encoder& operator=(V4l2H264Encoder&&) = delete;
  ~V4l2H264Encoder();

  /// Encode one raw frame (`in_fourcc`, tightly packed at the configured
  /// size). Appends the coded chunk — SPS + PPS + IDR slice on a keyframe, a
  /// P slice otherwise — to `out`. Returns false on a device error (drop the
  /// frame). `size` is the frame's byte length; it is copied into the OUTPUT
  /// buffer.
  [[nodiscard]] bool encode(const std::uint8_t* frame, std::size_t size,
                            std::vector<std::uint8_t>& out) noexcept;

  /// Encode one raw frame supplied as a dma-buf, imported straight into the
  /// OUTPUT queue with no copy. `dmabuf_fd` is the frame's dma-buf (Y at
  /// `data_offset`; NV12 UV assumed contiguous, matching a single-fd multiplane
  /// buffer). Only valid when the encoder was built with `dmabuf_output`.
  /// Returns false on a device error — notably an import the codec cannot map,
  /// which the caller should treat as "fall back to the copy path".
  [[nodiscard]] bool encode_dmabuf(int dmabuf_fd, std::size_t data_offset,
                                   std::vector<std::uint8_t>& out) noexcept;

  /// True when the OUTPUT queue was successfully set up for dma-buf import
  /// (so encode_dmabuf() is the path to use rather than encode()).
  [[nodiscard]] bool imports_dmabuf() const noexcept { return output_dmabuf_; }

  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

 private:
  V4l2H264Encoder() = default;
  void destroy_state() noexcept;

  /// Dequeue one coded frame from the CAPTURE queue (baseline is 1-in-1-out),
  /// append it to `out`, and requeue the buffer. Shared by both encode paths.
  [[nodiscard]] bool drain_capture(std::vector<std::uint8_t>& out) noexcept;

  struct Plane {
    std::uint8_t* start = nullptr;
    std::size_t length = 0;
  };

  int fd_ = -1;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::vector<Plane> out_bufs_;      // OUTPUT (raw) MMAP planes, index == buffer index
  std::vector<Plane> cap_bufs_;      // CAPTURE (H.264) MMAP planes
  std::uint32_t out_queued_ = 0;     // raw buffers handed to the kernel so far
  std::uint32_t out_count_ = 0;      // OUTPUT buffer slots (dma-buf path has no mmap)
  std::uint32_t out_sizeimage_ = 0;  // negotiated OUTPUT frame size (dma-buf QBUF length)
  bool output_dmabuf_ = false;       // OUTPUT queue imports dma-buf vs memcpy MMAP
  bool streaming_ = false;
};

}  // namespace drm::examples::camera
