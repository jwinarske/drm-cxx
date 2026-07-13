// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// stream_probe — hardware-validation CLI for drm::scene::probe_stream_capability.
//
// Usage: stream_probe [/dev/dri/cardN]
//
// Opens a DRM device (auto-selects if exactly one is present, prompts
// otherwise) and runs drm::scene::probe_stream_capability against it.
// Dumps the resulting StreamCapability to stdout in a human-readable
// form so users can see at a glance whether their system can host an
// EglStreamSource and what the driver advertises.
//
// What this validates end-to-end:
//
//   1. libEGL.so.1 is loadable on the host (or not — both are valid
//      results; Mesa systems may have no libEGL at all).
//   2. EGL client-side extensions (EGL_EXT_platform_device,
//      EGL_EXT_device_drm) determine whether the probe can even
//      enumerate per-DRM-node EGL devices. Mesa systems often advertise
//      these but the per-device extension set lacks streams.
//   3. The DRM-node matching (st_rdev compare) finds the EGL device
//      that wraps the same physical GPU as the caller's drm::Device.
//   4. The per-device-display extension query returns the streams
//      extension set (EGL_EXT_output_drm, EGL_KHR_stream,
//      EGL_EXT_stream_consumer_egloutput, etc.) when streams are
//      supported, and an empty set otherwise.
//
// Exit codes:
//   0 — probe ran cleanly (regardless of whether streams are usable).
//   1 — no DRM device available or DRM open failed.
//
// Output is read by humans; no machine-readable format. Pipe through
// grep / awk if scripting is wanted.

#include "common/select_device.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/scene/stream_capability.hpp>

#include <cstdlib>
#include <string>

namespace {

void dump(const drm::scene::StreamCapability& cap) {
  drm::println("EGL Streams capability");
  drm::println("  mixing            : {}", drm::scene::to_string(cap.mixing));
  drm::println("  usable            : {}", cap.usable() ? "yes" : "no");
  drm::println("");
  drm::println("EGL runtime");
  drm::println("  libEGL.so.1       : {}", cap.has_egl_runtime ? "loaded" : "absent");
  drm::println("");
  drm::println("Client-side extensions");
  drm::println("  EGL_EXT_platform_device  : {}", cap.has_platform_device ? "yes" : "no");
  drm::println("  EGL_EXT_device_drm       : {}", cap.has_device_drm ? "yes" : "no");
  drm::println("");
  drm::println("Per-device-display extensions");
  drm::println("  EGL_EXT_output_drm                : {}", cap.has_output_drm ? "yes" : "no");
  drm::println("  EGL_KHR_stream                    : {}", cap.has_khr_stream ? "yes" : "no");
  drm::println("  EGL_EXT_stream_consumer_egloutput : {}",
               cap.has_stream_consumer_egloutput ? "yes" : "no");
  drm::println("  EGL_NV_stream_consumer_eglimage   : {}",
               cap.has_nv_stream_consumer_eglimage ? "yes" : "no");
  drm::println("  EGL_KHR_stream_producer_eglsurface: {}",
               cap.has_stream_producer_eglsurface ? "yes" : "no");
  if (!cap.vendor.empty() || !cap.version.empty() || !cap.client_apis.empty()) {
    drm::println("");
    drm::println("EGL display strings");
    if (!cap.vendor.empty()) {
      drm::println("  vendor      : {}", cap.vendor);
    }
    if (!cap.version.empty()) {
      drm::println("  version     : {}", cap.version);
    }
    if (!cap.client_apis.empty()) {
      drm::println("  client APIs : {}", cap.client_apis);
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) try {
  const auto path = drm::examples::select_device(argc, argv);
  if (!path.has_value()) {
    return EXIT_FAILURE;
  }
  drm::println("Probing {}", *path);

  auto dev = drm::Device::open(*path);
  if (!dev) {
    drm::println(stderr, "Failed to open {}: {}", *path, dev.error().message());
    return EXIT_FAILURE;
  }

  const auto cap = drm::scene::probe_stream_capability(*dev);
  drm::println("");
  dump(cap);
  drm::println("");

  if (cap.usable()) {
    drm::println(
        "Streams are usable in {} mode. Pass this StreamCapability to "
        "LayerScene::Config::stream_capability to enable EglStreamSource layers.",
        drm::scene::to_string(cap.mixing));
  } else {
    drm::println(
        "Streams are NOT usable on this device. LayerScene::add_layer will reject "
        "BindingModel::DriverOwnsBinding sources. This is the expected result on "
        "Mesa-driven systems (amdgpu / i915 / lima / etc.).");
  }
  return EXIT_SUCCESS;
} catch (...) {
  return EXIT_FAILURE;
}
