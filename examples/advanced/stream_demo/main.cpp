// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// stream_demo — minimal EGL-streams-end-to-end demo for the
// drm::scene::EglStreamBuilder API.
//
// What it does:
//
//   1. Opens a DRM card (argv-selectable, prompts otherwise) and
//      picks the first connected output.
//   2. Probes EGL Streams capability. Exits with a friendly message
//      if streams aren't usable on this host (Mesa-only systems,
//      missing libEGL_nvidia.so.0, etc.).
//   3. Builds a `LayerScene` against the picked CRTC.
//   4. Adds a dumb-buffer background layer covering the full screen
//      (dark gray) so PRIMARY has an FB before modeset.
//   5. Builds an EGL stream source via `EglStreamBuilder` at
//      640x360 ARGB8888 and adds it as a foreground layer in the
//      center of the screen.
//   6. Makes the producer-side EGLSurface + GLES 3 context current.
//   7. Runs an animation loop: `glClearColor` cycles through a hue
//      ramp, `eglSwapBuffers` pushes each frame into the stream.
//      The kernel scans the latest stream frame out via the
//      consumer-bound plane without any per-frame scene commit
//      (the consumer extension drives the plane state).
//   8. Holds for `--seconds N` (default 5) then tears down cleanly.
//
// CLI:
//
//   stream_demo [--seconds N] [/dev/dri/cardN]
//
// Build gate: the entire file is `#if DRM_CXX_HAS_EGL_STREAMS`. The
// CMake / meson rules below the source guard the executable on the
// same flag plus availability of `glesv2.pc`. Without one of those
// the binary is not produced.

#include "common/open_output.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/resources.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/modeset/mode.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/scene/stream_capability.hpp>

#include <drm_fourcc.h>
#include <xf86drmMode.h>

#if DRM_CXX_HAS_EGL_STREAMS
#include <drm-cxx/scene/egl_stream_builder.hpp>
#include <drm-cxx/scene/egl_stream_source.hpp>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

namespace {

constexpr std::uint32_t k_stream_width = 640;
constexpr std::uint32_t k_stream_height = 360;
constexpr int k_default_seconds = 5;

struct Args {
  int seconds{k_default_seconds};
};

[[nodiscard]] Args parse_args(int& argc, char**& argv) {
  Args a;
  int write = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--seconds" && (i + 1) < argc) {
      a.seconds = std::atoi(argv[++i]);
    } else {
      argv[write++] = argv[i];
    }
  }
  argc = write;
  return a;
}

struct PickedOutput {
  std::uint32_t crtc_id{0};
  std::uint32_t connector_id{0};
  drmModeModeInfo mode{};
};

// Permissive output picker for demo use. Walks DRM resources and
// finds the first CONNECTED connector with at least one mode; if the
// connector hasn't been modeset yet (`encoder_id == 0`, typical on a
// fresh TTY-only host that's never had a compositor running) it
// falls back to the first encoder in `connector->encoders[]` and
// picks a CRTC from `encoder->possible_crtcs`. The shared
// open_and_pick_output helper requires `encoder_id != 0` and
// `crtc_id != 0`, which only holds after a prior compositor session
// has bound the connector — too restrictive for first-light
// validation on this host.
[[nodiscard]] std::optional<PickedOutput> pick_output(int fd) {
  const auto res = drm::get_resources(fd);
  if (!res) {
    return std::nullopt;
  }
  const auto connector_ids = drm::span<const std::uint32_t>(res->connectors, res->count_connectors);
  for (const auto cid : connector_ids) {
    const auto conn = drm::get_connector(fd, cid);
    if (!conn || conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0) {
      continue;
    }
    // Current encoder if the kernel has one bound, otherwise the
    // first encoder the connector advertises.
    std::uint32_t enc_id = conn->encoder_id;
    if (enc_id == 0 && conn->count_encoders > 0) {
      enc_id = conn->encoders[0];
    }
    if (enc_id == 0) {
      continue;
    }
    const auto enc = drm::get_encoder(fd, enc_id);
    if (!enc) {
      continue;
    }
    // Current CRTC if bound, otherwise the first CRTC allowed by
    // `possible_crtcs` — atomic modeset will wire it up on first
    // commit.
    std::uint32_t crtc_id = enc->crtc_id;
    if (crtc_id == 0) {
      for (int c = 0; c < res->count_crtcs; ++c) {
        if ((enc->possible_crtcs & (1U << static_cast<unsigned>(c))) != 0) {
          crtc_id = res->crtcs[c];
          break;
        }
      }
    }
    if (crtc_id == 0) {
      continue;
    }
    const auto mode_res = drm::select_preferred_mode(
        drm::span<const drmModeModeInfo>(conn->modes, conn->count_modes));
    return PickedOutput{crtc_id, cid, mode_res ? mode_res->drm_mode : conn->modes[0]};
  }
  return std::nullopt;
}

}  // namespace

#if !DRM_CXX_HAS_EGL_STREAMS

int main(int /*argc*/, char* /*argv*/[]) try {
  drm::println(stderr,
               "stream_demo: drm-cxx was built without EGL Streams "
               "(-DDRM_CXX_STREAMS=OFF or EGL headers absent). Rebuild "
               "with streams enabled to use this demo.");
  return EXIT_FAILURE;
} catch (...) {
  return EXIT_FAILURE;
}

#else  // DRM_CXX_HAS_EGL_STREAMS

int main(int argc, char* argv[]) try {
  const auto args = parse_args(argc, argv);

  auto ctx = drm::examples::open_device(argc, argv);
  if (!ctx) {
    return EXIT_FAILURE;
  }
  auto& device = ctx->device;

  const auto picked = pick_output(device.fd());
  if (!picked) {
    drm::println(stderr,
                 "stream_demo: no CONNECTED connector with modes on this device. "
                 "Plug a display in and ensure no other process is holding master.");
    return EXIT_FAILURE;
  }
  drm::println("stream_demo: picked crtc={} connector={} mode={}x{}@{}Hz", picked->crtc_id,
               picked->connector_id, picked->mode.hdisplay, picked->mode.vdisplay,
               picked->mode.vrefresh);

  // Probe streams capability before doing anything else; bail cleanly
  // on hosts that can't run this demo.
  const auto cap = drm::scene::probe_stream_capability(device);
  if (!cap.usable()) {
    drm::println(stderr, "stream_demo: EGL Streams not usable on this device. The probe reports:");
    drm::println(stderr, "  libEGL.so.1               : {}", cap.has_egl_runtime ? "yes" : "no");
    drm::println(stderr, "  EGL_EXT_platform_device   : {}",
                 cap.has_platform_device ? "yes" : "no");
    drm::println(stderr, "  EGL_KHR_stream            : {}", cap.has_khr_stream ? "yes" : "no");
    drm::println(stderr, "  EGL_EXT_stream_consumer_egloutput : {}",
                 cap.has_stream_consumer_egloutput ? "yes" : "no");
    drm::println(
        stderr,
        "  (run `stream_probe` for the full breakdown — typical: Mesa-only stack, no NVIDIA "
        "userspace.)");
    return EXIT_FAILURE;
  }
  drm::println("stream_demo: EGL Streams usable in {} mode (vendor={}, version={})",
               drm::scene::to_string(cap.mixing), cap.vendor, cap.version);

  const std::uint32_t fb_w = picked->mode.hdisplay;
  const std::uint32_t fb_h = picked->mode.vdisplay;

  // Background layer — a full-screen dumb buffer keeps PRIMARY armed
  // for the first commit's modeset. Pixels are left as the kernel's
  // zero-init (black); the stream layer above provides the visual.
  drm::scene::LayerScene::Config scene_cfg;
  scene_cfg.crtc_id = picked->crtc_id;
  scene_cfg.connector_id = picked->connector_id;
  scene_cfg.mode = picked->mode;
  scene_cfg.stream_capability = cap;
  auto scene_r = drm::scene::LayerScene::create(device, scene_cfg);
  if (!scene_r) {
    drm::println(stderr, "LayerScene::create: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto& scene = *scene_r;

  auto bg_source_r = drm::scene::DumbBufferSource::create(device, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  // Held below so the render loop can paint each frame even after
  // std::move into the scene. unique_ptr's move transfers ownership
  // but doesn't relocate the underlying object.
  drm::scene::DumbBufferSource* bg_raw = nullptr;
  if (!bg_source_r) {
    drm::println(stderr, "DumbBufferSource::create: {}", bg_source_r.error().message());
    return EXIT_FAILURE;
  }
  bg_raw = (*bg_source_r).get();
  drm::scene::LayerDesc bg_desc;
  bg_desc.source = std::move(*bg_source_r);
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.zpos = 1;
  if (auto r = scene->add_layer(std::move(bg_desc)); !r) {
    drm::println(stderr, "add_layer (background): {}", r.error().message());
    return EXIT_FAILURE;
  }

  // Stream layer — the EglStreamBuilder is the one-stop entry point.
  // The returned Result hands back the upcast source ready to add to
  // the scene plus the EGL handles the producer-side render loop
  // needs.
  drm::scene::EglStreamBuilder::Request bld_req;
  bld_req.capability = cap;
  bld_req.device = &device;
  bld_req.format =
      drm::scene::SourceFormat{DRM_FORMAT_ARGB8888, 0, k_stream_width, k_stream_height};
  auto bld_r = drm::scene::EglStreamBuilder::build(bld_req);
  if (!bld_r) {
    drm::println(stderr, "EglStreamBuilder::build: {}", bld_r.error().message());
    return EXIT_FAILURE;
  }
  auto& bld = *bld_r;
  EGLDisplay egl_display = bld.display;
  EGLContext egl_context = bld.context;
  // Non-owning pointer to the source so we can fetch the producer
  // surface after the scene's first commit creates it (NVIDIA defers
  // producer-side attachment until a consumer is bound).
  drm::scene::EglStreamSource* stream_source = bld.source_ptr;

  drm::scene::LayerDesc stream_desc;
  stream_desc.source = std::move(bld.source);
  stream_desc.display.src_rect = drm::scene::Rect{0, 0, k_stream_width, k_stream_height};
  // Center the stream on the screen.
  const auto sx = static_cast<std::int32_t>((fb_w - k_stream_width) / 2);
  const auto sy = static_cast<std::int32_t>((fb_h - k_stream_height) / 2);
  stream_desc.display.dst_rect = drm::scene::Rect{sx, sy, k_stream_width, k_stream_height};
  stream_desc.display.zpos = 2;
  if (auto r = scene->add_layer(std::move(stream_desc)); !r) {
    drm::println(stderr, "add_layer (stream): {}", r.error().message());
    return EXIT_FAILURE;
  }

  // First scene commit: modeset + background. The stream plane is
  // intentionally left untouched by the scene's atomic commit;
  // NVIDIA's auto-acquire path on the consumer-bound stream drives
  // the plane state on each producer eglSwapBuffers.
  if (auto r = scene->commit(); !r) {
    drm::println(stderr, "first commit: {}", r.error().message());
    return EXIT_FAILURE;
  }
  drm::println("stream_demo: first commit OK; stream layer pinned to plane {}",
               stream_source->bound_plane().value_or(0));
  EGLSurface egl_surface = stream_source->producer_surface();
  if (egl_surface == EGL_NO_SURFACE) {
    drm::println(stderr,
                 "stream_demo: first commit succeeded but producer surface still null "
                 "(bind_to_plane likely failed); see scene logs above");
    return EXIT_FAILURE;
  }

  // Visible-scanout caveat for desktop NVIDIA: the EGL Streams chain
  // up to and including consumer attachment is wired here, but on
  // desktop NVIDIA 535+ the consumer-to-KMS handoff requires the
  // EGL_NV_output_drm_atomic extension (only present on Tegra
  // drivers; not exported on the desktop stack). Without it, the
  // producer's eglSwapBuffers wedges on the first call — the
  // consumer's plane state isn't reachable via mainline KMS
  // properties (CRTC_ID-without-FB_ID returns EINVAL). The render
  // loop is skipped here; the demo holds the modeset state for
  // the requested duration so a reviewer can confirm the scene's
  // first commit took. End-to-end visible scanout works on Tegra
  // (where EGL_NV_output_drm_atomic provides the missing
  // first-frame handoff via EGL_DRM_ATOMIC_REQUEST_NV).
  const char* display_exts = eglQueryString(egl_display, EGL_EXTENSIONS);
  const bool has_nv_atomic =
      (display_exts != nullptr) &&
      (std::string_view{display_exts}.find("EGL_NV_output_drm_atomic") != std::string_view::npos);
  if (!has_nv_atomic) {
    drm::println(
        "stream_demo: EGL_NV_output_drm_atomic not exported on this driver — "
        "running the CPU-rendered background animation instead (no stream "
        "frames will be visible; see docs/streams.md for the known "
        "desktop-NVIDIA scanout gap). Duration {}s.",
        args.seconds);
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    const auto deadline = t0 + std::chrono::seconds(args.seconds);
    std::uint64_t bg_frames = 0;
    while (clk::now() < deadline) {
      const float t = std::chrono::duration<float>(clk::now() - t0).count();
      const auto rch = static_cast<std::uint8_t>(127.0F + (127.0F * std::sin(t)));
      const auto gch = static_cast<std::uint8_t>(127.0F + (127.0F * std::sin((t * 1.3F) + 2.0F)));
      const auto bch = static_cast<std::uint8_t>(127.0F + (127.0F * std::sin((t * 1.7F) + 4.0F)));
      const std::uint32_t color = (0xFFU << 24U) | (static_cast<std::uint32_t>(rch) << 16U) |
                                  (static_cast<std::uint32_t>(gch) << 8U) |
                                  static_cast<std::uint32_t>(bch);
      if (auto m = bg_raw->map(drm::MapAccess::Write); m.has_value()) {
        auto& mapping = *m;
        if (auto* base = mapping.pixels().data(); base != nullptr) {
          const auto stride = mapping.stride();
          for (std::uint32_t y = 0; y < fb_h; ++y) {
            auto* px =
                reinterpret_cast<std::uint32_t*>(base + (static_cast<std::size_t>(y) * stride));
            for (std::uint32_t x = 0; x < fb_w; ++x) {
              px[x] = color;
            }
          }
        }
      }
      if (auto r = scene->commit(); !r) {
        drm::println(stderr, "commit (background frame): {}", r.error().message());
        break;
      }
      ++bg_frames;
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    drm::println("stream_demo: {} background frames over {}s", bg_frames, args.seconds);
    scene.reset();
    eglTerminate(egl_display);
    return EXIT_SUCCESS;
  }

  // Tegra / future driver path: producer surface drives scanout.
  if (eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context) != EGL_TRUE) {
    drm::println(stderr, "eglMakeCurrent failed: 0x{:x}", static_cast<unsigned>(eglGetError()));
    return EXIT_FAILURE;
  }
  eglSwapInterval(egl_display, 0);

  // Run the empirical mixing probe once. On NVIDIA the verdict tells
  // us whether the driver permits FB-ID layers (our background, in
  // this demo) to share a CRTC with the stream consumer plane. We've
  // already committed both successfully, so the upgrade to Mixed is
  // expected here; the probe formalizes that.
  if (auto r = scene->probe_stream_mixing(); r) {
    drm::println("stream_demo: probe_stream_mixing -> {}", drm::scene::to_string(*r));
  } else {
    drm::println(stderr, "stream_demo: probe_stream_mixing failed: {}", r.error().message());
  }

  // Render loop. Animate the clear color through a hue cycle. The
  // stream consumer pulls frames at its own pace; the producer just
  // keeps pushing.
  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  const auto deadline = t0 + std::chrono::seconds(args.seconds);
  std::uint64_t frames = 0;
  while (clk::now() < deadline) {
    const float t = std::chrono::duration<float>(clk::now() - t0).count();
    const float r = 0.5F + (0.5F * std::sin(t * 1.0F));
    const float g = 0.5F + (0.5F * std::sin((t * 1.3F) + 2.0F));
    const float b = 0.5F + (0.5F * std::sin((t * 1.7F) + 4.0F));
    glClearColor(r, g, b, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    if (eglSwapBuffers(egl_display, egl_surface) != EGL_TRUE) {
      drm::println(stderr, "eglSwapBuffers: 0x{:x}", static_cast<unsigned>(eglGetError()));
      break;
    }
    ++frames;
    // ~60 Hz pacing — the stream consumer scans out at vsync, so
    // pushing faster than that just discards intermediate frames.
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  drm::println("stream_demo: {} frames produced over {}s", frames, args.seconds);

  // Tear-down. Drop the context-current binding so eglDestroyContext
  // / eglTerminate don't trip the "context in use" path. Source
  // destruction is owned by the scene (it holds the unique_ptr we
  // moved into add_layer); scene's destructor will drive
  // unbind_from_plane and the source's own teardown.
  eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (bld.context_created_by_builder) {
    eglDestroyContext(egl_display, egl_context);
  }
  // Scene destruction tears down stream sources before we terminate
  // the display. Reset the unique_ptr inside the expected<> by
  // moving from `scene`; the expected then holds an empty pointer.
  scene.reset();
  eglTerminate(egl_display);
  return EXIT_SUCCESS;
} catch (...) {
  return EXIT_FAILURE;
}

#endif  // DRM_CXX_HAS_EGL_STREAMS
