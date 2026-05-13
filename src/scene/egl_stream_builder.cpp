// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "egl_stream_builder.hpp"

#if DRM_CXX_HAS_EGL_STREAMS

#include "buffer_source.hpp"
#include "egl_runtime.hpp"
#include "egl_stream_source.hpp"
#include "stream_capability.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/log.hpp>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cstddef>
#include <memory>
#include <sys/stat.h>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::scene {

namespace {

using detail::egl_runtime;
using detail::extension_present;

std::error_code make_errc(std::errc e) noexcept {
  return std::make_error_code(e);
}

// Re-find the EGLDeviceEXT whose backing DRM node matches the
// caller's drm::Device by st_rdev. Mirrors probe_stream_capability's
// device-matching loop: on hybrid stacks where more than one
// EGLDeviceEXT reports the same DRM node we prefer the one with
// EGL_EXT_device_drm in its per-device extension string.
EGLDeviceEXT match_device(const drm::Device& dev) noexcept {
  const auto& rt = egl_runtime();
  if ((rt.query_devices == nullptr) || (rt.query_device_string == nullptr)) {
    return EGL_NO_DEVICE_EXT;
  }
  if (dev.fd() < 0) {
    return EGL_NO_DEVICE_EXT;
  }

  EGLint num = 0;
  if (rt.query_devices(0, nullptr, &num) != EGL_TRUE || num <= 0) {
    return EGL_NO_DEVICE_EXT;
  }
  std::vector<EGLDeviceEXT> devices(static_cast<std::size_t>(num));
  if (rt.query_devices(num, devices.data(), &num) != EGL_TRUE) {
    return EGL_NO_DEVICE_EXT;
  }
  devices.resize(static_cast<std::size_t>(num));

  struct ::stat dev_stat{};
  if (::fstat(dev.fd(), &dev_stat) != 0) {
    return EGL_NO_DEVICE_EXT;
  }

  EGLDeviceEXT first_match = EGL_NO_DEVICE_EXT;
  for (auto* egl_dev : devices) {
    const char* drm_path = rt.query_device_string(egl_dev, EGL_DRM_DEVICE_FILE_EXT);
    if ((drm_path == nullptr) || (*drm_path == '\0')) {
      continue;
    }
    struct ::stat egl_stat{};
    if (::stat(drm_path, &egl_stat) != 0) {
      continue;
    }
    if (egl_stat.st_rdev != dev_stat.st_rdev) {
      continue;
    }
    if (first_match == EGL_NO_DEVICE_EXT) {
      first_match = egl_dev;
    }
    // Prefer the device whose per-device extensions advertise
    // EGL_EXT_device_drm. The probe already weighed per-display
    // streams support; we trust its verdict and don't re-init each
    // display here.
    const char* dev_exts = rt.query_device_string(egl_dev, EGL_EXTENSIONS);
    if (extension_present(dev_exts, "EGL_EXT_device_drm")) {
      return egl_dev;
    }
  }
  return first_match;
}

// Pick an EGLConfig usable as both a stream producer surface target
// and a GLES rendering context. RGBA8888 + EGL_STREAM_BIT_KHR is the
// only mode the initial drop supports; richer formats land when a
// real workload calls for them.
EGLConfig choose_config(EGLDisplay display) noexcept {
  const auto& rt = egl_runtime();
  if (rt.choose_config == nullptr) {
    return nullptr;
  }
  const EGLint attribs[] = {
      EGL_SURFACE_TYPE,
      EGL_STREAM_BIT_KHR,
      // ES2_BIT also matches ES3-capable configs (the driver
      // typically reports both bits on the same config). Filtering
      // by the lower bit gives us the widest usable set.
      EGL_RENDERABLE_TYPE,
      EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE,
      8,
      EGL_GREEN_SIZE,
      8,
      EGL_BLUE_SIZE,
      8,
      EGL_ALPHA_SIZE,
      8,
      EGL_DEPTH_SIZE,
      0,
      EGL_STENCIL_SIZE,
      0,
      EGL_NONE,
  };
  EGLConfig config = nullptr;
  EGLint num = 0;
  if (rt.choose_config(display, attribs, &config, 1, &num) != EGL_TRUE || num == 0) {
    return nullptr;
  }
  return config;
}

// Create a GLES 3.x context with no surface requirement. The user
// will make this current with the producer surface when they want to
// render.
EGLContext create_gles_context(EGLDisplay display, EGLConfig config) noexcept {
  const auto& rt = egl_runtime();
  if ((rt.create_context == nullptr) || (rt.bind_api == nullptr)) {
    return EGL_NO_CONTEXT;
  }
  // Bind ES API for context creation. Per-thread state — affects
  // subsequent context-creating calls on this thread.
  if (rt.bind_api(EGL_OPENGL_ES_API) != EGL_TRUE) {
    return EGL_NO_CONTEXT;
  }
  const EGLint ctx_attribs[] = {
      EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE,
  };
  return rt.create_context(display, config, EGL_NO_CONTEXT, ctx_attribs);
}

}  // namespace

drm::expected<EglStreamBuilder::Result, std::error_code> EglStreamBuilder::build(
    const Request& req) {
  if (!req.capability.usable()) {
    return drm::unexpected<std::error_code>(make_errc(std::errc::function_not_supported));
  }
  if (req.format.width == 0 || req.format.height == 0) {
    return drm::unexpected<std::error_code>(make_errc(std::errc::invalid_argument));
  }
  const auto& rt = egl_runtime();
  if (!rt.loaded || (rt.initialize == nullptr) || (rt.get_platform_display == nullptr)) {
    return drm::unexpected<std::error_code>(make_errc(std::errc::function_not_supported));
  }

  Result result;

  // Display: reuse or create.
  if (req.existing_display != EGL_NO_DISPLAY) {
    result.display = req.existing_display;
  } else {
    if (req.device == nullptr) {
      return drm::unexpected<std::error_code>(make_errc(std::errc::invalid_argument));
    }
    EGLDeviceEXT egl_dev = match_device(*req.device);
    if (egl_dev == EGL_NO_DEVICE_EXT) {
      drm::log_warn("EglStreamBuilder: no EGL device matches the DRM node");
      return drm::unexpected<std::error_code>(make_errc(std::errc::no_such_device));
    }
    result.display = rt.get_platform_display(EGL_PLATFORM_DEVICE_EXT, egl_dev, nullptr);
    if (result.display == EGL_NO_DISPLAY) {
      drm::log_warn("EglStreamBuilder: eglGetPlatformDisplayEXT failed (egl 0x{:x})",
                    rt.get_error != nullptr ? rt.get_error() : 0);
      return drm::unexpected<std::error_code>(make_errc(std::errc::io_error));
    }
    EGLint major = 0;
    EGLint minor = 0;
    if (rt.initialize(result.display, &major, &minor) != EGL_TRUE) {
      drm::log_warn("EglStreamBuilder: eglInitialize failed (egl 0x{:x})",
                    rt.get_error != nullptr ? rt.get_error() : 0);
      return drm::unexpected<std::error_code>(make_errc(std::errc::io_error));
    }
  }

  result.egl_config = choose_config(result.display);
  if (result.egl_config == nullptr) {
    drm::log_warn("EglStreamBuilder: eglChooseConfig found no streams-capable RGBA8888 config");
    return drm::unexpected<std::error_code>(make_errc(std::errc::io_error));
  }

  // Context: reuse or create. The created-by-builder flag drives the
  // error-path cleanup below — if the source creation fails after we
  // allocated a context, destroy that context to avoid leaking it.
  if (req.existing_context != EGL_NO_CONTEXT) {
    result.context = req.existing_context;
    result.context_created_by_builder = false;
  } else {
    result.context = create_gles_context(result.display, result.egl_config);
    if (result.context == EGL_NO_CONTEXT) {
      drm::log_warn("EglStreamBuilder: eglCreateContext failed (egl 0x{:x})",
                    rt.get_error != nullptr ? rt.get_error() : 0);
      return drm::unexpected<std::error_code>(make_errc(std::errc::io_error));
    }
    result.context_created_by_builder = true;
  }

  const EglStreamSource::Config src_cfg{
      .display = result.display,
      .egl_config = result.egl_config,
      .format = req.format,
  };
  auto src = EglStreamSource::create(req.capability, src_cfg);
  if (!src) {
    // Source creation failed. Don't terminate the display (it may be
    // process-singleton and used elsewhere), but do clean up the
    // context we just allocated — leaking a context across a failed
    // build would surprise callers who track resource counts.
    if (result.context_created_by_builder && (rt.destroy_context != nullptr)) {
      rt.destroy_context(result.display, result.context);
    }
    return drm::unexpected<std::error_code>(src.error());
  }

  // Snapshot the producer-side handles into the result. Identity is
  // stable until a bind_to_plane rebind event, at which point the
  // source recreates them and the cached values here go stale —
  // documented in the header.
  result.producer_surface = (*src)->producer_surface();
  result.stream = (*src)->stream();
  result.source = std::move(*src);
  return result;
}

}  // namespace drm::scene

#endif  // DRM_CXX_HAS_EGL_STREAMS
