// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// egl_stream_source.hpp — `LayerBufferSource` backed by an EGL stream
// whose consumer is wired directly to a DRM plane via
// `EGL_EXT_output_drm` / `EGL_EXT_stream_consumer_egloutput`.
//
// Internal header. Not exported under include/drm-cxx/. End-user code
// constructs an `EglStreamSource` indirectly through
// `EglStreamBuilder` (public, see egl_stream_builder.hpp). Keeping
// this header internal contains the EGL type leakage; the public
// builder hands users back an upcast `LayerBufferSource` + the
// producer-side EGL handles they need.
//
// Lifecycle:
//
//   * `create()` initializes an EGLStream and a producer-side
//     EGLSurface attached to it. Until `bind_to_plane()` runs the
//     stream has no consumer; subsequent `acquire()` calls return
//     `errc::resource_unavailable_try_again` (EAGAIN) and the scene
//     skips the layer for that commit.
//
//   * `bind_to_plane(plane_id)` enumerates the EGL output layers for
//     the device-bound display, finds the one whose `EGL_DRM_PLANE_EXT`
//     attribute matches `plane_id`, and connects the stream consumer
//     to it via `eglStreamConsumerOutputEXT`. Once this returns
//     success, frames the user pushes into the producer surface
//     scan out through that plane without the scene writing FB_ID.
//
//   * `unbind_from_plane()` destroys the stream + producer surface
//     and recreates fresh ones. Plane reassignment is supported but
//     invalidates the user's cached `producer_surface()` handle —
//     callers must re-query after a rebind. Most workloads don't
//     reassign once the allocator has converged.
//
//   * `on_session_paused()` tears the stream down (the DRM fd is
//     going away); `on_session_resumed()` rebuilds it lazily. While
//     paused, `acquire()` returns EAGAIN so the scene drops this
//     layer for the duration.
//
// Build gate: the entire class is `#if DRM_CXX_HAS_EGL_STREAMS`.
// Translation units that need to construct an EglStreamSource must
// themselves be gated. The public `StreamCapability` API does not
// change shape across the gate.

#pragma once

#if DRM_CXX_HAS_EGL_STREAMS

#include "buffer_source.hpp"
#include "stream_capability.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

/// `LayerBufferSource` whose backing buffer is an EGL stream's
/// producer surface, and whose consumer is bound directly to a DRM
/// plane. Reports `BindingModel::DriverOwnsBinding` to the scene; the
/// allocator skips FB_ID writes for any layer using this source.
///
/// Construct via `create()` — the source manages stream + producer
/// surface lifetime internally. Plane binding happens through the
/// `LayerBufferSource::bind_to_plane` hook the scene drives after the
/// allocator has settled on a plane assignment.
class EglStreamSource final : public LayerBufferSource {
 public:
  /// Inputs needed to create the source. The caller (typically
  /// `EglStreamBuilder`) is responsible for producing a usable
  /// EGLDisplay + EGLConfig that matches `format`.
  struct Config {
    /// Device-bound EGLDisplay, already initialized via eglInitialize.
    /// The caller retains ownership; the source does not terminate
    /// the display in its destructor.
    EGLDisplay display{EGL_NO_DISPLAY};

    /// EGLConfig chosen against `display` with `EGL_STREAM_BIT_KHR`
    /// renderable type. Caller selects this to match the user's
    /// GL/GLES context.
    EGLConfig egl_config{nullptr};

    /// Producer-side surface dimensions and format. The source's
    /// `format()` returns this verbatim — `modifier` is ignored
    /// because the EGL stream consumer doesn't surface a modifier.
    SourceFormat format{};
  };

  /// Construct an `EglStreamSource`. Returns:
  ///
  ///   * `errc::function_not_supported` when the build was made
  ///     without EGL Streams or the runtime probe couldn't find the
  ///     extension chain on the matched device.
  ///   * `errc::invalid_argument` when `config.display` is
  ///     `EGL_NO_DISPLAY`, `config.egl_config` is null, or
  ///     `config.format` has zero width/height.
  ///   * `errc::io_error` when `eglCreateStreamKHR` or
  ///     `eglCreateStreamProducerSurfaceKHR` fail at the EGL level.
  ///
  /// On success, the returned source owns an EGLStream and a producer
  /// EGLSurface but is not yet bound to any DRM plane; `acquire()`
  /// returns EAGAIN until `bind_to_plane()` has succeeded.
  [[nodiscard]] static drm::expected<std::unique_ptr<EglStreamSource>, std::error_code> create(
      const StreamCapability& cap, Config config);

  EglStreamSource(const EglStreamSource&) = delete;
  EglStreamSource& operator=(const EglStreamSource&) = delete;
  EglStreamSource(EglStreamSource&&) = delete;
  EglStreamSource& operator=(EglStreamSource&&) = delete;
  ~EglStreamSource() override;

  // ── LayerBufferSource ──────────────────────────────────────────────
  [[nodiscard]] drm::expected<AcquiredBuffer, std::error_code> acquire() override;
  void release(AcquiredBuffer acquired) noexcept override;
  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return BindingModel::DriverOwnsBinding;
  }
  [[nodiscard]] SourceFormat format() const noexcept override { return config_.format; }
  drm::expected<void, std::error_code> bind_to_plane(std::uint32_t plane_id) override;
  void unbind_from_plane(std::uint32_t plane_id) noexcept override;
  void on_session_paused() noexcept override;
  drm::expected<void, std::error_code> on_session_resumed(const drm::Device& new_dev) override;

  // ── Producer-side accessors ────────────────────────────────────────
  //
  // The user's GL/GLES context makes `producer_surface()` current,
  // renders frames, and calls eglSwapBuffers to push them into the
  // stream. `producer_surface()` and `stream()` change identity
  // across unbind/rebind cycles and across session pause/resume —
  // callers must re-query after either event.

  [[nodiscard]] EGLSurface producer_surface() const noexcept { return producer_surface_; }
  [[nodiscard]] EGLStreamKHR stream() const noexcept { return stream_; }
  [[nodiscard]] EGLDisplay display() const noexcept { return config_.display; }
  [[nodiscard]] EGLConfig egl_config() const noexcept { return config_.egl_config; }

  /// Currently bound DRM plane id, or `std::nullopt` if the stream
  /// consumer has not been wired yet (or has been torn down by
  /// `on_session_paused`).
  [[nodiscard]] std::optional<std::uint32_t> bound_plane() const noexcept {
    return bound_plane_id_;
  }

 private:
  explicit EglStreamSource(Config cfg) noexcept;

  [[nodiscard]] drm::expected<void, std::error_code> create_stream_and_producer() noexcept;
  void destroy_stream_and_producer() noexcept;

  Config config_;
  EGLStreamKHR stream_{EGL_NO_STREAM_KHR};
  EGLSurface producer_surface_{EGL_NO_SURFACE};
  std::optional<std::uint32_t> bound_plane_id_;
  bool session_paused_{false};
};

}  // namespace drm::scene

#endif  // DRM_CXX_HAS_EGL_STREAMS
