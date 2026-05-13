// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "stream_capability.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/log.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/stat.h>

#if DRM_CXX_HAS_EGL_STREAMS
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <mutex>
#include <utility>
#include <vector>
#endif

namespace drm::scene {

const char* to_string(StreamMixingMode mode) noexcept {
  switch (mode) {
    case StreamMixingMode::Unsupported:
      return "Unsupported";
    case StreamMixingMode::Exclusive:
      return "Exclusive";
    case StreamMixingMode::Mixed:
      return "Mixed";
  }
  return "Unknown";
}

StreamCapability stream_capability_unsupported() noexcept {
  return StreamCapability{};
}

#if !DRM_CXX_HAS_EGL_STREAMS

// Build-side gate disabled. Probe always reports Unsupported. The
// public header keeps the type complete so callers compile uniformly.
StreamCapability probe_stream_capability(const drm::Device& /*dev*/) noexcept {
  return StreamCapability{};
}

#else  // DRM_CXX_HAS_EGL_STREAMS

namespace {

// Singleton EGL runtime loader. The proprietary EGL implementations
// keep thread-local state across initialize/terminate and don't
// gracefully survive dlclose once eglQueryDevicesEXT has been called.
// We open libEGL.so.1 once per process and leak the handle — the
// probe is short-lived and the state cost is one void*.
//
// Returns nullptr (handle) and `loaded == false` on first call that
// cannot find libEGL.so.1; subsequent calls remember the failure and
// short-circuit without retrying dlopen.
struct EglRuntime {
  void* handle{nullptr};
  bool loaded{false};
  bool tried{false};

  // Required client-side function pointers. eglGetProcAddress can't
  // resolve eglGetProcAddress itself, and the platform-device entry
  // points are only fetched via eglGetProcAddress because they were
  // added by extension. So we dlsym the bootstrap symbols.
  PFNEGLGETPROCADDRESSPROC get_proc_address{nullptr};
  decltype(&eglQueryString) query_string{nullptr};
  decltype(&eglGetDisplay) get_display{nullptr};
  decltype(&eglInitialize) initialize{nullptr};
  decltype(&eglTerminate) terminate{nullptr};
  decltype(&eglGetError) get_error{nullptr};

  // Extension entry points resolved lazily after the client extension
  // string has been queried. Null until the relevant client extension
  // is confirmed present.
  PFNEGLQUERYDEVICESEXTPROC query_devices{nullptr};
  PFNEGLQUERYDEVICESTRINGEXTPROC query_device_string{nullptr};
  PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display{nullptr};
};

EglRuntime& egl_runtime() noexcept {
  static EglRuntime rt;
  static std::once_flag once;
  std::call_once(once, [&] {
    rt.tried = true;
    rt.handle = ::dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (rt.handle == nullptr) {
      drm::log_info(
          "scene::stream_capability: libEGL.so.1 not loadable ({}) — EGL Streams unsupported",
          ::dlerror() != nullptr ? ::dlerror() : "no error reported");
      return;
    }
    rt.get_proc_address =
        reinterpret_cast<PFNEGLGETPROCADDRESSPROC>(::dlsym(rt.handle, "eglGetProcAddress"));
    rt.query_string =
        reinterpret_cast<decltype(rt.query_string)>(::dlsym(rt.handle, "eglQueryString"));
    rt.get_display =
        reinterpret_cast<decltype(rt.get_display)>(::dlsym(rt.handle, "eglGetDisplay"));
    rt.initialize = reinterpret_cast<decltype(rt.initialize)>(::dlsym(rt.handle, "eglInitialize"));
    rt.terminate = reinterpret_cast<decltype(rt.terminate)>(::dlsym(rt.handle, "eglTerminate"));
    rt.get_error = reinterpret_cast<decltype(rt.get_error)>(::dlsym(rt.handle, "eglGetError"));

    if ((rt.get_proc_address == nullptr) || (rt.query_string == nullptr) ||
        (rt.initialize == nullptr) || (rt.terminate == nullptr) || (rt.get_error == nullptr)) {
      drm::log_warn(
          "scene::stream_capability: libEGL.so.1 loaded but missing core symbols — treating as "
          "unsupported");
      rt.handle = nullptr;
      return;
    }
    rt.loaded = true;
  });
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

// Resolve the DRM-node path the EGL device reports against the
// caller-supplied drm::Device by comparing st_rdev. Symlink-resolution
// differences (/dev/dri/by-path/... vs /dev/dri/cardN) can't fool us
// at the rdev level — both resolve to the same character-device major:minor.
//
// Returns true iff the EGL device's reported DRM node file refers to
// the same device node as `dev.fd()`.
bool matches_drm_node(int dev_fd, const char* egl_drm_path) noexcept {
  if ((dev_fd < 0) || (egl_drm_path == nullptr) || (*egl_drm_path == '\0')) {
    return false;
  }
  struct ::stat dev_stat{};
  if (::fstat(dev_fd, &dev_stat) != 0) {
    return false;
  }
  struct ::stat egl_stat{};
  if (::stat(egl_drm_path, &egl_stat) != 0) {
    return false;
  }
  return dev_stat.st_rdev == egl_stat.st_rdev;
}

}  // namespace

StreamCapability probe_stream_capability(const drm::Device& dev) noexcept {
  StreamCapability cap;
  auto& rt = egl_runtime();
  if (!rt.loaded) {
    return cap;
  }
  cap.has_egl_runtime = true;

  // Client-side extensions are queried with EGL_NO_DISPLAY. We need the
  // platform-device entry point and the device enumeration extension at
  // the client level; everything else (including EGL_EXT_device_drm,
  // which is a *per-device* extension, not client-side) is queried
  // after we pick an EGLDeviceEXT.
  const char* client_exts = rt.query_string(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  cap.has_platform_device = extension_present(client_exts, "EGL_EXT_platform_device");
  const bool has_device_base = extension_present(client_exts, "EGL_EXT_device_base") ||
                               extension_present(client_exts, "EGL_EXT_device_enumeration");

  if (!cap.has_platform_device || !has_device_base) {
    // libEGL is present but lacks the per-device platform extensions
    // needed to bind to a specific DRM node. Mesa-only systems without
    // libglvnd land here. Leave mixing == Unsupported.
    return cap;
  }

  rt.query_devices =
      reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(rt.get_proc_address("eglQueryDevicesEXT"));
  rt.query_device_string = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(
      rt.get_proc_address("eglQueryDeviceStringEXT"));
  rt.get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
      rt.get_proc_address("eglGetPlatformDisplayEXT"));
  if ((rt.query_devices == nullptr) || (rt.query_device_string == nullptr) ||
      (rt.get_platform_display == nullptr)) {
    drm::log_warn(
        "scene::stream_capability: client extensions advertised but entry points missing — "
        "treating as unsupported");
    return cap;
  }

  EGLint num_devices = 0;
  if (rt.query_devices(0, nullptr, &num_devices) != EGL_TRUE || (num_devices <= 0)) {
    return cap;
  }
  std::vector<EGLDeviceEXT> devices(static_cast<std::size_t>(num_devices));
  if (rt.query_devices(num_devices, devices.data(), &num_devices) != EGL_TRUE) {
    return cap;
  }
  devices.resize(static_cast<std::size_t>(num_devices));

  // On hybrid systems (e.g. NVIDIA + Mesa zink/kmsro) more than one EGL
  // device reports the same backing DRM node. Iterate every DRM-rdev
  // match and prefer the one whose per-display extension set includes
  // the streams consumer chain. Fall back to the first match if none
  // advertise streams — that way the returned StreamCapability still
  // populates vendor/version diagnostics in the Mesa-wins-enumeration case.
  EGLDeviceEXT chosen = EGL_NO_DEVICE_EXT;
  StreamCapability chosen_caps;
  bool chosen_has_streams = false;

  for (auto egl_dev : devices) {
    const char* drm_path = rt.query_device_string(egl_dev, EGL_DRM_DEVICE_FILE_EXT);
    if (!matches_drm_node(dev.fd(), drm_path)) {
      continue;
    }

    StreamCapability local_caps;
    // EGL_EXT_device_drm lives in the per-device extension string, not the
    // client-side one. Without it, eglQueryDeviceStringEXT(EGL_DRM_DEVICE_FILE_EXT)
    // would not have returned a path — but we record it for diagnostics.
    const char* dev_exts = rt.query_device_string(egl_dev, EGL_EXTENSIONS);
    local_caps.has_device_drm = extension_present(dev_exts, "EGL_EXT_device_drm");

    EGLDisplay display =
        rt.get_platform_display(EGL_PLATFORM_DEVICE_EXT, egl_dev, /*attrib_list=*/nullptr);
    if (display == EGL_NO_DISPLAY) {
      drm::log_warn("scene::stream_capability: eglGetPlatformDisplayEXT failed for matched device");
      continue;
    }
    EGLint major = 0;
    EGLint minor = 0;
    if (rt.initialize(display, &major, &minor) != EGL_TRUE) {
      drm::log_warn("scene::stream_capability: eglInitialize failed for matched device");
      continue;
    }

    const char* dpy_exts = rt.query_string(display, EGL_EXTENSIONS);
    local_caps.has_output_drm = extension_present(dpy_exts, "EGL_EXT_output_drm") ||
                                extension_present(dpy_exts, "EGL_EXT_output_base");
    local_caps.has_khr_stream = extension_present(dpy_exts, "EGL_KHR_stream") ||
                                extension_present(dpy_exts, "EGL_KHR_stream_attrib");
    local_caps.has_stream_consumer_egloutput =
        extension_present(dpy_exts, "EGL_EXT_stream_consumer_egloutput");
    local_caps.has_nv_stream_consumer_eglimage =
        extension_present(dpy_exts, "EGL_NV_stream_consumer_eglimage");
    local_caps.has_stream_producer_eglsurface =
        extension_present(dpy_exts, "EGL_KHR_stream_producer_eglsurface");

    if (const char* v = rt.query_string(display, EGL_VENDOR); v != nullptr) {
      local_caps.vendor = v;
    }
    if (const char* v = rt.query_string(display, EGL_VERSION); v != nullptr) {
      local_caps.version = v;
    }
    if (const char* v = rt.query_string(display, EGL_CLIENT_APIS); v != nullptr) {
      local_caps.client_apis = v;
    }

    // eglTerminate is safe here: a subsequent eglInitialize on the same
    // display by an EglStreamSource consumer re-runs initialization
    // cleanly. The probe does not retain the EGLDisplay.
    rt.terminate(display);

    const bool has_streams = local_caps.has_output_drm && local_caps.has_khr_stream &&
                             local_caps.has_stream_consumer_egloutput;

    if (chosen == EGL_NO_DEVICE_EXT || has_streams) {
      chosen = egl_dev;
      chosen_caps = std::move(local_caps);
      chosen_has_streams = has_streams;
      if (has_streams) {
        break;
      }
    }
  }

  if (chosen == EGL_NO_DEVICE_EXT) {
    drm::log_info(
        "scene::stream_capability: no EGL device matches the DRM node — EGL Streams "
        "unsupported on this device");
    return cap;
  }

  cap.has_device_drm = chosen_caps.has_device_drm;
  cap.has_output_drm = chosen_caps.has_output_drm;
  cap.has_khr_stream = chosen_caps.has_khr_stream;
  cap.has_stream_consumer_egloutput = chosen_caps.has_stream_consumer_egloutput;
  cap.has_nv_stream_consumer_eglimage = chosen_caps.has_nv_stream_consumer_eglimage;
  cap.has_stream_producer_eglsurface = chosen_caps.has_stream_producer_eglsurface;
  cap.vendor = std::move(chosen_caps.vendor);
  cap.version = std::move(chosen_caps.version);
  cap.client_apis = std::move(chosen_caps.client_apis);

  if (chosen_has_streams) {
    // Conservative default. Phase 7.2 will add an empirical mixing
    // probe (test commit with stream consumer + FB-ID plane) that can
    // upgrade this to Mixed.
    cap.mixing = StreamMixingMode::Exclusive;
  }
  return cap;
}

#endif  // DRM_CXX_HAS_EGL_STREAMS

}  // namespace drm::scene
