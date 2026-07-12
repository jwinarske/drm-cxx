// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "egl_loader.hpp"

#if DRM_CXX_HAS_EGL

#include <drm-cxx/log.hpp>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <mutex>

namespace drm::detail {

namespace {

template <typename Fn>
Fn resolve_sym(void* handle, const char* name) noexcept {
  return reinterpret_cast<Fn>(::dlsym(handle, name));
}

template <typename Fn>
Fn resolve_proc(PFNEGLGETPROCADDRESSPROC gpa, const char* name) noexcept {
  return reinterpret_cast<Fn>(gpa(name));
}

void initialize_runtime(EglLoader& rt) noexcept {
  rt.handle = ::dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
  if (rt.handle == nullptr) {
    drm::log_info("egl_loader: libEGL.so.1 not loadable ({}) — EGL features unavailable",
                  ::dlerror() != nullptr ? ::dlerror() : "no error reported");
    return;
  }

  rt.get_proc_address = resolve_sym<PFNEGLGETPROCADDRESSPROC>(rt.handle, "eglGetProcAddress");
  rt.query_string = resolve_sym<decltype(rt.query_string)>(rt.handle, "eglQueryString");
  rt.get_display = resolve_sym<decltype(rt.get_display)>(rt.handle, "eglGetDisplay");
  rt.initialize = resolve_sym<decltype(rt.initialize)>(rt.handle, "eglInitialize");
  rt.terminate = resolve_sym<decltype(rt.terminate)>(rt.handle, "eglTerminate");
  rt.get_error = resolve_sym<decltype(rt.get_error)>(rt.handle, "eglGetError");
  rt.create_context = resolve_sym<decltype(rt.create_context)>(rt.handle, "eglCreateContext");
  rt.destroy_context = resolve_sym<decltype(rt.destroy_context)>(rt.handle, "eglDestroyContext");
  rt.make_current = resolve_sym<decltype(rt.make_current)>(rt.handle, "eglMakeCurrent");
  rt.destroy_surface = resolve_sym<decltype(rt.destroy_surface)>(rt.handle, "eglDestroySurface");
  rt.choose_config = resolve_sym<decltype(rt.choose_config)>(rt.handle, "eglChooseConfig");
  rt.bind_api = resolve_sym<decltype(rt.bind_api)>(rt.handle, "eglBindAPI");
  rt.get_platform_display_core =
      resolve_sym<decltype(rt.get_platform_display_core)>(rt.handle, "eglGetPlatformDisplay");
  rt.get_config_attrib =
      resolve_sym<decltype(rt.get_config_attrib)>(rt.handle, "eglGetConfigAttrib");
  rt.swap_buffers = resolve_sym<decltype(rt.swap_buffers)>(rt.handle, "eglSwapBuffers");

  if ((rt.get_proc_address == nullptr) || (rt.query_string == nullptr) ||
      (rt.initialize == nullptr) || (rt.terminate == nullptr) || (rt.get_error == nullptr)) {
    drm::log_warn(
        "egl_loader: libEGL.so.1 loaded but missing core symbols — treating as "
        "unsupported");
    rt.handle = nullptr;
    return;
  }

  // Device-enumeration extension entry points. Null on Mesa stacks
  // without libglvnd, present on every modern libEGL.
  rt.query_devices =
      resolve_proc<PFNEGLQUERYDEVICESEXTPROC>(rt.get_proc_address, "eglQueryDevicesEXT");
  rt.query_device_string =
      resolve_proc<PFNEGLQUERYDEVICESTRINGEXTPROC>(rt.get_proc_address, "eglQueryDeviceStringEXT");
  rt.get_platform_display = resolve_proc<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
      rt.get_proc_address, "eglGetPlatformDisplayEXT");

  // Platform window-surface entry point for the GL scanout producer.
  // Resolved via eglGetProcAddress (the platform/glvnd-correct path);
  // null on libEGL < 1.5, which the producer treats as unsupported.
  rt.create_platform_window_surface = resolve_proc<decltype(rt.create_platform_window_surface)>(
      rt.get_proc_address, "eglCreatePlatformWindowSurface");

  // Streams extension entry points. Null on Mesa-only stacks; non-null
  // on the proprietary EGL implementations that advertise the
  // EGL_KHR_stream / EGL_EXT_output_drm / EGL_EXT_stream_consumer_egloutput
  // chain on the device-bound display. Per-display absence shows up
  // as the pointer being null because eglGetProcAddress returns null
  // for unsupported names.
  rt.create_stream =
      resolve_proc<PFNEGLCREATESTREAMKHRPROC>(rt.get_proc_address, "eglCreateStreamKHR");
  rt.destroy_stream =
      resolve_proc<PFNEGLDESTROYSTREAMKHRPROC>(rt.get_proc_address, "eglDestroyStreamKHR");
  rt.stream_attrib =
      resolve_proc<PFNEGLSTREAMATTRIBKHRPROC>(rt.get_proc_address, "eglStreamAttribKHR");
  rt.query_stream =
      resolve_proc<PFNEGLQUERYSTREAMKHRPROC>(rt.get_proc_address, "eglQueryStreamKHR");
  rt.get_output_layers =
      resolve_proc<PFNEGLGETOUTPUTLAYERSEXTPROC>(rt.get_proc_address, "eglGetOutputLayersEXT");
  rt.query_output_layer_attrib = resolve_proc<PFNEGLQUERYOUTPUTLAYERATTRIBEXTPROC>(
      rt.get_proc_address, "eglQueryOutputLayerAttribEXT");
  rt.stream_consumer_output = resolve_proc<PFNEGLSTREAMCONSUMEROUTPUTEXTPROC>(
      rt.get_proc_address, "eglStreamConsumerOutputEXT");
  rt.create_stream_producer_surface = resolve_proc<PFNEGLCREATESTREAMPRODUCERSURFACEKHRPROC>(
      rt.get_proc_address, "eglCreateStreamProducerSurfaceKHR");
  rt.stream_consumer_acquire = resolve_proc<PFNEGLSTREAMCONSUMERACQUIREKHRPROC>(
      rt.get_proc_address, "eglStreamConsumerAcquireKHR");
  rt.stream_consumer_release = resolve_proc<PFNEGLSTREAMCONSUMERRELEASEKHRPROC>(
      rt.get_proc_address, "eglStreamConsumerReleaseKHR");
  // Try the KHR name first (system-header standard), fall back to
  // NVIDIA's EXT alias — NVIDIA exports both, but older drivers
  // shipped only the EXT name.
  rt.stream_consumer_acquire_attrib = resolve_proc<PFNEGLSTREAMCONSUMERACQUIREATTRIBKHRPROC>(
      rt.get_proc_address, "eglStreamConsumerAcquireAttribKHR");
  if (rt.stream_consumer_acquire_attrib == nullptr) {
    rt.stream_consumer_acquire_attrib = resolve_proc<PFNEGLSTREAMCONSUMERACQUIREATTRIBKHRPROC>(
        rt.get_proc_address, "eglStreamConsumerAcquireAttribEXT");
  }

  // EGL fence-sync entry points for the GL producer's acquire-fence export.
  rt.create_sync = resolve_proc<PFNEGLCREATESYNCKHRPROC>(rt.get_proc_address, "eglCreateSyncKHR");
  rt.destroy_sync =
      resolve_proc<PFNEGLDESTROYSYNCKHRPROC>(rt.get_proc_address, "eglDestroySyncKHR");
  rt.dup_native_fence_fd = resolve_proc<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
      rt.get_proc_address, "eglDupNativeFenceFDANDROID");

  // EGL image + dma-buf import entry points for the GL compositor's EGLImage
  // path. create_image/destroy_image are EGL_KHR_image_base; the query_dma_buf_*
  // pair is the optional EGL_EXT_image_dma_buf_import_modifiers query. Null when
  // unadvertised — the compositor's capability probe gates on them.
  rt.create_image =
      resolve_proc<PFNEGLCREATEIMAGEKHRPROC>(rt.get_proc_address, "eglCreateImageKHR");
  rt.destroy_image =
      resolve_proc<PFNEGLDESTROYIMAGEKHRPROC>(rt.get_proc_address, "eglDestroyImageKHR");
  rt.query_dma_buf_formats = resolve_proc<PFNEGLQUERYDMABUFFORMATSEXTPROC>(
      rt.get_proc_address, "eglQueryDmaBufFormatsEXT");
  rt.query_dma_buf_modifiers = resolve_proc<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(
      rt.get_proc_address, "eglQueryDmaBufModifiersEXT");

  rt.loaded = true;
}

}  // namespace

const EglLoader& egl_loader() noexcept {
  static EglLoader rt;
  static std::once_flag once;
  std::call_once(once, [] { initialize_runtime(rt); });
  return rt;
}

bool extension_present(const char* extensions, const char* name) noexcept {
  if ((extensions == nullptr) || (name == nullptr)) {
    return false;
  }
  const std::size_t name_len = ::strlen(name);
  const char* cursor = extensions;
  while (true) {
    const char* match = ::strstr(cursor, name);
    if (match == nullptr) {
      return false;
    }
    const bool left_ok = (match == extensions) || (match[-1] == ' ');
    const char tail = match[name_len];
    const bool right_ok = (tail == '\0') || (tail == ' ');
    if (left_ok && right_ok) {
      return true;
    }
    cursor = match + name_len;
  }
}

}  // namespace drm::detail

#endif  // DRM_CXX_HAS_EGL
