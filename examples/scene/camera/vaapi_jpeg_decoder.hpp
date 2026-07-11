// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// vaapi_jpeg_decoder.hpp — hardware MJPEG decode for the camera example.
//
// Wraps a VADisplay opened on the same DRM render node the scene scans
// out from, configures `VAProfileJPEGBaseline`, allocates VA surfaces
// matching the JPEG's chroma sub-sampling, and exports a scanout-side
// NV12 surface as a dma-buf the rest of the pipeline can hand to
// `drm::scene::ExternalDmaBufSource` for zero-copy scanout.
//
// Two configurations:
//
//   * Yuv420 — one NV12 surface that is both the decode target and the
//     scanout source. The hardware writes 4:2:0 chroma directly into
//     the format the OVERLAY plane scans out.
//   * Yuv422 — a YUYV (4:2:2) intermediate decode surface plus a
//     separate NV12 output surface; per frame the JPEG is hw-decoded
//     into YUYV, then a VAAPI Video Processing pass converts YUYV to
//     NV12 in the output surface. The NV12 output is what gets
//     exported / scanned out. No CPU pixel touch in either step.
//
// The exported dma-buf is allocated once at construction; subsequent
// decodes write the same physical pages, so the `ExternalDmaBufSource`
// built once at startup keeps pointing at fresh pixels on every frame.
//
// Single-buffered: a scanout pass that races a decode into the same
// surface will tear. UVC at 30 Hz doesn't hit this in practice; the
// camera example's existing single-buffer `DumbBufferSource` path has
// the same property and the `cam_example_plan.md` deferred list calls
// out double-buffering separately.
//
// Build is gated on `libva` + `libva-drm` (the meson / cmake camera
// probe wires `CAMERA_HAS_VAAPI=1` when both resolve); the header is
// safe to include unguarded but the symbols it declares are only
// defined when the gate is on.

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {
class ExternalDmaBufSource;
}  // namespace drm::scene

namespace drm::examples::camera {

/// Hardware MJPEG → NV12 decoder backed by VA-API. Holds one or two
/// VASurfaces (depending on sampling) and one exported dma-buf for the
/// NV12 scanout output for its lifetime.
class VaapiJpegDecoder {
 public:
  /// Chroma sub-sampling of the JPEG stream we expect to decode.
  /// Unknown is used as a "detected" value after a parse, never as a
  /// requested mode.
  enum class Sampling : std::uint8_t { Yuv420, Yuv422, Unknown };

  /// Open a VADisplay on `drm_fd` and call `vaInitialize`. The returned
  /// pointer (a `VADisplay`, opaque to non-VAAPI callers) must be
  /// closed with `close_display()` at teardown. libva does not support
  /// repeated `vaInitialize` cycles on the same DRM fd within a single
  /// process, so the caller is expected to hold one display per slot
  /// for its full lifetime and hand it to every `create()` call —
  /// surface/context rebuilds reuse the existing display.
  [[nodiscard]] static void* open_display(int drm_fd, std::error_code* ec = nullptr) noexcept;

  /// Counterpart to `open_display`. Safe on nullptr.
  static void close_display(void* va_display) noexcept;

  /// Build a decoder bound to `va_display`. The display is borrowed;
  /// the decoder destroys only the surfaces / contexts / configs it
  /// allocated, never the display. Failures fall through to `nullptr`
  /// with `ec` populated; callers fall back to the CPU
  /// `mjpeg_to_xrgb` path on miss.
  ///
  /// The dma-buf fd the decoder owns is closed when the decoder is
  /// destroyed, after every consumer (the `ExternalDmaBufSource`
  /// returned by `make_source`) has been torn down — the source
  /// duplicates the fd on construction so the lifetimes are
  /// independent.
  [[nodiscard]] static std::unique_ptr<VaapiJpegDecoder> create(void* va_display,
                                                                std::uint32_t width,
                                                                std::uint32_t height,
                                                                Sampling sampling,
                                                                std::error_code* ec = nullptr);

  VaapiJpegDecoder(const VaapiJpegDecoder&) = delete;
  VaapiJpegDecoder& operator=(const VaapiJpegDecoder&) = delete;
  VaapiJpegDecoder(VaapiJpegDecoder&&) = delete;
  VaapiJpegDecoder& operator=(VaapiJpegDecoder&&) = delete;
  ~VaapiJpegDecoder();

  /// Parse the JPEG header, drive the decode (and, for Yuv422, the
  /// VP conversion), and block until the scanout surface is ready.
  /// Returns `false` on parse failure, decoder failure, or when the
  /// JPEG's chroma sampling doesn't match the configured mode; in the
  /// last case `detected_sampling()` reports what the JPEG actually
  /// uses so the caller can rebuild the decoder with the matching
  /// sampling. Logs to stderr on hard VA-API errors; callers should
  /// treat a `false` return as a single-frame skip.
  [[nodiscard]] bool decode_into_surface(const std::uint8_t* jpeg, std::size_t jpeg_size) noexcept;

  /// After `decode_into_surface()` returns `false`, reports the chroma
  /// sampling parsed from the last JPEG header. Unknown if the parser
  /// itself failed (truncated / non-baseline / dim mismatch). Stable
  /// across the lifetime of the decoder once set — only updated by
  /// `decode_into_surface()`.
  [[nodiscard]] Sampling detected_sampling() const noexcept { return detected_sampling_; }

  /// What was passed to `create()`.
  [[nodiscard]] Sampling configured_sampling() const noexcept { return configured_sampling_; }

  /// Build an `ExternalDmaBufSource` over the decoder's NV12 output
  /// surface. Call once at slot setup; the returned source's cached
  /// fb_id stays valid for every subsequent `decode_into_surface()`
  /// call.
  ///
  /// `dev` is the DRM device the scene scans out on; it can differ
  /// from the render node `drm_fd` the decoder opened (e.g.
  /// `/dev/dri/card0` vs `/dev/dri/renderD128`) — the dma-buf is
  /// re-imported via `drmPrimeFDToHandle` on the display fd.
  [[nodiscard]] drm::expected<std::unique_ptr<drm::scene::ExternalDmaBufSource>, std::error_code>
  make_source(const drm::Device& dev) const;

  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

  /// The NV12 output VASurfaceID (as an opaque unsigned int), valid after a
  /// successful decode_into_surface(). Lets an in-process VA-API consumer on
  /// the same VADisplay — e.g. VaapiH264Encoder — read the decoded frame with
  /// no export/import. `VA_INVALID_ID` (0xffffffff) before the first decode.
  [[nodiscard]] unsigned int output_surface() const noexcept { return va_output_surface_; }

 private:
  VaapiJpegDecoder() = default;

  /// Tear down every VA-API resource we hold. Idempotent. Run from
  /// both the failure paths in `create()` and the destructor.
  void destroy_state() noexcept;

  // Opaque holders — the implementation uses `VADisplay`, `VAConfigID`,
  // `VAContextID`, `VASurfaceID` (all `uintptr_t` / `unsigned int`
  // aliases inside `<va/va.h>`). Stored as raw void*/uints so this
  // header doesn't drag <va/va.h> into every consumer.
  void* va_display_ = nullptr;                 // VADisplay
  unsigned int va_jpeg_config_ = 0xffffffffU;  // VAConfigID; VA_INVALID_ID sentinel
  unsigned int va_jpeg_context_ = 0xffffffffU;
  unsigned int va_decode_surface_ = 0xffffffffU;  // YUYV (Yuv422) or NV12 (Yuv420)
  unsigned int va_output_surface_ = 0xffffffffU;  // NV12 scanout target (== decode for Yuv420)
  unsigned int va_vp_config_ = 0xffffffffU;       // only used for Yuv422
  unsigned int va_vp_context_ = 0xffffffffU;      // only used for Yuv422

  int exported_fd_ = -1;
  std::uint32_t exported_offset_y_ = 0;
  std::uint32_t exported_offset_uv_ = 0;
  std::uint32_t exported_pitch_y_ = 0;
  std::uint32_t exported_pitch_uv_ = 0;
  std::uint64_t exported_modifier_ = 0;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  Sampling configured_sampling_ = Sampling::Yuv420;
  Sampling detected_sampling_ = Sampling::Unknown;
};

}  // namespace drm::examples::camera
