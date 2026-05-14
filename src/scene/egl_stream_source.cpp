// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "egl_stream_source.hpp"

#if DRM_CXX_HAS_EGL_STREAMS

#include "buffer_source.hpp"
#include "egl_runtime.hpp"
#include "stream_capability.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/log.hpp>
#include <drm-cxx/modeset/atomic.hpp>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglplatform.h>
#include <array>
#include <cstdint>
#include <memory>
#include <system_error>

namespace {
// EGL_NV_stream_attrib + EGL_NV_output_drm_atomic +
// EGL_NV_output_drm_flip_event constants. Not in the system
// /usr/include/EGL/eglext.h; values from NVIDIA's eglext_nv.h
// shipped with the JetPack SDK. Anonymous-namespace constants
// keep the macro-as-constant lint quiet without losing the
// "EGL_..._EXT" / "_NV" naming the upstream headers use.
constexpr EGLenum egl_consumer_auto_acquire_ext = 0x332B;
constexpr EGLAttrib egl_drm_atomic_request_nv = 0x3333;
constexpr EGLint egl_drm_flip_event_data_nv = 0x333E;
}  // namespace

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
  if (auto r = source->create_stream(); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  // Producer surface creation is deferred until bind_to_plane() —
  // NVIDIA's driver rejects producer attachment to a stream that
  // doesn't yet have a consumer, so producer surface lives behind
  // the consumer-bind call.
  return source;
}

EglStreamSource::EglStreamSource(Config cfg) noexcept : config_(cfg) {}

EglStreamSource::~EglStreamSource() {
  destroy_stream_and_producer();
}

drm::expected<void, std::error_code> EglStreamSource::create_stream() noexcept {
  const auto& rt = egl_runtime();

  // FIFO length 0 = mailbox mode: producer swaps never block waiting
  // for the consumer to retire prior frames. EGL_CONSUMER_AUTO_ACQUIRE
  // is left at its default (TRUE) so NVIDIA's driver pulls each
  // producer frame and arms the consumer plane internally — desktop
  // NVIDIA doesn't support EGL_NV_output_drm_atomic, so there's no
  // Tegra-style EGL_DRM_ATOMIC_REQUEST_NV first-frame handoff
  // available; the consumer drives plane updates on its own once a
  // frame lands in the stream.
  const EGLint stream_attribs[] = {
      EGL_STREAM_FIFO_LENGTH_KHR,
      0,
      EGL_NONE,
  };
  stream_ = rt.create_stream(config_.display, stream_attribs);
  if (stream_ == EGL_NO_STREAM_KHR) {
    drm::log_warn("EglStreamSource: eglCreateStreamKHR failed (egl error 0x{:x})",
                  rt.get_error != nullptr ? rt.get_error() : 0);
    return drm::unexpected<std::error_code>(make_errc(std::errc::io_error));
  }
  return {};
}

drm::expected<void, std::error_code> EglStreamSource::create_producer_surface() noexcept {
  const auto& rt = egl_runtime();

  // The producer surface can only be created AFTER a consumer is
  // attached on NVIDIA's implementation: eglCreateStreamProducerSurfaceKHR
  // returns EGL_BAD_STATE_KHR on a freshly-created stream with no
  // consumer. The KHR spec is permissive about ordering; the driver
  // isn't. Callers should invoke this from bind_to_plane after the
  // consumer-side eglStreamConsumerOutputEXT call returns success.
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
  // The flip-event identifier is keyed to the destroyed consumer
  // layer; a re-bind queries a fresh value if the new layer
  // supports the extension.
  flip_event_data_.reset();
  // Fresh stream means we need to drive a fresh first-frame prime
  // before NVIDIA's auto-acquire path can take over.
  first_frame_primed_ = false;
}

drm::expected<void, std::error_code> EglStreamSource::prime_first_commit(drm::AtomicRequest& req) {
  if (first_frame_primed_) {
    return {};
  }
  const auto& rt = egl_runtime();
  if (rt.stream_consumer_acquire_attrib == nullptr || rt.stream_attrib == nullptr) {
    return drm::unexpected<std::error_code>(make_errc(std::errc::function_not_supported));
  }
  if (stream_ == EGL_NO_STREAM_KHR || !bound_plane_id_.has_value()) {
    return drm::unexpected<std::error_code>(make_errc(std::errc::resource_unavailable_try_again));
  }
  auto* atomic = req.native_handle();
  if (atomic == nullptr) {
    return drm::unexpected<std::error_code>(make_errc(std::errc::invalid_argument));
  }

  // Hand the atomic request to NVIDIA. The driver fills in FB_ID
  // for the stream's first frame and submits the commit itself —
  // the caller MUST NOT call req.commit() after this returns
  // success, or the kernel sees the same state twice.
  const std::array<EGLAttrib, 3> attrs{
      egl_drm_atomic_request_nv,
      reinterpret_cast<EGLAttrib>(atomic),
      EGL_NONE,
  };
  if (rt.stream_consumer_acquire_attrib(config_.display, stream_, attrs.data()) != EGL_TRUE) {
    drm::log_warn(
        "EglStreamSource: eglStreamConsumerAcquireAttribKHR (first-frame prime) failed "
        "(egl error 0x{:x})",
        rt.get_error != nullptr ? rt.get_error() : 0);
    return drm::unexpected<std::error_code>(make_errc(std::errc::io_error));
  }

  // Re-enable auto-acquire so the producer's subsequent
  // eglSwapBuffers calls drive plane updates through NVIDIA's
  // internal commits, no scene intervention required.
  if (rt.stream_attrib(config_.display, stream_,
                       static_cast<EGLenum>(egl_consumer_auto_acquire_ext), EGL_TRUE) != EGL_TRUE) {
    drm::log_warn("EglStreamSource: failed to re-enable auto-acquire (egl error 0x{:x})",
                  rt.get_error != nullptr ? rt.get_error() : 0);
    // Not fatal — the stream still has a consumer-side queue. The
    // caller can keep driving frames; auto-acquire just remains off.
  }
  first_frame_primed_ = true;
  return {};
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
    if (auto r = create_stream(); !r) {
      return drm::unexpected<std::error_code>(r.error());
    }
    // Producer surface stays absent for now — created below after
    // the consumer-bind call succeeds on the new plane.
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

  // Now that the consumer is attached, create the producer surface.
  // This is the order NVIDIA's implementation requires; the
  // EGL_KHR_stream_producer_eglsurface spec is permissive but the
  // driver returns EGL_BAD_STATE_KHR when called against a
  // consumer-less stream.
  if (producer_surface_ == EGL_NO_SURFACE) {
    if (auto r = create_producer_surface(); !r) {
      return drm::unexpected<std::error_code>(r.error());
    }
  }

  // Query the NVIDIA flip-event identifier on the consumer layer.
  // When the driver exports EGL_NV_output_drm_flip_event, this is
  // the user_data value the kernel passes back on drm vblank
  // events for the consumer plane -- callers route flips to the
  // right source by matching it. Absence is informational, not
  // an error: Mesa and older proprietary drivers don't export
  // the extension and the query returns EGL_FALSE with no event
  // identifier available.
  if (rt.query_output_layer_attrib != nullptr) {
    EGLAttrib data = 0;
    if (rt.query_output_layer_attrib(config_.display, layer, egl_drm_flip_event_data_nv, &data) ==
        EGL_TRUE) {
      flip_event_data_ = static_cast<std::uint64_t>(data);
    }
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
  // Destroy + recreate the stream. The producer surface stays gone
  // until the next bind_to_plane attaches a fresh consumer. The
  // stream cannot be retargeted in place on most drivers, and even
  // where it can the state machine would need to be drained first.
  destroy_stream_and_producer();
  if (auto r = create_stream(); !r) {
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
  // Honest contract: the source's EGLDisplay was created by the
  // builder against the *original* DRM fd via EGL_DRM_MASTER_FD_EXT
  // (see [[reference_nvidia_egl_drm_master_fd]]). libseat closes that
  // fd on pause and hands the scene a new one on resume, so the
  // display's internal master-fd reference is now stale on every
  // driver that honors the attribute (NVIDIA proprietary; Mesa
  // ignores it). The source doesn't own the display and can't
  // rebuild it in place — the EGLDeviceEXT match lives in the
  // builder, and the caller retains display ownership per the
  // `Config` doc.
  //
  // Probe the display with a cheap eglQueryString(EGL_VENDOR). When
  // the underlying device fd is dead, NVIDIA returns null with
  // EGL_BAD_DISPLAY. Mesa keeps the display alive across fd
  // turnover, so the probe passes there. On probe failure we surface
  // a clear error so the caller knows to destroy this source and let
  // the builder construct a fresh one against the new fd; the source
  // remains in a paused-equivalent state (stream torn down, acquire
  // returns EAGAIN) so a subsequent commit doesn't crash inside
  // mesa.
  const auto& rt = egl_runtime();
  if (rt.query_string != nullptr) {
    if (rt.query_string(config_.display, EGL_VENDOR) == nullptr) {
      drm::log_warn(
          "EglStreamSource: EGLDisplay is unusable after session resume — the display was bound to "
          "the original DRM master fd, which libseat has closed. Caller must destroy this source "
          "and rebuild via EglStreamBuilder against the new device.");
      return drm::unexpected<std::error_code>(make_errc(std::errc::not_connected));
    }
  }
  session_paused_ = false;
  if (auto r = create_stream(); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  return {};
}

}  // namespace drm::scene

#endif  // DRM_CXX_HAS_EGL_STREAMS
