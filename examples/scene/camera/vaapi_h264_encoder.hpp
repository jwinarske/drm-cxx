// SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// vaapi_h264_encoder.hpp — hardware H.264 encode for the camera example's
// recording consumer.
//
// Wraps a VA-API encode context (VAEntrypointEncSlice) on a borrowed
// VADisplay — the same display the MJPEG decoder opened, so the decoder's
// NV12 output surface can be encoded with no export/import. Takes NV12
// frames (an existing VASurface, or CPU planes it uploads) and appends an
// Annex-B H.264 elementary stream to a caller-owned buffer, ready to write
// to a `.h264` file and play with ffplay/mpv.
//
// Coding: Constrained Baseline profile, constant-QP rate control, an IPPP
// GOP with a periodic IDR (single reference — each P frame references the
// immediately preceding reconstructed frame). SPS/PPS are emitted as packed
// headers on every IDR so the stream is self-contained and seekable to each
// keyframe. No B-frames, no CPU pixel touch on the surface path.
//
// Build is gated on `libva` + `libva-drm` (CAMERA_HAS_VAAPI=1), the same gate
// as vaapi_jpeg_decoder; the header is safe to include unguarded but the
// symbols are only defined when the gate is on.

#pragma once

#include <cstdint>
#include <memory>
#include <system_error>
#include <vector>

namespace drm::examples::camera {

/// Hardware H.264 encoder backed by VA-API. Holds an encode config/context,
/// an upload surface, a two-entry reconstructed-surface pool (current +
/// reference), and a coded-output buffer for its lifetime.
class VaapiH264Encoder {
 public:
  struct Config {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t fps{30};  // written into the SPS VUI timing
    std::uint32_t gop{0};   // IDR interval in frames; 0 => fps * 2
    std::uint32_t qp{26};   // constant QP, 0..51 (lower = higher quality/bitrate)
  };

  /// Build an encoder bound to `va_display` (borrowed; opened once per
  /// process via VaapiJpegDecoder::open_display). Returns nullptr with `ec`
  /// set on any VA-API failure — the caller should disable recording, not
  /// abort. `cfg.width`/`cfg.height` must be non-zero; other fields default.
  [[nodiscard]] static std::unique_ptr<VaapiH264Encoder> create(void* va_display, const Config& cfg,
                                                                std::error_code* ec = nullptr);

  VaapiH264Encoder(const VaapiH264Encoder&) = delete;
  VaapiH264Encoder& operator=(const VaapiH264Encoder&) = delete;
  VaapiH264Encoder(VaapiH264Encoder&&) = delete;
  VaapiH264Encoder& operator=(VaapiH264Encoder&&) = delete;
  ~VaapiH264Encoder();

  /// Encode one NV12 VA surface (borrowed; must be finished/synced by the
  /// caller and stay valid for the call). Appends this frame's Annex-B NAL
  /// units — SPS + PPS + IDR slice on a keyframe, a single P slice otherwise
  /// — to `out`. Returns false on a VA-API error (treat as a dropped frame).
  [[nodiscard]] bool encode_surface(unsigned int nv12_surface,
                                    std::vector<std::uint8_t>& out) noexcept;

  /// Upload CPU NV12 (`y` plane at `y_stride`, interleaved `uv` at
  /// `uv_stride`) into the encoder's own input surface, then encode it. For
  /// camera tiers that don't already hold a VA surface. One upload copy.
  [[nodiscard]] bool encode_nv12(const std::uint8_t* y, const std::uint8_t* uv,
                                 std::uint32_t y_stride, std::uint32_t uv_stride,
                                 std::vector<std::uint8_t>& out) noexcept;

  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

 private:
  VaapiH264Encoder() = default;

  /// Core encode of a source surface (the upload path funnels through here).
  [[nodiscard]] bool encode_source(unsigned int src_surface,
                                   std::vector<std::uint8_t>& out) noexcept;

  /// Tear down every VA-API resource we own. Idempotent; never touches the
  /// borrowed display.
  void destroy_state() noexcept;

  // Opaque holders (VADisplay / VAConfigID / VAContextID / VASurfaceID /
  // VABufferID are uintptr_t / unsigned-int aliases inside <va/va.h>; kept as
  // void*/unsigned int so this header doesn't drag the VA headers in).
  void* va_display_ = nullptr;
  unsigned int va_config_ = 0xffffffffU;
  unsigned int va_context_ = 0xffffffffU;
  unsigned int va_input_surface_ = 0xffffffffU;            // upload target (encode_nv12)
  unsigned int va_recon_[2] = {0xffffffffU, 0xffffffffU};  // reconstructed / reference pool
  unsigned int va_coded_buffer_ = 0xffffffffU;             // VAEncCodedBufferType

  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::uint32_t fps_ = 30;
  std::uint32_t gop_ = 60;
  std::uint32_t qp_ = 26;

  // GOP / reference state, advanced per encoded frame.
  std::uint32_t frame_in_gop_ = 0;  // 0 => this frame is an IDR
  int cur_recon_ = 0;               // va_recon_ slot written this frame
  bool have_ref_ = false;           // a prior reconstructed frame exists as a P reference
  std::uint16_t frame_num_ = 0;     // H.264 frame_num (resets at IDR)
  std::uint32_t idr_pic_id_ = 0;    // increments per IDR
  std::int32_t poc_ = 0;            // pic_order_cnt_lsb source (resets at IDR)
};

}  // namespace drm::examples::camera
