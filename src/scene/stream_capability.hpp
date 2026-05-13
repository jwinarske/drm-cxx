// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// stream_capability.hpp — runtime EGL Streams capability probe.
// See docs/streams.md for the full design.
//
// What this is and isn't:
//
//   * IS: an EGL-header-free public API that reports whether the
//     running system has libEGL with the extension set drm::scene's
//     EglStreamSource needs. The probe does the dlopen itself;
//     consumers never link or load libEGL directly.
//
//   * IS: the input to LayerScene::Config that lets the scene decide
//     up-front whether a layer carrying `BindingModel::DriverOwnsBinding`
//     can coexist with FB-ID layers on this CRTC.
//
//   * ISN'T: an EGL wrapper, a stream lifecycle helper, or a producer.
//     The EglStreamSource and its EGL context builder live in
//     scene/egl_stream_source.{hpp,cpp}; that's the construction side.
//
// Build-time gate: when -Dstreams=disabled or EGL headers are absent at
// build, this translation unit still compiles — probe_stream_capability
// short-circuits to `StreamMixingMode::Unsupported` without trying to
// dlopen libEGL. The result type itself is always defined so callers
// don't conditionally compile.

#pragma once

#include <cstdint>
#include <string>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

/// How stream-consumed planes interact with FB-ID-attached planes on
/// the same CRTC. Set by `probe_stream_capability` and consumed by
/// LayerScene at commit time to gate which mixes of layer-binding
/// models the allocator will permit.
enum class StreamMixingMode : std::uint8_t {
  /// EGL Streams are not available on this system. Either libEGL is
  /// not installed, the platform device extensions are missing, or
  /// drm-cxx was built with -Dstreams=disabled. `EglStreamSource`
  /// construction will fail; `LayerScene::add_layer` rejects sources
  /// reporting `BindingModel::DriverOwnsBinding`.
  Unsupported,

  /// EGL Streams are available, but the driver does not (or has not
  /// been confirmed to) support mixing stream-consumed planes with
  /// FB-ID-attached planes on the same CRTC. The scene operates in
  /// single-stream-layer mode: at most one `DriverOwnsBinding` layer
  /// per CRTC, any FB-ID layers present must composite together with
  /// the stream layer's bounding rect under composition fallback.
  ///
  /// This is the conservative default the static probe returns when
  /// it cannot rule out the mixing restriction empirically.
  Exclusive,

  /// Stream-consumed and FB-ID-attached planes can coexist on the
  /// same CRTC. The allocator treats stream layers as a constraint
  /// dimension on plane selection but otherwise composes them
  /// alongside FB-ID layers normally. Reported only when an explicit
  /// runtime check (a test commit pairing a stream consumer with an
  /// FB-ID plane) has succeeded.
  Mixed,
};

/// Result of `probe_stream_capability`. All EGL string fields are
/// populated only when the dlopen+eglGetDisplay+eglInitialize chain
/// succeeded for the device being probed; in `Unsupported` mode they
/// remain empty.
///
/// The boolean flags report extension presence on the *device-bound*
/// EGL display, not the platform-default display — proprietary drivers
/// expose the stream extension set per-device, so the per-device probe
/// is the only meaningful answer.
struct StreamCapability {
  StreamMixingMode mixing{StreamMixingMode::Unsupported};

  /// libEGL.so.1 was loadable and exposed the platform-device extension
  /// chain needed to even attempt a per-DRM-node display.
  bool has_egl_runtime{false};

  /// `EGL_EXT_platform_device` — lets the probe call
  /// eglGetPlatformDisplay with `EGL_PLATFORM_DEVICE_EXT`, which is the
  /// only way to bind to a specific DRM node from a headless DRM-master
  /// process.
  bool has_platform_device{false};

  /// `EGL_EXT_device_drm` — advertised by the *chosen* EGL device's
  /// per-device extension string (not by client-side extensions). The
  /// extension itself enables `eglQueryDeviceStringEXT(EGL_DRM_DEVICE_FILE_EXT)`,
  /// which the probe uses to match an EGL device to the caller's
  /// drm::Device. Recorded here for diagnostics; a probe that found a
  /// DRM-rdev match implicitly relied on this extension being present.
  bool has_device_drm{false};

  /// `EGL_EXT_output_drm` — the EGL display surfaces DRM CRTCs and
  /// planes as `EGLOutputLayer`s the stream consumer can bind to.
  /// Strictly required for streams-on-KMS; absence means streams
  /// exist but cannot drive a plane.
  bool has_output_drm{false};

  /// `EGL_KHR_stream` — the base stream object lifecycle (eglCreateStreamKHR,
  /// eglDestroyStreamKHR, state machine).
  bool has_khr_stream{false};

  /// `EGL_EXT_stream_consumer_egloutput` — connects a stream to an
  /// `EGLOutputLayer` (which `EGL_EXT_output_drm` provides) so produced
  /// frames scan out through the kernel plane. This is the consumer
  /// type EglStreamSource binds.
  bool has_stream_consumer_egloutput{false};

  /// `EGL_NV_stream_consumer_eglimage` — alternate consumer type that
  /// surfaces frames as EGLImages instead of scanning them out
  /// directly. Reported for diagnostics; not used by EglStreamSource
  /// (we want direct scanout, not intermediate import).
  bool has_nv_stream_consumer_eglimage{false};

  /// `EGL_KHR_stream_producer_eglsurface` — lets a GL or VG context
  /// render into a stream via its EGLSurface. The producer side; not
  /// consumed by the scene, but the source's builder needs it to wire
  /// the application's EGL context into the stream.
  bool has_stream_producer_eglsurface{false};

  /// eglQueryString(EGL_VENDOR) for the per-device display, e.g.
  /// "NVIDIA". Empty in Unsupported mode.
  std::string vendor;

  /// eglQueryString(EGL_VERSION) for the per-device display, e.g.
  /// "1.5". Empty in Unsupported mode.
  std::string version;

  /// eglQueryString(EGL_CLIENT_APIS) for the per-device display, e.g.
  /// "OpenGL OpenGL_ES". Useful for diagnostics; the producer-side
  /// builder may need it to choose between GL and GLES contexts.
  std::string client_apis;

  /// True iff streams can be used in any mode (Exclusive or Mixed).
  [[nodiscard]] bool usable() const noexcept { return mixing != StreamMixingMode::Unsupported; }
};

/// Probe EGL at runtime for stream support on the DRM node `dev`
/// wraps. The probe never throws and never aborts:
///
///   * On systems where libEGL is absent or where the EGL stack is
///     Mesa-only (no EGL Streams extension set), returns a
///     StreamCapability with `mixing == Unsupported` and all flags
///     false. This is the expected result on every Mesa-driven
///     system (amdgpu, i915, lima, etc.) and is not an error.
///
///   * On NVIDIA proprietary / Tegra systems where the extension set
///     is present and `dev` matches an enumerable EGL device, returns
///     `mixing == Exclusive` (the conservative default) with the full
///     extension set populated. Callers who need `Mixed` semantics
///     must run an empirical mixing test (a TEST commit with both a
///     stream consumer and a fully-armed FB-ID plane on the same
///     CRTC) and override the field on the returned struct before
///     handing it to `LayerScene::Config`. `LayerScene::probe_stream_mixing()`
///     drives that test once an `EglStreamSource` has bound a consumer
///     plane; on success it upgrades the cached mixing verdict.
///
///   * On a build with `-Dstreams=disabled` or with EGL headers
///     unavailable at build time, this is a no-op that returns
///     Unsupported.
///
/// The probe loads libEGL via `dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL)`
/// and never unloads it for the lifetime of the process — the EGL
/// runtime is not designed to survive a `dlclose` after the platform
/// device extension chain has been exercised, and proprietary
/// implementations leak thread-local state.
[[nodiscard]] StreamCapability probe_stream_capability(const drm::Device& dev) noexcept;

/// Shorthand for "force-Unsupported" without running a probe. Useful
/// when constructing a `LayerScene::Config` in tests, in code paths
/// known not to need streams, or to opt out of streams in production
/// even on systems where the probe would have succeeded.
[[nodiscard]] StreamCapability stream_capability_unsupported() noexcept;

/// Human-readable name for diagnostics / logging.
[[nodiscard]] const char* to_string(StreamMixingMode mode) noexcept;

}  // namespace drm::scene
