// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "egl_stream_source.hpp"

#if DRM_CXX_HAS_EGL_STREAMS

#include "buffer_source.hpp"
#include "egl_runtime.hpp"
#include "stream_capability.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/log.hpp>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cstdint>
#include <memory>
#include <system_error>

namespace drm::scene {

namespace {

using detail::egl_runtime;

std::error_code make_errc(std::errc e) noexcept {
  return std::make_error_code(e);
}

}  // namespace

drm::expected<std::unique_ptr<EglStreamSource>, std::error_code> EglStreamSource::create(
    const StreamCapability& cap, Config config) {
  if (!cap.usable() || !cap.has_khr_stream || !cap.has_output_drm ||
      !cap.has_stream_consumer_egloutput || !cap.has_stream_producer_eglsurface) {
    return drm::unexpected<std::error_code>(make_errc(std::errc::function_not_supported));
  }
  if (config.display == EGL_NO_DISPLAY || config.egl_config == nullptr ||
      config.format.width == 0 || config.format.height == 0) {
    return drm::unexpected<std::error_code>(make_errc(std::errc::invalid_argument));
  }

  const auto& rt = egl_runtime();
  if (!rt.loaded || rt.create_stream == nullptr || rt.create_stream_producer_surface == nullptr ||
      rt.destroy_stream == nullptr) {
    return drm::unexpected<std::error_code>(make_errc(std::errc::function_not_supported));
  }

  std::unique_ptr<EglStreamSource> source(new EglStreamSource(config));
  if (auto r = source->create_stream_and_producer(); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  return source;
}

EglStreamSource::EglStreamSource(Config cfg) noexcept : config_(cfg) {}

EglStreamSource::~EglStreamSource() {
  destroy_stream_and_producer();
}

drm::expected<void, std::error_code> EglStreamSource::create_stream_and_producer() noexcept {
  const auto& rt = egl_runtime();

  // Empty attribute list: producer/consumer latency, FIFO depth, and
  // acquire timeout default to the driver's choices. Tuning lands
  // later with a real workload to measure against — opinionated
  // defaults here would be guesses.
  const EGLint stream_attribs[] = {EGL_NONE};
  stream_ = rt.create_stream(config_.display, stream_attribs);
  if (stream_ == EGL_NO_STREAM_KHR) {
    drm::log_warn("EglStreamSource: eglCreateStreamKHR failed (egl error 0x{:x})",
                  rt.get_error != nullptr ? rt.get_error() : 0);
    return drm::unexpected<std::error_code>(make_errc(std::errc::io_error));
  }

  // Producer surface dimensions match the source's declared format.
  // The user's GL/GLES context renders into this surface; eglSwapBuffers
  // pushes the frame into the stream for the consumer to acquire.
  const EGLint surface_attribs[] = {
      EGL_WIDTH,  static_cast<EGLint>(config_.format.width),
      EGL_HEIGHT, static_cast<EGLint>(config_.format.height),
      EGL_NONE,
  };
  producer_surface_ = rt.create_stream_producer_surface(config_.display, config_.egl_config,
                                                        stream_, surface_attribs);
  if (producer_surface_ == EGL_NO_SURFACE) {
    drm::log_warn("EglStreamSource: eglCreateStreamProducerSurfaceKHR failed (egl error 0x{:x})",
                  rt.get_error != nullptr ? rt.get_error() : 0);
    rt.destroy_stream(config_.display, stream_);
    stream_ = EGL_NO_STREAM_KHR;
    return drm::unexpected<std::error_code>(make_errc(std::errc::io_error));
  }
  return {};
}

void EglStreamSource::destroy_stream_and_producer() noexcept {
  const auto& rt = egl_runtime();
  if (producer_surface_ != EGL_NO_SURFACE) {
    if (rt.destroy_surface != nullptr) {
      rt.destroy_surface(config_.display, producer_surface_);
    }
    producer_surface_ = EGL_NO_SURFACE;
  }
  if (stream_ != EGL_NO_STREAM_KHR) {
    if (rt.destroy_stream != nullptr) {
      rt.destroy_stream(config_.display, stream_);
    }
    stream_ = EGL_NO_STREAM_KHR;
  }
  bound_plane_id_.reset();
}

drm::expected<AcquiredBuffer, std::error_code> EglStreamSource::acquire() {
  // EAGAIN tells the scene to skip this layer for the current frame.
  // We return it whenever the stream isn't in a state that can
  // contribute to scanout: session paused (DRM master gone), or the
  // stream consumer hasn't been wired to a plane yet (the allocator
  // hasn't picked one, or unbind_from_plane just ran).
  if (session_paused_ || stream_ == EGL_NO_STREAM_KHR || !bound_plane_id_.has_value()) {
    return drm::unexpected<std::error_code>(make_errc(std::errc::resource_unavailable_try_again));
  }
  // FB_ID stays zero — the scene's lower_layer path keys off
  // binding_model() == DriverOwnsBinding to skip the property write,
  // and the kernel plane state is already configured by the stream
  // consumer binding from bind_to_plane().
  return AcquiredBuffer{};
}

void EglStreamSource::release(AcquiredBuffer /*acquired*/) noexcept {
  // Nothing to do — acquire() handed out no resources the scene needs
  // to give back. The stream's producer/consumer state machine
  // handles frame retirement on its own.
}

drm::expected<void, std::error_code> EglStreamSource::bind_to_plane(std::uint32_t plane_id) {
  const auto& rt = egl_runtime();
  if (rt.get_output_layers == nullptr || rt.stream_consumer_output == nullptr) {
    return drm::unexpected<std::error_code>(make_errc(std::errc::function_not_supported));
  }
  if (session_paused_) {
    return drm::unexpected<std::error_code>(make_errc(std::errc::resource_unavailable_try_again));
  }
  if (stream_ == EGL_NO_STREAM_KHR) {
    return drm::unexpected<std::error_code>(make_errc(std::errc::resource_unavailable_try_again));
  }

  // Idempotent on the same plane — the scene may call bind_to_plane
  // every commit; only the first one does real work.
  if (bound_plane_id_.has_value() && *bound_plane_id_ == plane_id) {
    return {};
  }

  // Cross-plane reassignment: most drivers don't support retargeting an
  // active stream consumer in place. Tear down and recreate. This
  // invalidates the user's producer_surface() handle — callers caching
  // it across rebinds must re-fetch, which is why the public docs
  // mark the accessor's return value as "changes across rebinds."
  if (bound_plane_id_.has_value()) {
    drm::log_info("EglStreamSource: rebinding plane {} -> {}", *bound_plane_id_, plane_id);
    destroy_stream_and_producer();
    if (auto r = create_stream_and_producer(); !r) {
      return drm::unexpected<std::error_code>(r.error());
    }
  }

  // Enumerate the EGLOutputLayer that wraps the requested DRM plane.
  // EGL_EXT_output_drm exposes one layer per plane the device can
  // drive; filtering by EGL_DRM_PLANE_EXT pulls just the matching
  // one. A return of zero layers means the EGL device doesn't expose
  // this plane (likely a CRTC/connector mismatch upstream).
  const EGLAttrib filter[] = {
      EGL_DRM_PLANE_EXT,
      static_cast<EGLAttrib>(plane_id),
      EGL_NONE,
  };
  EGLOutputLayerEXT layer = EGL_NO_OUTPUT_LAYER_EXT;
  EGLint num_layers = 0;
  if (rt.get_output_layers(config_.display, filter, &layer, 1, &num_layers) != EGL_TRUE ||
      num_layers == 0) {
    drm::log_warn("EglStreamSource: no EGL output layer for plane {} (egl error 0x{:x})", plane_id,
                  rt.get_error != nullptr ? rt.get_error() : 0);
    return drm::unexpected<std::error_code>(make_errc(std::errc::no_such_device));
  }

  // Bind the stream consumer. After this returns success the kernel
  // sees the plane as driven by the stream; subsequent frames
  // produced into the surface scan out without any FB_ID write from
  // the scene.
  if (rt.stream_consumer_output(config_.display, stream_, layer) != EGL_TRUE) {
    drm::log_warn(
        "EglStreamSource: eglStreamConsumerOutputEXT failed for plane {} (egl error 0x{:x})",
        plane_id, rt.get_error != nullptr ? rt.get_error() : 0);
    return drm::unexpected<std::error_code>(make_errc(std::errc::io_error));
  }

  bound_plane_id_ = plane_id;
  return {};
}

void EglStreamSource::unbind_from_plane(std::uint32_t plane_id) noexcept {
  // Spurious unbind (e.g. the layer wasn't on us): ignore. Matches
  // the noexcept release() pattern — buffer-source teardown paths
  // are expected to swallow mismatches.
  if (!bound_plane_id_.has_value() || *bound_plane_id_ != plane_id) {
    return;
  }
  // Destroy + recreate. The stream cannot be retargeted in place on
  // most drivers, and even where it can the state machine would need
  // to be drained first. A fresh stream is simpler and matches what
  // the scene expects from bind_to_plane being called next.
  destroy_stream_and_producer();
  if (auto r = create_stream_and_producer(); !r) {
    drm::log_warn("EglStreamSource: failed to recreate stream after unbind from plane {}: {}",
                  plane_id, r.error().message());
  }
}

void EglStreamSource::on_session_paused() noexcept {
  // The DRM fd is going away; the EGL display is bound to it
  // indirectly through the kernel plane state. Tearing the stream
  // down here keeps the destructor and the resume path from
  // dereferencing dead state. acquire() returns EAGAIN while paused.
  session_paused_ = true;
  destroy_stream_and_producer();
}

drm::expected<void, std::error_code> EglStreamSource::on_session_resumed(
    const drm::Device& /*new_dev*/) {
  // The EGLDisplay typically survives session pause (it's bound to
  // the EGLDeviceEXT, not the DRM fd). Recreate the stream + producer
  // surface and wait for the scene to call bind_to_plane again.
  session_paused_ = false;
  if (auto r = create_stream_and_producer(); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  return {};
}

}  // namespace drm::scene

#endif  // DRM_CXX_HAS_EGL_STREAMS
