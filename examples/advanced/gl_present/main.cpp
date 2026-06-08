// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// gl_present — minimal GL ScanoutProducer demo.
//
// Shows the whole present path driven by a GPU producer: a
// drm::present::GlScanoutProducer renders an animated clear color via EGL/GLES
// into a gbm_surface, and drm::present::ScanoutBackend discovers the output,
// negotiates modifiers, builds the single-layer scene, and commits each frame.
//
// The producer encapsulates the EGL display / config / context / window-surface
// setup, so the demo is just: create producer -> create backend -> per frame
// make_current, draw, swap_buffers, present.
//
// libEGL is dlopen'd by the library; this demo links libGLESv2 for the draw
// calls (glClear). Gated on egl + glesv2 like egl_scene; the library itself
// links neither.
//
//   ./gl_present [/dev/dri/cardN] [frames]
//
// Run from a free VT (it holds DRM master and modesets).

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/present/gl_scanout_producer.hpp>
#include <drm-cxx/present/scanout_backend.hpp>

#include <drm_fourcc.h>

#include <GLES2/gl2.h>
#include <cmath>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
  const std::string dev_path = (argc > 1) ? argv[1] : "/dev/dri/card0";
  const int frames = (argc > 2) ? std::atoi(argv[2]) : 120;

  auto dev = drm::Device::open(dev_path);
  if (!dev) {
    drm::println(stderr, "gl_present: open {}: {}", dev_path, dev.error().message());
    return EXIT_FAILURE;
  }

  auto producer = drm::present::GlScanoutProducer::create(*dev);
  if (!producer) {
    drm::println(stderr, "gl_present: GlScanoutProducer::create: {} (no usable libEGL?)",
                 producer.error().message());
    return EXIT_FAILURE;
  }

  drm::present::ScanoutBackend::Config cfg;
  cfg.fourcc = DRM_FORMAT_ARGB8888;
  auto backend = drm::present::ScanoutBackend::create(*dev, **producer, cfg);
  if (!backend) {
    drm::println(stderr, "gl_present: ScanoutBackend::create: {}", backend.error().message());
    return EXIT_FAILURE;
  }

  const auto& target = (*backend)->target();
  drm::println("gl_present: {}x{} on {} (driver {}), {} negotiated modifier(s)",
               target.mode.hdisplay, target.mode.vdisplay, dev_path, (*backend)->profile().name,
               (*backend)->modifiers().size());

  for (int frame = 0; frame < frames; ++frame) {
    if (auto r = (*producer)->make_current(); !r) {
      drm::println(stderr, "gl_present: make_current: {}", r.error().message());
      return EXIT_FAILURE;
    }
    // Cycle the clear color over one full period across `frames` (2*pi).
    const float phase = (static_cast<float>(frame) / static_cast<float>(frames)) * 6.2831853F;
    glViewport(0, 0, target.mode.hdisplay, target.mode.vdisplay);
    glClearColor(0.5F + (0.5F * std::sin(phase)), 0.3F, 0.5F + (0.5F * std::cos(phase)), 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    if (auto r = (*producer)->swap_buffers(); !r) {
      drm::println(stderr, "gl_present: swap_buffers: {}", r.error().message());
      return EXIT_FAILURE;
    }
    if (auto r = (*backend)->present(0); !r) {
      drm::println(stderr, "gl_present: present: {}", r.error().message());
      return EXIT_FAILURE;
    }
  }

  drm::println("gl_present: presented {} frames", frames);
  return EXIT_SUCCESS;
}
