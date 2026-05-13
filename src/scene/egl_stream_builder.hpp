// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// egl_stream_builder.hpp — public entry point for constructing
// `LayerBufferSource`s backed by an EGL stream whose consumer is
// wired to a DRM plane.
//
// One-stop ergonomic for the "I have a drm::Device, give me a
// streams source for this format" workflow. The builder:
//
//   * Re-finds the EGL device that wraps the same DRM node as the
//     caller's drm::Device, mirroring what `probe_stream_capability`
//     did at capability time.
//   * Creates (or reuses) a device-bound EGLDisplay via
//     `eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, ...)`.
//   * Picks an `EGLConfig` matching the producer-surface requirements
//     (`EGL_STREAM_BIT_KHR`, RGBA8888 by default).
//   * Creates a GLES 3.x context if the caller didn't supply one.
//   * Constructs the underlying stream source (private
//     `EglStreamSource` type — never exposed in the public API).
//
// The result hands back the `LayerBufferSource` upcast for handing
// to `LayerScene::add_layer`, plus all the EGL handles the producer
// side needs (display, config, context, producer surface, stream).
//
// Lifetime contract — the caller owns:
//
//   * The returned `source` (unique_ptr).
//   * The `display`. Call `eglTerminate(display)` after all sources
//     and contexts using it have been destroyed.
//   * The `context`. Always destroy via `eglDestroyContext` before
//     terminating the display.
//
// Producer-side note: `producer_surface` and `stream` are snapshots
// at build time. If the scene's allocator later moves the source's
// layer to a different plane, `EglStreamSource::bind_to_plane`
// destroys + recreates the stream + surface — re-fetch them via the
// source's accessors after a rebind event (rare in practice).
//
// Build gate: the entire class is `#if DRM_CXX_HAS_EGL_STREAMS`.
// Builds without EGL headers (`-DDRM_CXX_STREAMS=OFF`, or no egl.h on
// the host) don't define the class, and consumer code that references
// `EglStreamBuilder` won't compile against that drm-cxx build. The
// public `StreamCapability` API still provides the gating signal
// callers branch on.

#pragma once

#include "buffer_source.hpp"
#include "stream_capability.hpp"

#include <drm-cxx/detail/expected.hpp>

#if DRM_CXX_HAS_EGL_STREAMS
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

#include <memory>
#include <system_error>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

#if DRM_CXX_HAS_EGL_STREAMS

class EglStreamSource;

/// Build an EGL-streams-backed `LayerBufferSource` end-to-end —
/// device matching, display + config + context setup, stream + producer
/// surface wiring. Stateless: every call is independent.
class EglStreamBuilder {
 public:
  /// Inputs.
  struct Request {
    /// Probed capability for the target device. `usable()` must be
    /// true; the builder rejects `Unsupported` with
    /// `errc::function_not_supported`.
    StreamCapability capability;

    /// DRM device the stream's consumer plane will live on. Required
    /// when `existing_display == EGL_NO_DISPLAY`; the builder
    /// re-enumerates EGL devices and matches by `st_rdev`. Ignored
    /// when an existing display is supplied (caller already picked
    /// the device).
    const drm::Device* device{nullptr};

    /// Producer surface dimensions and format. The source's
    /// `format()` returns this verbatim. Width and height must be
    /// non-zero; modifier is ignored (the stream consumer doesn't
    /// surface a modifier).
    SourceFormat format{};

    /// Optional: existing EGLDisplay to reuse. `EGL_NO_DISPLAY` (the
    /// default) means "create a fresh device-bound display." When
    /// reusing, the caller must have already run `eglInitialize` on
    /// the display.
    EGLDisplay existing_display{EGL_NO_DISPLAY};

    /// Optional: existing EGLContext to reuse. `EGL_NO_CONTEXT` (the
    /// default) means "create a GLES 3.x context on the chosen
    /// config." When reusing, the caller is responsible for ensuring
    /// the context's underlying config is stream-compatible.
    EGLContext existing_context{EGL_NO_CONTEXT};
  };

  /// Outputs. Every field is populated on success.
  struct Result {
    /// Upcast source — hand directly to `LayerScene::add_layer`.
    std::unique_ptr<LayerBufferSource> source;

    /// Device-bound EGLDisplay. Same handle as
    /// `Request::existing_display` if one was supplied. Caller is
    /// responsible for `eglTerminate(display)` after destroying all
    /// sources, surfaces, and contexts attached to it.
    EGLDisplay display{EGL_NO_DISPLAY};

    /// EGLConfig the builder selected for the producer surface (and,
    /// when applicable, the context).
    EGLConfig egl_config{nullptr};

    /// GL/GLES context. The caller always owns destruction via
    /// `eglDestroyContext`; `context_created_by_builder` tells you
    /// whether the builder allocated it (true) or you passed it in
    /// (false). Builder-created contexts are GLES 3.x with no
    /// surfaceless requirement.
    EGLContext context{EGL_NO_CONTEXT};

    /// True iff the builder created the context. Informational —
    /// destruction responsibility is the caller's regardless.
    bool context_created_by_builder{false};

    /// Non-owning pointer to the underlying `EglStreamSource`. Stays
    /// valid as long as the `LayerScene` that owns the (moved-into)
    /// `source` keeps the layer alive. Use this to query the
    /// producer surface after the first `LayerScene::commit()`:
    /// NVIDIA's driver only lets the producer surface come into
    /// existence after a consumer is attached, and the consumer
    /// attach happens inside the scene's bind_to_plane pre-pass on
    /// first commit.
    ///
    /// Typical flow:
    ///
    ///   scene.add_layer({.source = std::move(bld.source), ...});
    ///   scene.commit();   // triggers source->bind_to_plane()
    ///   auto surf = bld.source_ptr->producer_surface();
    ///   eglMakeCurrent(bld.display, surf, surf, bld.context);
    ///
    /// Set to nullptr only when build() fails before constructing
    /// the source; success paths always populate it.
    EglStreamSource* source_ptr{nullptr};

    /// Backing EGLStream. Mostly diagnostic; the source manages the
    /// stream's lifecycle. Identity may change across rebinds.
    EGLStreamKHR stream{EGL_NO_STREAM_KHR};
  };

  /// Run the build. Returns:
  ///
  ///   * `errc::function_not_supported` — `Request::capability` is
  ///     `Unsupported`, the build was made without EGL Streams, or
  ///     the runtime is missing required entry points.
  ///   * `errc::invalid_argument` — null device with no existing
  ///     display, zero format dimensions.
  ///   * `errc::no_such_device` — no EGL device matched the caller's
  ///     drm::Device.
  ///   * `errc::io_error` — any EGL call (init, choose config, create
  ///     context, create stream, create producer surface) returned
  ///     failure.
  ///
  /// Side effect: when the builder creates a context, it first calls
  /// `eglBindAPI(EGL_OPENGL_ES_API)`. Callers with their own thread-
  /// local API binding should pass `existing_context` to keep the
  /// builder from rebinding.
  [[nodiscard]] static drm::expected<Result, std::error_code> build(const Request& req);
};

#endif  // DRM_CXX_HAS_EGL_STREAMS

}  // namespace drm::scene
