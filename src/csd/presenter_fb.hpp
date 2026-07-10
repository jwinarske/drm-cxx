// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// csd/presenter_fb.hpp — legacy /dev/fb0 blit presenter.
//
// Where the plane and composite presenters write KMS atomic state, the
// framebuffer presenter has no KMS at all: it opens a Linux fbdev node
// (/dev/fb0), mmaps it, and blits the painted decoration Surfaces
// straight into that memory. It's the fallback for targets with a
// legacy framebuffer and no (or unusable) KMS plane budget — simplefb,
// a bootloader/VESA fb, or the DRM-emulated fbcon when no plane is free.
//
// The blend itself reuses scene::CompositeCanvas's static helpers
// (blend_into / convert_row), so the SRC_OVER math and the
// ARGB8888 -> {XRGB,XBGR,RGB565,...} row conversion are the same
// battle-tested code the composite presenter uses; only the final
// destination differs (an fbdev mmap instead of a KMS dumb buffer).
//
// Interface note: Presenter::apply() takes an AtomicRequest, but fbdev
// has no atomic commit — the "commit" is the blit into the mmap. This
// presenter therefore ignores `req` entirely and performs the blit as a
// side effect of apply(); callers detect `tier() == Tier::Fb` and skip
// the KMS commit + page-flip wait they do for the other tiers.
//
// v1 is single-buffered (a full-frame blit each apply()); FBIOPAN_DISPLAY
// double-buffering and blit-side damage are follow-ups.

#pragma once

#include "presenter.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <vector>

namespace drm {
class AtomicRequest;
}  // namespace drm

namespace drm::csd {

/// Map an fbdev pixel layout to the DRM FourCC that `CompositeCanvas::
/// convert_row` emits for it. `bpp` is `fb_var_screeninfo::bits_per_pixel`;
/// `red_offset` / `blue_offset` are the channel bit offsets; `transp_length`
/// is the alpha channel's bit width (0 = opaque). Returns nullopt when the
/// layout isn't one convert_row can produce (the fb can't host the canvas).
///
/// Handles the common cases: 32bpp with red above blue -> XRGB/ARGB8888,
/// red below blue -> XBGR/ABGR8888; 16bpp with red above blue -> RGB565,
/// else BGR565. Pure — exposed for unit tests.
[[nodiscard]] std::optional<std::uint32_t> fb_fourcc_for(std::uint32_t bpp,
                                                         std::uint32_t red_offset,
                                                         std::uint32_t blue_offset,
                                                         std::uint32_t transp_length);

/// One decoration to blit: CPU-mapped ARGB/XRGB pixels plus where it lands
/// in the framebuffer. Mirrors the fields `blend_into` needs, decoupled
/// from `csd::Surface` so the compose loop is unit-testable.
struct FbBlitItem {
  drm::span<const std::uint8_t> pixels;
  std::uint32_t stride{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::int32_t x{0};
  std::int32_t y{0};
  std::uint32_t fourcc{0};  // source DRM FourCC (ARGB8888 / XRGB8888)
};

/// Clear `shadow` (an ARGB8888 scratch buffer sized `fb_w * fb_h * 4`),
/// SRC_OVER-blend each item into it bottom-to-top, then convert the shadow
/// row-by-row into `fb` (`fb_stride` bytes/row, `fb_fourcc` layout). Pure:
/// no fbdev / DRM handles, so unit tests drive it with stack buffers.
void compose_into_framebuffer(drm::span<std::uint8_t> fb, std::uint32_t fb_stride,
                              std::uint32_t fb_w, std::uint32_t fb_h, std::uint32_t fb_fourcc,
                              drm::span<std::uint8_t> shadow,
                              drm::span<const FbBlitItem> items) noexcept;

class FramebufferPresenter : public Presenter {
 public:
  /// Open + mmap `fb_path` (default "/dev/fb0"), reading its geometry and
  /// pixel layout from the fbdev ioctls. No DRM device is needed — the
  /// presenter owns only the fbdev mapping. The Surfaces handed to apply()
  /// are still allocated elsewhere (the caller's DRM device on scenario A).
  ///
  /// Errors:
  ///   * `errc::no_such_file_or_directory` / open errno — `fb_path` absent.
  ///   * `errc::io_error` — an FBIOGET_*SCREENINFO ioctl or mmap failed.
  ///   * `errc::not_supported` — the fb's pixel layout isn't one the canvas
  ///     can convert to (see `fb_fourcc_for`).
  [[nodiscard]] static drm::expected<std::unique_ptr<FramebufferPresenter>, std::error_code> create(
      const char* fb_path = "/dev/fb0");

  FramebufferPresenter(const FramebufferPresenter&) = delete;
  FramebufferPresenter& operator=(const FramebufferPresenter&) = delete;
  FramebufferPresenter(FramebufferPresenter&&) = delete;
  FramebufferPresenter& operator=(FramebufferPresenter&&) = delete;
  ~FramebufferPresenter() override;

  [[nodiscard]] Tier tier() const noexcept override { return Tier::Fb; }

  /// Blit `surfaces` into the framebuffer (bottom-to-top; vacant slots
  /// contribute nothing). `req` is ignored — fbdev has no atomic commit,
  /// so the blit here IS the commit. A per-surface CPU-map failure is
  /// surfaced as an error; the caller should stop the frame.
  drm::expected<void, std::error_code> apply(drm::span<const SurfaceRef> surfaces,
                                             drm::AtomicRequest& req) override;

  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] std::uint32_t fourcc() const noexcept { return fourcc_; }

 private:
  FramebufferPresenter() = default;

  int fd_{-1};
  std::uint8_t* fb_{nullptr};
  std::size_t map_len_{0};
  std::uint32_t width_{0};
  std::uint32_t height_{0};
  std::uint32_t stride_{0};  // bytes per row (fb_fix_screeninfo::line_length)
  std::uint32_t fourcc_{0};
  std::vector<std::uint8_t> shadow_;  // ARGB8888 scratch, width_*height_*4
};

}  // namespace drm::csd
