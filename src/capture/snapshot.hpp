// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// capture/snapshot.hpp — read back the current plane composition of a
// CRTC into an in-memory ARGB8888 image.
//
// What snapshot() does:
//   - Enumerates the planes bound to `crtc_id` on the given Device.
//   - For each plane, fetches the scanout FB via drmModeGetFB2, checks
//     the format + modifier, reads the pixels back via DMA-BUF mmap,
//     and composites them (in zpos order) into an output Image sized
//     to the CRTC's active mode.
//   - Returns the Image, ready to hand to write_png() or inspect in
//     user code.
//
// What snapshot() does NOT do (V1 limitations, per docs/blend2d_plan.md):
//   - Tiled / AFBC / DCC / compressed modifiers — skipped with a warning.
//   - YUV formats (NV12 and friends) — skipped with a warning. Real
//     video overlays are usually both tiled AND YUV, so covering them
//     means GPU-assisted readback; that's V2 territory.
//   - Planes the caller lacks read access to — returns an error.
//
// Image is a plain value type holding premultiplied ARGB8888 pixels
// (matching Blend2D's BL_FORMAT_PRGB32 in-memory layout on LE hosts:
// one uint32 per pixel, 0xAARRGGBB). It is move-only; construct it
// empty or with explicit dimensions, then either receive pixels from
// snapshot() or fill them directly (tests, synthetic inputs).
//
// The Image type carries no Blend2D types — consumers who only pass
// an Image through write_png() do not need the blend2d headers.

#pragma once

#include "core/device.hpp"
#include "detail/expected.hpp"
#include "detail/span.hpp"

#include <cstdint>
#include <system_error>
#include <vector>

namespace drm::capture {

/// An in-memory ARGB8888 image. Premultiplied alpha (Blend2D PRGB32
/// convention). Row-contiguous; `stride_bytes() == width() * 4`.
class Image {
 public:
  Image() = default;

  /// Construct an all-zero image of the given dimensions. `width == 0`
  /// or `height == 0` yields the same state as `Image()`.
  Image(std::uint32_t width, std::uint32_t height);

  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;
  Image(Image&&) noexcept = default;
  Image& operator=(Image&&) noexcept = default;
  ~Image() = default;

  [[nodiscard]] std::uint32_t width() const noexcept { return w_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return h_; }
  [[nodiscard]] std::uint32_t stride_bytes() const noexcept { return w_ * 4; }
  [[nodiscard]] bool empty() const noexcept { return w_ == 0 || h_ == 0; }

  /// Mutable and const views into the pixel storage. One uint32 per
  /// pixel: `0xAARRGGBB` on little-endian hosts.
  [[nodiscard]] drm::span<const std::uint32_t> pixels() const noexcept {
    return {pixels_.data(), pixels_.size()};
  }
  [[nodiscard]] drm::span<std::uint32_t> pixels() noexcept {
    return {pixels_.data(), pixels_.size()};
  }

 private:
  std::uint32_t w_{0};
  std::uint32_t h_{0};
  std::vector<std::uint32_t> pixels_;
};

/// Read back the current plane composition of the given CRTC.
/// Unsupported planes (tiled, YUV, unreadable) are logged via
/// drm::log::warn() and omitted; the returned Image covers the CRTC's
/// active mode.
///
/// Requires the caller to have enabled the atomic client cap on
/// `device` (e.g. via `Device::enable_atomic()`). Plane placement is
/// read from the atomic-only CRTC_X/CRTC_Y/CRTC_W/CRTC_H properties,
/// which the kernel hides from non-atomic clients — without the cap,
/// every plane is silently invisible to this function and the call
/// returns an error.
///
/// Returns an error when:
///   - the CRTC id is not active on `device`;
///   - every scanout plane on the CRTC is unsupported (nothing to
///     composite);
///   - Blend2D allocation fails.
[[nodiscard]] drm::expected<Image, std::error_code> snapshot(const drm::Device& device,
                                                             std::uint32_t crtc_id);

}  // namespace drm::capture