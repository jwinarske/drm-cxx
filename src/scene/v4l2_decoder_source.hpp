// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// v4l2_decoder_source.hpp — LayerBufferSource that drives a stateful
// V4L2 video decoder (`/dev/video-decN` style, MPLANE) and exposes the
// most-recently-decoded CAPTURE buffer as a scanout-ready KMS
// framebuffer.
//
// The source owns the decoder's CAPTURE side end-to-end: REQBUFS+MMAP,
// VIDIOC_EXPBUF to a DMA-BUF fd, drmPrimeFDToHandle on the caller's
// drm::Device, drmModeAddFB2WithModifiers to a stable per-buffer fb_id.
// `acquire()` returns the most-recently-completed CAPTURE buffer and
// drops anything older that was sitting in the queue (latest-frame-wins
// semantics — see roadmap §225). `release()` re-queues the buffer.
//
// The OUTPUT side (the bitstream that gets *into* the decoder) is the
// caller's responsibility. The caller pumps coded frames in via
// `submit_bitstream(span<uint8_t> coded, uint64_t pts_ns)`; the source
// MMAPs OUTPUT buffers internally and copies the bitstream in. Coupling
// to a demuxer (FFmpeg, GStreamer, a Matroska reader, raw Annex-B
// streaming) is left to the caller — the source is the V4L2 boundary,
// not a media pipeline. See roadmap §231 for the integration-pattern
// rationale.
//
// Format scope:
//   * Codec input: any V4L2 OUTPUT pixel format the decoder advertises
//     (V4L2_PIX_FMT_H264, _HEVC, _VP9, _AV1, _MJPEG, …). Caller
//     specifies; the source does not probe.
//   * Decoded output: any V4L2 CAPTURE pixel format the decoder
//     produces, mapped through to a DRM FourCC. NV12 + LINEAR is the
//     primary path; tiled/AFBC modifiers are out of scope for v1
//     (same restriction as ExternalDmaBufSource — the kernel's
//     plane-format negotiation around modifiers is too driver-specific
//     to validate up front).
//
// Resolution change handling: when the bitstream's resolution changes
// mid-stream the decoder fires V4L2_EVENT_SOURCE_CHANGE. The source
// surfaces this as an error from `drive()` and stops producing frames;
// callers destroy the source and create a new one with the new
// dimensions. Reallocating CAPTURE buffers in place would invalidate
// every cached fb_id and re-import path, which is feasible but adds
// failure modes the v1 surface doesn't need.
//
// Fence handling: out of scope for v1. V4L2's fence story
// (V4L2_BUF_FLAG_REQUEST_FD + media-controller request API) is
// driver-uneven, and most stateful decoders just produce ready buffers
// synchronously. `AcquiredBuffer::acquire_fence_fd` stays -1.
//
// Session pause/resume: the source drops its DRM-side state (FB IDs,
// GEM handles) on pause and re-imports CAPTURE buffers' DMA-BUF fds
// against the new fd on resume. The V4L2 side is unaffected by VT
// switches — the decoder fd is not a DRM master fd.

#pragma once

#include "buffer_source.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

/// Configuration for `V4l2DecoderSource::create`. Required fields have
/// no default; defaulted fields use values that are reasonable for a
/// 1080p H.264 decode against a typical embedded SoC.
struct V4l2DecoderConfig {
  /// V4L2 OUTPUT pixel format — the codec the decoder is being asked
  /// to consume. e.g. V4L2_PIX_FMT_H264 (`'H','2','6','4'`).
  std::uint32_t codec_fourcc{0};

  /// V4L2 CAPTURE pixel format — what the decoder will produce. NV12
  /// (V4L2_PIX_FMT_NV12) is the universal path; vendor tiled formats
  /// are not supported in v1.
  std::uint32_t capture_fourcc{0};

  /// Maximum coded-stream dimensions the source will be asked to
  /// decode. The decoder uses these to size OUTPUT buffers. A larger
  /// value uses more memory but lets the source survive
  /// resolution-up-bumps without reallocating; an exact value is fine
  /// for fixed-resolution streams.
  std::uint32_t coded_width{0};
  std::uint32_t coded_height{0};

  /// Number of OUTPUT buffers (compressed bitstream chunks in flight
  /// to the decoder). Must be >= 2 so the caller can pump while the
  /// decoder works.
  std::uint32_t output_buffer_count{4};

  /// Number of CAPTURE buffers (decoded frames in flight back to the
  /// scene). Must be >= 2; >= 4 is recommended so a slow consumer
  /// doesn't stall the decoder.
  std::uint32_t capture_buffer_count{4};

  /// Per-OUTPUT-buffer size in bytes. 0 lets the decoder pick (most
  /// drivers report `sizeimage` from the negotiated format). Override
  /// when feeding unusually large compressed frames (lossless H.264,
  /// keyframe-heavy AV1, etc.).
  std::size_t output_buffer_size{0};
};

/// `LayerBufferSource` driving a V4L2 stateful decoder. See file
/// comment for the full contract.
class V4l2DecoderSource : public LayerBufferSource {
 public:
  /// Open the decoder at `device_path`, negotiate formats per `cfg`,
  /// allocate buffers on both queues, export every CAPTURE buffer as
  /// a DMA-BUF, and import each into `dev` as a KMS FB.
  ///
  /// Argument validation runs before the device is touched:
  ///   * `device_path` non-null and non-empty,
  ///   * `cfg.codec_fourcc` and `cfg.capture_fourcc` non-zero,
  ///   * `cfg.coded_width` and `cfg.coded_height` non-zero,
  ///   * `cfg.output_buffer_count` and `cfg.capture_buffer_count`
  ///     each in [2, 32].
  ///
  /// On any failure the partial state (open fds, mmaps, GEM handles)
  /// is unwound before returning. The caller's `dev` is borrowed for
  /// the duration of the call only; the source caches its own dup of
  /// the underlying DRM fd via `drm::Device::dup`-equivalent semantics.
  [[nodiscard]] static drm::expected<std::unique_ptr<V4l2DecoderSource>, std::error_code> create(
      const drm::Device& dev, const char* device_path, const V4l2DecoderConfig& cfg);

  V4l2DecoderSource(const V4l2DecoderSource&) = delete;
  V4l2DecoderSource& operator=(const V4l2DecoderSource&) = delete;
  V4l2DecoderSource(V4l2DecoderSource&&) = delete;
  V4l2DecoderSource& operator=(V4l2DecoderSource&&) = delete;
  ~V4l2DecoderSource() override;

  // ── Caller API: bitstream feeding ────────────────────────────────────

  /// File descriptor the caller polls for "decoder has work to report".
  /// Add to a libuv / epoll / `drm::PageFlip::add_source` loop; when
  /// readable, call `drive()`.
  [[nodiscard]] int fd() const noexcept;

  /// Drain pending V4L2 events without blocking:
  ///   * dequeue completed OUTPUT buffers (returns them to the source's
  ///     free list so `submit_bitstream` can fill them again);
  ///   * dequeue newly-decoded CAPTURE buffers (parks them in the
  ///     ready-to-acquire list, dropping any prior pending entry —
  ///     latest-frame-wins);
  ///   * surface V4L2_EVENT_SOURCE_CHANGE as
  ///     `errc::operation_canceled` (caller must destroy + recreate).
  ///
  /// Returns success when there is no error to report, regardless of
  /// whether any buffers actually moved.
  drm::expected<void, std::error_code> drive() noexcept;

  /// Queue a coded bitstream chunk on the OUTPUT side. The bytes are
  /// memcpy'd into a free OUTPUT buffer and queued (`VIDIOC_QBUF`);
  /// `coded.size()` must be <= the OUTPUT buffer size.
  ///
  /// `timestamp_ns` becomes the V4L2 timestamp on the OUTPUT buffer
  /// and propagates through to the matching CAPTURE buffer's
  /// timestamp — callers tracking PTS use it as a frame ID. Pass 0
  /// when timestamps are not relevant.
  ///
  /// Returns `errc::resource_unavailable_try_again` when no OUTPUT
  /// buffer is currently free; the caller waits for `fd()` to become
  /// readable, calls `drive()` to dequeue completed buffers, and
  /// retries.
  drm::expected<void, std::error_code> submit_bitstream(drm::span<const std::uint8_t> coded,
                                                        std::uint64_t timestamp_ns = 0);

  // ── LayerBufferSource ────────────────────────────────────────────────

  [[nodiscard]] drm::expected<AcquiredBuffer, std::error_code> acquire() override;
  void release(AcquiredBuffer acquired) noexcept override;
  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return BindingModel::SceneSubmitsFbId;
  }
  [[nodiscard]] SourceFormat format() const noexcept override;

  // map() inherits the base default — decoder CAPTURE buffers are not
  // generally CPU-mappable in a useful way (NV12 with vendor strides,
  // potentially in carveout memory). Layers backed by a
  // V4l2DecoderSource that the allocator can't place on a hardware
  // plane will be dropped this frame; the composition fallback cannot
  // rescue them.

  void on_session_paused() noexcept override;
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override;

 private:
  V4l2DecoderSource();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace drm::scene
