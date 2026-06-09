// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// egl_loader.hpp — process-singleton libEGL.so.1 dlopen wrapper, shared
// across the library's EGL consumers: the EGL Streams probe
// (stream_capability.cpp), the EglStreamSource construction path
// (egl_stream_source.cpp), and the GL scanout producer
// (present/gl_scanout_producer.cpp).
//
// dlsym bootstraps eglGetProcAddress and the EGL 1.0 core ABI symbols
// (portable to any EGL version); eglGetProcAddress resolves the
// extension and platform entry points.
//
// Internal header. Not exported under include/drm-cxx/. Translation
// units that need to drive libEGL include this and access the
// singleton via `egl_loader()`. The first caller pays the dlopen +
// symbol-resolution cost under a std::once_flag; subsequent callers
// read the resolved struct without synchronization.
//
// The runtime intentionally never dlcloses libEGL.so.1: the
// proprietary EGL implementations keep thread-local state across
// initialize/terminate and don't gracefully survive dlclose once
// eglQueryDevicesEXT has been called. The process-lifetime cost is
// one void* and the resolved function-pointer table.

#pragma once

#if DRM_CXX_HAS_EGL

#include <EGL/egl.h>
#include <EGL/eglext.h>

namespace drm::detail {

/// Resolved EGL entry-point table. All pointers null until
/// `egl_loader()` runs its std::call_once initializer. After init:
///
///   * If `loaded == true`, every bootstrap symbol is non-null and
///     callers can use them unconditionally.
///   * Extension entry points are non-null iff `eglGetProcAddress`
///     was willing to return a pointer for that name on this libEGL
///     install. Callers MUST null-check before calling — Mesa-only
///     stacks have all the streams entry points as nullptr; the
///     proprietary EGL stack on a driver-version mismatch may also
///     leave some null.
struct EglLoader {
  void* handle{nullptr};
  bool loaded{false};

  // Bootstrap symbols — resolved via dlsym, always non-null when
  // `loaded == true`. eglGetProcAddress cannot resolve
  // eglGetProcAddress itself, and the extension entry points were
  // added by extension so eglGetProcAddress only resolves them once
  // the client extension chain has been queried (which itself needs
  // eglQueryString). dlsym is the bootstrap.
  PFNEGLGETPROCADDRESSPROC get_proc_address{nullptr};
  decltype(&eglQueryString) query_string{nullptr};
  decltype(&eglGetDisplay) get_display{nullptr};
  decltype(&eglInitialize) initialize{nullptr};
  decltype(&eglTerminate) terminate{nullptr};
  decltype(&eglGetError) get_error{nullptr};
  decltype(&eglCreateContext) create_context{nullptr};
  decltype(&eglDestroyContext) destroy_context{nullptr};
  decltype(&eglMakeCurrent) make_current{nullptr};
  decltype(&eglDestroySurface) destroy_surface{nullptr};
  decltype(&eglChooseConfig) choose_config{nullptr};
  decltype(&eglBindAPI) bind_api{nullptr};
  // EGL 1.5 core entry point — uses EGLAttrib (intptr_t) attribute
  // lists, distinct from the EXT variant (`get_platform_display`,
  // EGLint*). Needed for `EGL_DRM_MASTER_FD_EXT` which is documented
  // for the core call only. Null on libEGL < 1.5.
  decltype(&eglGetPlatformDisplay) get_platform_display_core{nullptr};

  // Rendering-path entry points used by the GL scanout producer (a
  // gbm_surface-backed EGL window surface). Core EGL, resolved via
  // dlsym; create_platform_window_surface is EGL 1.5 core (EGLAttrib*
  // attribute list) and is null on libEGL < 1.5.
  decltype(&eglGetConfigAttrib) get_config_attrib{nullptr};
  decltype(&eglSwapBuffers) swap_buffers{nullptr};
  decltype(&eglCreatePlatformWindowSurface) create_platform_window_surface{nullptr};

  // Device-enumeration extension entry points — non-null iff the
  // corresponding client-side extension is advertised. The probe
  // gates on these.
  PFNEGLQUERYDEVICESEXTPROC query_devices{nullptr};
  PFNEGLQUERYDEVICESTRINGEXTPROC query_device_string{nullptr};
  PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display{nullptr};

  // Stream extension entry points — non-null iff the corresponding
  // per-display extension is advertised on the device-bound display.
  // Resolved eagerly here; per-display absence shows up as the
  // pointer being null because eglGetProcAddress returns null for
  // unsupported names. EglStreamSource gates on these.
  PFNEGLCREATESTREAMKHRPROC create_stream{nullptr};
  PFNEGLDESTROYSTREAMKHRPROC destroy_stream{nullptr};
  PFNEGLSTREAMATTRIBKHRPROC stream_attrib{nullptr};
  PFNEGLQUERYSTREAMKHRPROC query_stream{nullptr};
  PFNEGLGETOUTPUTLAYERSEXTPROC get_output_layers{nullptr};
  PFNEGLQUERYOUTPUTLAYERATTRIBEXTPROC query_output_layer_attrib{nullptr};
  PFNEGLSTREAMCONSUMEROUTPUTEXTPROC stream_consumer_output{nullptr};
  PFNEGLCREATESTREAMPRODUCERSURFACEKHRPROC create_stream_producer_surface{nullptr};
  PFNEGLSTREAMCONSUMERACQUIREKHRPROC stream_consumer_acquire{nullptr};
  PFNEGLSTREAMCONSUMERRELEASEKHRPROC stream_consumer_release{nullptr};
  // EGL_KHR_stream_attrib: takes an EGLAttrib attribute list so
  // callers can pass `EGL_DRM_ATOMIC_REQUEST_NV, drmModeAtomicReq*`
  // for the first-frame consumer acquire on an EGL_EXT_output_drm
  // plane. NVIDIA's driver fills in FB_ID for the stream's first
  // frame and submits the atomic commit itself.
  PFNEGLSTREAMCONSUMERACQUIREATTRIBKHRPROC stream_consumer_acquire_attrib{nullptr};

  // EGL_KHR_fence_sync + EGL_ANDROID_native_fence_sync entry points — the GL
  // scanout producer creates a native-fence sync after a frame's draw and
  // exports it as a sync_file (the acquire fence KMS waits on). Null on stacks
  // without those extensions; the producer then renders synchronously.
  PFNEGLCREATESYNCKHRPROC create_sync{nullptr};
  PFNEGLDESTROYSYNCKHRPROC destroy_sync{nullptr};
  PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_native_fence_fd{nullptr};
};

/// Process-singleton EGL runtime accessor. First call performs the
/// dlopen + symbol resolution under std::once_flag; subsequent calls
/// are zero-cost reads of the resolved struct. If libEGL.so.1 is not
/// loadable, the returned runtime has `loaded == false` and all
/// pointers null.
[[nodiscard]] const EglLoader& egl_loader() noexcept;

/// Returns true iff `name` (NUL-terminated, no leading/trailing
/// space) appears as a space-delimited token in `extensions`. Used
/// to check both client-side and per-device EGL extension strings.
/// Tolerates `extensions == nullptr` and `name == nullptr` (returns
/// false).
[[nodiscard]] bool extension_present(const char* extensions, const char* name) noexcept;

}  // namespace drm::detail

#endif  // DRM_CXX_HAS_EGL
