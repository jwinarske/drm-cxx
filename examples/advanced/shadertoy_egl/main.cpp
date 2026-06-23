// SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// shadertoy_egl — run any Shadertoy "Image" shader directly on DRM/KMS via
// EGL/GBM, using the platform-agnostic shadertoy-cxx renderer.
//
// This is the DRM/KMS counterpart of the Wayland shadertoy host: the exact same
// shadertoy::GlRenderer drives both. Here the "window" is a scanned-out
// gbm_surface fronted by a GLES 3 EGL context, and mouse/touch input is read
// straight from libinput (via drm::input::Seat) and mapped into iMouse.
//
// What it does (mirrors egl_scene's GBM-surface scanout path):
//
//   1. Open a DRM card + pick the first connected output.
//   2. Build a LayerScene; add a dumb-buffer background so PRIMARY has an FB
//      across modeset.
//   3. Negotiate a DRM format modifier shared by the scene allocator and EGL.
//   4. Create a GbmSurfaceSource at the output resolution.
//   5. Bring up an EGL display over the source's gbm_device, a *GLES 3* context,
//      and a window surface over the source's gbm_surface.
//   6. Compile the Shadertoy shader (shadertoy-cxx) and render it each frame,
//      eglSwapBuffers, then commit the scene to scan it out.
//   7. libinput pointer + touch events feed iMouse.
//
// CLI:
//
//   shadertoy_egl [--cycle N] [--seconds N] [shader ...] [/dev/dri/cardN]
//
//   shader        One or more Shadertoy exports (.json, multi-pass) or bare
//                 Image .frag/.glsl. With none, the installed bundled set is
//                 used. Advance with SPACE/RIGHT (LEFT = prev) or a touch tap.
//   --cycle N     Auto-advance every N seconds (default: off).
//   --seconds N   Total runtime; exit after N seconds (default: until SIGINT).
//
// Run from a free VT (it holds DRM master and modesets); sudo on a bare TTY,
// or membership in the seat group on a seatd-managed system.
//
// Build gate: libEGL + glesv2 + shadertoy-cxx. The library never links these.

#include "common/open_output.hpp"

#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/gbm/device.hpp>
#include <drm-cxx/input/seat.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/gbm_surface_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <gbm.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglplatform.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <linux/input-event-codes.h>  // BTN_LEFT
#include <optional>
#include <shadertoy/gl_renderer.hpp>
#include <shadertoy/inputs.hpp>
#include <shadertoy/playlist.hpp>
#include <shadertoy/program.hpp>
#include <string>
#include <string_view>
#include <sys/poll.h>  // pollfd, POLLIN, poll
#include <utility>
#include <variant>
#include <vector>

namespace {

// ── Quit on SIGINT/SIGTERM ─────────────────────────────────────
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_stop = 0;
extern "C" void on_signal(int /*sig*/) {
  g_stop = 1;
}

struct Args {
  int seconds{0};        // total runtime; 0 = run until SIGINT
  int cycle_seconds{0};  // per-shader auto-advance; 0 = off
  std::vector<std::string> shaders;
};

// Recognise a Shadertoy shader argument (.json export or bare .frag/.glsl).
[[nodiscard]] bool is_shader_arg(std::string_view a) {
  auto ends = [&](std::string_view s) {
    return a.size() >= s.size() && a.compare(a.size() - s.size(), s.size(), s) == 0;
  };
  return ends(".json") || ends(".frag") || ends(".glsl");
}

[[nodiscard]] Args parse_args(int& argc, char**& argv) {
  Args a;
  int write = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--seconds" && (i + 1) < argc) {
      a.seconds = std::atoi(argv[++i]);
    } else if (arg == "--cycle" && (i + 1) < argc) {
      a.cycle_seconds = std::atoi(argv[++i]);
    } else if (is_shader_arg(arg)) {
      a.shaders.emplace_back(argv[i]);
    } else {
      argv[write++] = argv[i];  // leave the device path for open_and_pick_output
    }
  }
  argc = write;
  return a;
}

// Mouse/touch state in framebuffer pixels, top-left origin (y down). Converted
// to Shadertoy's bottom-left origin only when filling iMouse.
struct MouseState {
  double x{0.0};
  double y{0.0};
  double click_x{0.0};
  double click_y{0.0};
  bool down{false};
  int32_t touch_slot{-1};   // active touch slot, -1 = none
  double touch_moved{0.0};  // accumulated touch travel since down (for tap)
};

// Negotiate a single DRM format modifier acceptable to both the scene allocator
// and the EGL driver, or DRM_FORMAT_MOD_INVALID to let the driver choose.
[[nodiscard]] std::uint64_t pick_modifier(EGLDisplay egl_display,
                                          const std::vector<std::uint64_t>& scene_mods,
                                          std::uint32_t drm_format) noexcept {
  if (scene_mods.empty()) {
    return DRM_FORMAT_MOD_INVALID;
  }
  using PfnQueryDmaBufModifiers =
      EGLBoolean (*)(EGLDisplay, EGLint, EGLint, EGLuint64KHR*, EGLBoolean*, EGLint*);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto query_modifiers =
      reinterpret_cast<PfnQueryDmaBufModifiers>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
  if (query_modifiers == nullptr) {
    return DRM_FORMAT_MOD_INVALID;
  }
  EGLint count = 0;
  if (query_modifiers(egl_display, static_cast<EGLint>(drm_format), 0, nullptr, nullptr, &count) !=
          EGL_TRUE ||
      count <= 0) {
    return DRM_FORMAT_MOD_INVALID;
  }
  std::vector<EGLuint64KHR> egl_mods(static_cast<std::size_t>(count));
  std::vector<EGLBoolean> external_only(static_cast<std::size_t>(count));
  if (query_modifiers(egl_display, static_cast<EGLint>(drm_format), count, egl_mods.data(),
                      external_only.data(), &count) != EGL_TRUE) {
    return DRM_FORMAT_MOD_INVALID;
  }
  for (const auto m : scene_mods) {
    if (m == DRM_FORMAT_MOD_INVALID) {
      continue;
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
      if (egl_mods[i] == m) {
        return m;
      }
    }
  }
  return DRM_FORMAT_MOD_INVALID;
}

[[nodiscard]] const char* gl_strerror(EGLint err) noexcept {
  switch (err) {
    case EGL_SUCCESS:
      return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED:
      return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS:
      return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC:
      return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE:
      return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONFIG:
      return "EGL_BAD_CONFIG";
    case EGL_BAD_CONTEXT:
      return "EGL_BAD_CONTEXT";
    case EGL_BAD_DISPLAY:
      return "EGL_BAD_DISPLAY";
    case EGL_BAD_MATCH:
      return "EGL_BAD_MATCH";
    case EGL_BAD_NATIVE_PIXMAP:
      return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW:
      return "EGL_BAD_NATIVE_WINDOW";
    case EGL_BAD_PARAMETER:
      return "EGL_BAD_PARAMETER";
    case EGL_BAD_SURFACE:
      return "EGL_BAD_SURFACE";
    case EGL_CONTEXT_LOST:
      return "EGL_CONTEXT_LOST";
    default:
      return "(unknown EGL error)";
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  const auto args = parse_args(argc, argv);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  auto out = drm::examples::open_and_pick_output(argc, argv);
  if (!out) {
    return EXIT_FAILURE;
  }
  auto& device = out->device;
  const std::uint32_t fb_w = out->mode.hdisplay;
  const std::uint32_t fb_h = out->mode.vdisplay;
  drm::println("shadertoy_egl: crtc={} connector={} mode={}x{}@{}Hz", out->crtc_id,
               out->connector_id, fb_w, fb_h, out->mode.vrefresh);

  // ── Scene + background layer ───────────────────────────────────
  drm::scene::LayerScene::Config scene_cfg;
  scene_cfg.crtc_id = out->crtc_id;
  scene_cfg.connector_id = out->connector_id;
  scene_cfg.mode = out->mode;
  auto scene_r = drm::scene::LayerScene::create(device, scene_cfg);
  if (!scene_r) {
    drm::println(stderr, "LayerScene::create: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto& scene = *scene_r;

  auto bg_source_r = drm::scene::DumbBufferSource::create(device, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  if (!bg_source_r) {
    drm::println(stderr, "DumbBufferSource::create: {}", bg_source_r.error().message());
    return EXIT_FAILURE;
  }
  drm::scene::LayerDesc bg_desc;
  bg_desc.source = std::move(*bg_source_r);
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.zpos = 2;
  if (auto r = scene->add_layer(std::move(bg_desc)); !r) {
    drm::println(stderr, "add_layer (background): {}", r.error().message());
    return EXIT_FAILURE;
  }

  // ── Modifier negotiation (probe gbm_device + EGL display) ──────
  const auto scene_mods = scene->candidate_modifiers(DRM_FORMAT_ARGB8888);
  auto probe_gbm = drm::gbm::GbmDevice::create(device.fd());
  if (!probe_gbm) {
    drm::println(stderr, "gbm device for probe: {}", probe_gbm.error().message());
    return EXIT_FAILURE;
  }
  using PfnGetPlatformDisplay = EGLDisplay (*)(EGLenum, void*, const EGLAttrib*);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto get_platform_display =
      reinterpret_cast<PfnGetPlatformDisplay>(eglGetProcAddress("eglGetPlatformDisplay"));
  if (get_platform_display == nullptr) {
    drm::println(stderr, "shadertoy_egl: eglGetPlatformDisplay not exported by libEGL");
    return EXIT_FAILURE;
  }
  EGLDisplay probe_display = get_platform_display(EGL_PLATFORM_GBM_KHR, probe_gbm->raw(), nullptr);
  std::uint64_t modifier = DRM_FORMAT_MOD_INVALID;
  if (probe_display != EGL_NO_DISPLAY &&
      eglInitialize(probe_display, nullptr, nullptr) == EGL_TRUE) {
    modifier = pick_modifier(probe_display, scene_mods, DRM_FORMAT_ARGB8888);
    eglTerminate(probe_display);
  }

  // ── Real GbmSurfaceSource + EGL stack ──────────────────────────
  drm::scene::GbmSurfaceConfig src_cfg;
  src_cfg.width = fb_w;
  src_cfg.height = fb_h;
  src_cfg.drm_format = DRM_FORMAT_ARGB8888;
  src_cfg.modifier = modifier;
  src_cfg.usage = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
  auto src_r = drm::scene::GbmSurfaceSource::create(device, src_cfg);
  if (!src_r) {
    drm::println(stderr, "GbmSurfaceSource::create: {}", src_r.error().message());
    return EXIT_FAILURE;
  }
  auto* src_ptr = (*src_r).get();

  EGLDisplay display =
      get_platform_display(EGL_PLATFORM_GBM_KHR, src_ptr->native_device(), nullptr);
  if (display == EGL_NO_DISPLAY || eglInitialize(display, nullptr, nullptr) != EGL_TRUE) {
    drm::println(stderr, "shadertoy_egl: eglInitialize failed: {}", gl_strerror(eglGetError()));
    return EXIT_FAILURE;
  }
  if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
    drm::println(stderr, "shadertoy_egl: eglBindAPI: {}", gl_strerror(eglGetError()));
    eglTerminate(display);
    return EXIT_FAILURE;
  }

  // GLES 3 config + context (shadertoy::GlRenderer needs ES 3: VAOs, #version
  // 300 es, gl_VertexID). The native visual must match the gbm_surface format.
  const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE,
                              EGL_WINDOW_BIT,
                              EGL_RENDERABLE_TYPE,
                              EGL_OPENGL_ES3_BIT,
                              EGL_RED_SIZE,
                              8,
                              EGL_GREEN_SIZE,
                              8,
                              EGL_BLUE_SIZE,
                              8,
                              EGL_ALPHA_SIZE,
                              8,
                              EGL_NONE};
  EGLint num_configs = 0;
  if (eglChooseConfig(display, cfg_attrs, nullptr, 0, &num_configs) != EGL_TRUE ||
      num_configs == 0) {
    drm::println(stderr, "shadertoy_egl: no matching EGLConfig");
    eglTerminate(display);
    return EXIT_FAILURE;
  }
  std::vector<EGLConfig> configs(static_cast<std::size_t>(num_configs));
  eglChooseConfig(display, cfg_attrs, configs.data(), num_configs, &num_configs);
  EGLConfig egl_config = nullptr;
  for (auto* const cfg : configs) {
    EGLint visual_id = 0;
    if (eglGetConfigAttrib(display, cfg, EGL_NATIVE_VISUAL_ID, &visual_id) == EGL_TRUE &&
        static_cast<std::uint32_t>(visual_id) == DRM_FORMAT_ARGB8888) {
      egl_config = cfg;
      break;
    }
  }
  if (egl_config == nullptr) {
    drm::println(stderr, "shadertoy_egl: no EGLConfig with NATIVE_VISUAL_ID == ARGB8888");
    eglTerminate(display);
    return EXIT_FAILURE;
  }

  const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  EGLContext context = eglCreateContext(display, egl_config, EGL_NO_CONTEXT, ctx_attrs);
  if (context == EGL_NO_CONTEXT) {
    drm::println(stderr, "shadertoy_egl: eglCreateContext (ES3): {}", gl_strerror(eglGetError()));
    eglTerminate(display);
    return EXIT_FAILURE;
  }

  using PfnCreatePlatformWindowSurface =
      EGLSurface (*)(EGLDisplay, EGLConfig, void*, const EGLAttrib*);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto create_window_surface = reinterpret_cast<PfnCreatePlatformWindowSurface>(
      eglGetProcAddress("eglCreatePlatformWindowSurface"));
  EGLSurface egl_surface = EGL_NO_SURFACE;
  if (create_window_surface != nullptr) {
    egl_surface = create_window_surface(display, egl_config, src_ptr->native_surface(), nullptr);
  }
  if (egl_surface == EGL_NO_SURFACE) {
    egl_surface = eglCreateWindowSurface(
        display, egl_config,
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        reinterpret_cast<EGLNativeWindowType>(src_ptr->native_surface()), nullptr);
  }
  if (egl_surface == EGL_NO_SURFACE) {
    drm::println(stderr, "shadertoy_egl: eglCreateWindowSurface: {}", gl_strerror(eglGetError()));
    eglDestroyContext(display, context);
    eglTerminate(display);
    return EXIT_FAILURE;
  }
  if (eglMakeCurrent(display, egl_surface, egl_surface, context) != EGL_TRUE) {
    drm::println(stderr, "shadertoy_egl: eglMakeCurrent: {}", gl_strerror(eglGetError()));
    eglDestroySurface(display, egl_surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    return EXIT_FAILURE;
  }
  eglSwapInterval(display, 0);  // the scene's commit() paces to vblank

  // ── Build the shader playlist ──────────────────────────────────
  // CLI shaders (.json multi-pass / .frag single), else the installed bundled
  // set, else the built-in default.
  shadertoy::Playlist playlist;
  {
    std::string err;
    for (const std::string& s : args.shaders) {
      if (!playlist.AddFile(s, &err)) {
        drm::println(stderr, "shadertoy_egl: {}: {}", s, err);
      }
    }
    if (playlist.empty()) {
      playlist.AddDirectory(shadertoy::DefaultShaderDir());
    }
    if (playlist.empty()) {
      playlist.Add(shadertoy::MakeSinglePass(shadertoy::DefaultImageShader(), "default"));
    }
  }

  shadertoy::GlRenderer renderer;
  if (!renderer.SetProgram(playlist.current())) {
    drm::println(stderr, "shadertoy_egl: shader failed to load");
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, egl_surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    return EXIT_FAILURE;
  }
  drm::println(
      "shadertoy_egl: {} shader(s) [{}]; cycle={}s "
      "(SPACE/RIGHT next, LEFT prev, tap to advance)",
      playlist.size(), playlist.current().name, args.cycle_seconds);

  // Set to +1/-1 by input (key or touch tap) to step the playlist; applied in
  // the render loop where the GL context is current.
  int pending_step = 0;

  // ── libinput mouse/touch → iMouse ──────────────────────────────
  MouseState mouse;
  std::optional<drm::input::Seat> input_seat;
  {
    drm::input::InputDeviceOpener opener;
    if (out->seat) {
      opener = out->seat->input_opener();
    }
    auto seat_r =
        opener.empty() ? drm::input::Seat::open({}) : drm::input::Seat::open({}, std::move(opener));
    if (seat_r) {
      input_seat = std::move(*seat_r);
    } else {
      drm::println(stderr, "shadertoy_egl: input unavailable ({}); running without mouse/touch",
                   seat_r.error().message());
    }
  }
  if (input_seat) {
    input_seat->set_event_handler([&](const drm::input::InputEvent& ev) {
      if (const auto* ke = std::get_if<drm::input::KeyboardEvent>(&ev)) {
        if (ke->pressed) {
          switch (ke->key) {
            case KEY_ESC:
              g_stop = 1;
              break;
            case KEY_SPACE:
            case KEY_RIGHT:
              pending_step = 1;
              break;
            case KEY_LEFT:
              pending_step = -1;
              break;
            default:
              break;
          }
        }
      } else if (const auto* pe = std::get_if<drm::input::PointerEvent>(&ev)) {
        if (const auto* m = std::get_if<drm::input::PointerMotionEvent>(pe)) {
          mouse.x = std::clamp(mouse.x + m->dx, 0.0, static_cast<double>(fb_w));
          mouse.y = std::clamp(mouse.y + m->dy, 0.0, static_cast<double>(fb_h));
        } else if (const auto* b = std::get_if<drm::input::PointerButtonEvent>(pe)) {
          if (b->button == BTN_LEFT) {
            mouse.down = b->pressed;
            if (b->pressed) {
              mouse.click_x = mouse.x;
              mouse.click_y = mouse.y;
            }
          }
        }
      } else if (const auto* te = std::get_if<drm::input::TouchEvent>(&ev)) {
        using T = drm::input::TouchEvent::Type;
        // Touch x/y are normalized [0,1) in device orientation.
        const double px = te->x * static_cast<double>(fb_w);
        const double py = te->y * static_cast<double>(fb_h);
        if (te->type == T::Down && mouse.touch_slot < 0) {
          mouse.touch_slot = te->slot;
          mouse.x = px;
          mouse.y = py;
          mouse.click_x = px;
          mouse.click_y = py;
          mouse.touch_moved = 0.0;
          mouse.down = true;
        } else if (te->type == T::Motion && te->slot == mouse.touch_slot) {
          mouse.touch_moved += std::abs(px - mouse.x) + std::abs(py - mouse.y);
          mouse.x = px;
          mouse.y = py;
        } else if ((te->type == T::Up || te->type == T::Cancel) && te->slot == mouse.touch_slot) {
          // A tap (little travel) advances the playlist; a drag drove iMouse.
          if (te->type == T::Up && mouse.touch_moved < 16.0) {
            pending_step = 1;
          }
          mouse.touch_slot = -1;
          mouse.down = false;
        }
      }
    });
  }

  // ── Render one frame upfront so the source has a front buffer to lock ──
  shadertoy::ShaderInputs inputs;
  inputs.res_x = static_cast<float>(fb_w);
  inputs.res_y = static_cast<float>(fb_h);
  renderer.Render(inputs);
  if (eglSwapBuffers(display, egl_surface) != EGL_TRUE) {
    drm::println(stderr, "shadertoy_egl: initial eglSwapBuffers: {}", gl_strerror(eglGetError()));
    renderer.Destroy();
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, egl_surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    return EXIT_FAILURE;
  }

  // Add the GBM-surface layer AFTER the first swap, then first commit.
  drm::scene::LayerDesc fg_desc;
  fg_desc.source = std::move(*src_r);
  fg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  fg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  fg_desc.display.zpos = 3;
  if (auto r = scene->add_layer(std::move(fg_desc)); !r) {
    drm::println(stderr, "add_layer (gbm surface): {}", r.error().message());
    return EXIT_FAILURE;
  }
  if (auto r = scene->commit(); !r) {
    drm::println(stderr, "first commit: {}", r.error().message());
    return EXIT_FAILURE;
  }

  // ── Render loop ────────────────────────────────────────────────
  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  auto last = t0;
  auto program_start = t0;  // iTime is relative to the current shader
  const int input_fd = input_seat ? input_seat->fd() : -1;
  std::uint64_t frames = 0;

  while (g_stop == 0) {
    // Drain pending input (non-blocking); commit() below paces to vblank.
    if (input_fd >= 0) {
      pollfd pfd{input_fd, POLLIN, 0};
      if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN) != 0) {
        (void)input_seat->dispatch();
      }
    }

    // Auto-advance after cycle_seconds on the current shader, then apply any
    // pending step (cycle/key/tap) while the GL context is current.
    if (args.cycle_seconds > 0 && playlist.size() > 1) {
      const float since = std::chrono::duration<float>(clk::now() - program_start).count();
      if (since >= static_cast<float>(args.cycle_seconds)) {
        pending_step = 1;
      }
    }
    if (pending_step != 0 && playlist.size() > 1) {
      if (pending_step > 0) {
        playlist.next();
      } else {
        playlist.prev();
      }
      if (!renderer.SetProgram(playlist.current())) {
        drm::println(stderr, "shadertoy_egl: failed to switch shader");
      }
      program_start = clk::now();
      inputs.frame = 0;
    }
    pending_step = 0;

    const auto now = clk::now();
    const float t = std::chrono::duration<float>(now - program_start).count();
    const float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    if (args.seconds > 0 &&
        std::chrono::duration<float>(now - t0).count() >= static_cast<float>(args.seconds)) {
      break;
    }

    inputs.res_x = static_cast<float>(fb_w);
    inputs.res_y = static_cast<float>(fb_h);
    inputs.res_z = 1.0F;
    inputs.time = t;
    inputs.time_delta = dt;
    inputs.frame_rate = dt > 0.0F ? 1.0F / dt : 60.0F;
    // iMouse in Shadertoy's bottom-left origin.
    inputs.mouse_x = static_cast<float>(mouse.x);
    inputs.mouse_y = static_cast<float>(fb_h) - static_cast<float>(mouse.y);
    const auto cx = static_cast<float>(mouse.click_x);
    const float cy = static_cast<float>(fb_h) - static_cast<float>(mouse.click_y);
    inputs.mouse_z = mouse.down ? cx : -cx;
    inputs.mouse_w = mouse.down ? cy : -cy;

    const std::time_t tt = std::time(nullptr);
    std::tm tm_buf{};
    if (localtime_r(&tt, &tm_buf) != nullptr) {
      inputs.date_y = static_cast<float>(tm_buf.tm_year + 1900);
      inputs.date_m = static_cast<float>(tm_buf.tm_mon);
      inputs.date_d = static_cast<float>(tm_buf.tm_mday);
      inputs.date_s =
          static_cast<float>((tm_buf.tm_hour * 3600) + (tm_buf.tm_min * 60) + tm_buf.tm_sec);
    }

    renderer.Render(inputs);
    if (eglSwapBuffers(display, egl_surface) != EGL_TRUE) {
      drm::println(stderr, "eglSwapBuffers: {}", gl_strerror(eglGetError()));
      break;
    }
    if (auto cr = scene->commit(); !cr) {
      drm::println(stderr, "commit: {}", cr.error().message());
      break;
    }
    ++inputs.frame;
    ++frames;
  }
  drm::println("shadertoy_egl: {} frames", frames);

  // ── Teardown (GL objects first, while the context is still current) ──
  renderer.Destroy();
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(display, egl_surface);
  eglDestroyContext(display, context);
  scene.reset();
  eglTerminate(display);
  return EXIT_SUCCESS;
}
