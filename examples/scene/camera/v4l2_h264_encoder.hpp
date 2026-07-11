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
// Multiplanar M2M, MMAP buffers on both queues (one CPU copy of the frame
// into the OUTPUT buffer; the CAPTURE buffer is copied out). Baseline/no
// B-frames, so one coded frame comes out per raw frame in.
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

  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

 private:
  V4l2H264Encoder() = default;
  void destroy_state() noexcept;

  struct Plane {
    std::uint8_t* start = nullptr;
    std::size_t length = 0;
  };

  int fd_ = -1;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::vector<Plane> out_bufs_;   // OUTPUT (raw) MMAP planes, index == buffer index
  std::vector<Plane> cap_bufs_;   // CAPTURE (H.264) MMAP planes
  std::uint32_t out_queued_ = 0;  // raw buffers handed to the kernel so far
  bool streaming_ = false;
};

}  // namespace drm::examples::camera
