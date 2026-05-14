// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// v4l2_camera_source.hpp — LayerBufferSource over a V4L2 CAPTURE-only
// streaming endpoint (UVC webcams, embedded ISPs, vivid).
//
// The source owns the device end-to-end: open + VIDIOC_S_FMT + REQBUFS +
// QBUF every buffer + STREAMON. Frames flow into the scene via
// `acquire()` with latest-frame-wins semantics — older queued frames are
// dropped if a newer one is already ready.
//
// Two memory modes, picked at `create()` time:
//
//   * BufferMode::DmaBufZeroCopy — VIDIOC_REQBUFS(DMABUF) +
//     VIDIOC_EXPBUF per buffer + drmPrimeFDToHandle +
//     drmModeAddFB2WithModifiers. Each V4L2 buffer gets a stable
//     KMS framebuffer ID minted once at create() time. `acquire()`
//     returns whichever buffer the driver most-recently filled.
//     `map()` reports `function_not_supported` — uncompositable.
//     Works on CMA-backed capture (RPi unicam, RK3588 ISP1, vivid with
//     dma-buf export); fails on amdgpu when the producer is a foreign
//     vmalloc-backed dma-buf (UVC), per
//     `reference_amdgpu_rejects_foreign_vmalloc_dmabuf`.
//
//   * BufferMode::MmapCopy — VIDIOC_REQBUFS(MMAP) + mmap per buffer +
//     per-frame memcpy into a DumbBuffer the scene scans out. Universal
//     fallback. `map()` returns the dumb buffer's CPU mapping so the
//     composition fallback can rescue overflow.
//
//   * BufferMode::Auto (default) — probe DMABUF first; on EXPBUF or
//     AddFB2 failure, tear down and fall back to MMAP. The probe
//     committed-once at create(); runtime does not switch modes.
//
// Format scope for v1:
//   * V4L2_PIX_FMT_NV12 → DRM_FORMAT_NV12 (semi-planar 4:2:0). Required.
//   * V4L2_PIX_FMT_YUYV → DRM_FORMAT_YUYV (packed 4:2:2). UVC default.
//   * V4L2 MPLANE (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) supported in
//     addition to single-plane.
//
//   * V4L2_PIX_FMT_YUV420 (planar 4:2:0) deliberately not supported —
//     drmModeAddFB2 EINVALs on 3-DRM-plane YUV420 (see
//     `reference_drm_plane_layout_nv12_only`). Callers wanting 4:2:0
//     should request NV12 from the camera.
//
//   * V4L2_PIX_FMT_MJPEG deliberately not supported here — MJPEG needs
//     decode. Use a `v4l2_decoder_source` chained after a separate
//     MJPEG capture; or do VAAPI in application code (see the camera
//     example).
//
// Fence handling: out of scope for v1. V4L2's fence story
// (V4L2_BUF_FLAG_REQUEST_FD + media-controller request API) is
// driver-uneven, and most capture devices produce ready buffers
// synchronously. `AcquiredBuffer::acquire_fence_fd` stays -1. Matches
// `V4l2DecoderSource`.
//
// Session pause/resume: the DRM-side state (FB IDs, GEM handles) drops
// on pause and re-imports against the new fd on resume. The V4L2 side
// is unaffected by VT switches — the capture fd is not a DRM master fd.
// MMAP-mode dumb buffers are reallocated against the new DRM device.

#pragma once

#include "buffer_source.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <memory>
#include <system_error>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

/// Memory mode for the capture buffers. See file comment for the
/// behavior of each variant.
enum class V4l2CameraBufferMode : std::uint8_t {
  /// Probe DMABUF zero-copy; on failure fall back to MMAP+copy. The
  /// probe runs once at `create()` time and the decision sticks for the
  /// life of the source.
  Auto,
  /// Force VIDIOC_REQBUFS(DMABUF) + VIDIOC_EXPBUF + AddFB2 import. Fail
  /// `create()` if the driver or DRM device won't accept the import.
  /// `map()` returns function_not_supported.
  DmaBufZeroCopy,
  /// Force VIDIOC_REQBUFS(MMAP) + per-frame copy into a dumb buffer.
  /// `map()` returns the dumb buffer's CPU mapping.
  MmapCopy,
};

/// Configuration for `V4l2CameraSource::create`.
struct V4l2CameraConfig {
  /// V4L2 CAPTURE pixel format (V4L2_PIX_FMT_NV12 / _YUYV). Required.
  std::uint32_t pixel_fourcc{0};

  /// Requested capture dimensions. The kernel may snap to a nearby
  /// supported size; inspect `format()` after construction to see what
  /// landed.
  std::uint32_t width{0};
  std::uint32_t height{0};

  /// Number of capture-queue buffers. Must be in [2, 32]. 4 is a
  /// sensible default; latency-sensitive callers can drop to 3.
  std::uint32_t buffer_count{4};

  /// How buffers reach the scene. See `V4l2CameraBufferMode`.
  V4l2CameraBufferMode mode{V4l2CameraBufferMode::Auto};

  /// DRM modifier hint for the DMABUF path. 0 → DRM_FORMAT_MOD_LINEAR.
  /// Non-LINEAR modifiers are passed through to AddFB2 but the kernel
  /// negotiation is driver-uneven; expect LINEAR to be the working
  /// case for v1.
  std::uint64_t modifier{0};
};

/// `LayerBufferSource` over a V4L2 CAPTURE endpoint. See file comment
/// for the full contract.
class V4l2CameraSource : public LayerBufferSource {
 public:
  /// Open `device_path`, negotiate `cfg`, set up buffers (DMABUF or
  /// MMAP depending on `cfg.mode`), STREAMON.
  ///
  /// Validation runs before the device is touched:
  ///   * `device_path` non-null and non-empty,
  ///   * `cfg.pixel_fourcc` non-zero AND one of the supported FourCCs,
  ///   * `cfg.width` / `cfg.height` non-zero,
  ///   * `cfg.buffer_count` in [2, 32].
  ///
  /// On failure the partial state is unwound. `dev` is borrowed for
  /// the duration of the call; the source caches a duplicate of the
  /// underlying DRM fd for FB minting.
  [[nodiscard]] static drm::expected<std::unique_ptr<V4l2CameraSource>, std::error_code> create(
      const drm::Device& dev, const char* device_path, const V4l2CameraConfig& cfg);

  V4l2CameraSource(const V4l2CameraSource&) = delete;
  V4l2CameraSource& operator=(const V4l2CameraSource&) = delete;
  V4l2CameraSource(V4l2CameraSource&&) = delete;
  V4l2CameraSource& operator=(V4l2CameraSource&&) = delete;
  ~V4l2CameraSource() override;

  /// Pollable fd. Add to a libuv / epoll / `drm::PageFlip` loop; when
  /// readable, call `drive()` to dequeue completed buffers.
  [[nodiscard]] int fd() const noexcept;

  /// Drain pending CAPTURE buffers without blocking. Buffers older
  /// than the most recent ready one are dropped (latest-frame-wins),
  /// matching `V4l2DecoderSource::drive`.
  drm::expected<void, std::error_code> drive() noexcept;

  /// Which mode the source actually settled on. With `mode = Auto`,
  /// this reports the path that won the probe; with an explicit mode,
  /// it echoes the request.
  [[nodiscard]] V4l2CameraBufferMode active_mode() const noexcept;

  // ── LayerBufferSource ────────────────────────────────────────────────

  [[nodiscard]] drm::expected<AcquiredBuffer, std::error_code> acquire() override;
  void release(AcquiredBuffer acquired) noexcept override;
  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return BindingModel::SceneSubmitsFbId;
  }
  [[nodiscard]] SourceFormat format() const noexcept override;

  /// `MmapCopy` mode: returns the active dumb buffer's CPU mapping so
  /// composition fallback can pull pixels. `DmaBufZeroCopy` mode:
  /// returns `function_not_supported` (capture pixels live in a
  /// foreign buffer the scene can't safely touch).
  [[nodiscard]] drm::expected<drm::BufferMapping, std::error_code> map(
      drm::MapAccess access) override;

  void on_session_paused() noexcept override;
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override;

 private:
  V4l2CameraSource();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace drm::scene