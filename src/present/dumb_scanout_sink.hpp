// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// dumb_scanout_sink.hpp — present CPU-rendered full-screen frames via dumb buffers.
//
// A thin software-present sink for callers that render a whole frame on the CPU
// (e.g. a software rasterizer that hands back a finished pixel buffer). It
// bundles a single-layer LayerScene fed by a DumbRingSource — reusing the
// hardened atomic-present path rather than re-rolling modeset/page-flip — and
// present() copies a finished CPU frame into the next ring buffer and drives one
// atomic flip. No GL / Vulkan / GBM.
//
// For vsync, pass DRM_MODE_PAGE_FLIP_EVENT as `flags` plus a PageFlip to
// present(), and dispatch that PageFlip to receive flip-complete events.

#pragma once

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/scene/commit_report.hpp>

#include <xf86drmMode.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>

namespace drm {
class Device;
class PageFlip;
}  // namespace drm

namespace drm::scene {
class LayerScene;
}  // namespace drm::scene

namespace drm::present {

class DumbRingSource;

class DumbScanoutSink {
 public:
  struct Config {
    std::uint32_t drm_format{0};  ///< 0 => XRGB8888; any packed format (e.g. RGB565)
    std::size_t buffers{3};       ///< ring depth; 0 => 3 (the buffer-age path wants >=3)
  };

  /// Build a sink over an already-picked output (crtc / connector / mode — e.g.
  /// from drm::examples::open_output). Allocates the dumb ring and the
  /// single-layer scene; the first present() drives the modesetting commit.
  [[nodiscard]] static drm::expected<std::unique_ptr<DumbScanoutSink>, std::error_code> create(
      drm::Device& dev, std::uint32_t crtc_id, std::uint32_t connector_id,
      const drmModeModeInfo& mode, const Config& cfg);

  /// Overload using a default Config (XRGB8888, triple-buffered).
  [[nodiscard]] static drm::expected<std::unique_ptr<DumbScanoutSink>, std::error_code> create(
      drm::Device& dev, std::uint32_t crtc_id, std::uint32_t connector_id,
      const drmModeModeInfo& mode);

  ~DumbScanoutSink();
  DumbScanoutSink(const DumbScanoutSink&) = delete;
  DumbScanoutSink& operator=(const DumbScanoutSink&) = delete;
  DumbScanoutSink(DumbScanoutSink&&) = delete;
  DumbScanoutSink& operator=(DumbScanoutSink&&) = delete;

  /// Copy a tightly-packed CPU frame (`src_stride` bytes per row, in the sink's
  /// drm_format) into the next ring buffer and commit one atomic flip. `flags`
  /// is OR-ed into the commit (e.g. DRM_MODE_PAGE_FLIP_EVENT); pass a `flip` for
  /// the kernel to route the completion event back to. Returns
  /// errc::resource_unavailable_try_again when every ring slot is still busy
  /// (retry after the next flip completes), or errc::invalid_argument if `src`
  /// is shorter than height * src_stride.
  [[nodiscard]] drm::expected<scene::CommitReport, std::error_code> present(
      drm::span<const std::byte> src, std::uint32_t src_stride, std::uint32_t flags = 0,
      drm::PageFlip* flip = nullptr);

  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] std::uint32_t refresh_hz() const noexcept { return refresh_; }

  /// The underlying scene, for advanced commit / teardown wiring.
  [[nodiscard]] scene::LayerScene& scene() noexcept { return *scene_; }

 private:
  DumbScanoutSink(std::unique_ptr<scene::LayerScene> scene, DumbRingSource* ring, std::uint32_t w,
                  std::uint32_t h, std::uint32_t refresh) noexcept;

  std::unique_ptr<scene::LayerScene> scene_;
  DumbRingSource* ring_;  ///< owned by scene_ (the layer's source)
  std::uint32_t width_;
  std::uint32_t height_;
  std::uint32_t refresh_;
};

}  // namespace drm::present
