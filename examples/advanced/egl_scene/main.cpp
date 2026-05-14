// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// egl_scene — minimal end-to-end demo for `drm::scene::GbmSurfaceSource`
// fronted by an EGL / GLES 3 renderer.
//
// What it does:
//
//   1. Opens a DRM card and picks the first connected output (via the
//      shared `open_and_pick_output` helper).
//   2. Builds a `LayerScene` against the picked CRTC.
//   3. Adds a dumb-buffer background layer so PRIMARY has an FB
//      through modeset (the GBM-surface layer lands on an OVERLAY).
//   4. Calls `LayerScene::candidate_modifiers(DRM_FORMAT_ARGB8888)`
//      to learn the modifier set the allocator will accept, queries
//      the matching set from EGL via `eglQueryDmaBufModifiersEXT`,
//      intersects, and picks one — preferring driver-preference
//      order from KMS.
//   5. Constructs a `GbmSurfaceSource` at the output's resolution
//      with the picked modifier.
//   6. Creates an EGL display over `GbmSurfaceSource::native_device()`,
//      a GLES 3 context, and a window surface over
//      `GbmSurfaceSource::native_surface()`.
//   7. Renders a hue-cycling clear-color into the surface each frame,
//      `eglSwapBuffers`, then commits the scene. After the first
//      swap the source's `acquire()` returns the locked front buffer's
//      FB ID and the scene scans it out.
//
// CLI:
//
//   egl_scene [--seconds N] [/dev/dri/cardN]
//
// Build gate: requires libEGL + glesv2 at link time. The example
// is omitted from the build when either pkg-config dependency is
// missing.

#include "common/open_output.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/gbm/device.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/gbm_surface_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <drm_fourcc.h>
#include <gbm.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

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

// Negotiate a single DRM format modifier that satisfies both the
// allocator (LayerScene::candidate_modifiers) and the EGL driver
// (eglQueryDmaBufModifiersEXT). Returns DRM_FORMAT_MOD_INVALID when
// either side has nothing to offer or the EGL extension is missing —
// the caller then constructs the GbmSurfaceSource with INVALID,
// which falls back to bare gbm_surface_create and lets the driver
// choose.
[[nodiscard]] std::uint64_t pick_modifier(EGLDisplay egl_display,
                                          const std::vector<std::uint64_t>& scene_mods,
                                          std::uint32_t drm_format) noexcept {
  if (scene_mods.empty()) {
    return DRM_FORMAT_MOD_INVALID;
  }

  using PfnQueryDmaBufModifiers =
      EGLBoolean (*)(EGLDisplay, EGLint, EGLint, EGLuint64KHR*, EGLBoolean*, EGLint*);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto query_modifiers = reinterpret_cast<PfnQueryDmaBufModifiers>(
      eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
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

  // Walk the scene's modifier list (driver-preference order from
  // IN_FORMATS); pick the first that also appears in the EGL set.
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
    case EGL_SUCCESS: return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
    case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
    case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
    case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
    case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
    case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
    case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
    case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
    default: return "(unknown EGL error)";
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  const auto args = parse_args(argc, argv);

  auto out = drm::examples::open_and_pick_output(argc, argv);
  if (!out) {
    return EXIT_FAILURE;
  }
  auto& device = out->device;
  const std::uint32_t fb_w = out->mode.hdisplay;
  const std::uint32_t fb_h = out->mode.vdisplay;
  drm::println("egl_scene: crtc={} connector={} mode={}x{}@{}Hz", out->crtc_id, out->connector_id,
               fb_w, fb_h, out->mode.vrefresh);

  // Scene first — its candidate_modifiers() query drives the rest.
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

  // Background dumb-buffer layer. PRIMARY needs a non-null FB across
  // the modeset; the scene's allocator may place the GBM-surface
  // layer above it on an OVERLAY plane. zpos=2 mirrors the
  // amdgpu primary-pin / multi-plane convention used elsewhere.
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

  // Modifier negotiation. The scene's list is the allocator's view;
  // EGL's list is the renderer's view. We need a value that lives in
  // both — or fall back to INVALID and let the driver pick.
  const auto scene_mods = scene->candidate_modifiers(DRM_FORMAT_ARGB8888);
  drm::println("egl_scene: scene accepts {} modifier(s) for ARGB8888", scene_mods.size());

  // The source needs a modifier picked BEFORE it creates its
  // gbm_surface. EGL needs a display BEFORE we can query EGL
  // modifiers. We dance: create a probe gbm_device + EGL display to
  // query modifiers, pick one, then drop the probe and construct the
  // source with the chosen modifier — the source's internal
  // gbm_device + surface is what we actually render against.
  auto probe_gbm = drm::gbm::GbmDevice::create(device.fd());
  if (!probe_gbm) {
    drm::println(stderr, "gbm device for probe: {}", probe_gbm.error().message());
    return EXIT_FAILURE;
  }

  using PfnGetPlatformDisplay = EGLDisplay (*)(EGLenum, void*, const EGLAttrib*);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto get_platform_display = reinterpret_cast<PfnGetPlatformDisplay>(
      eglGetProcAddress("eglGetPlatformDisplay"));
  if (get_platform_display == nullptr) {
    drm::println(stderr, "egl_scene: eglGetPlatformDisplay not exported by libEGL");
    return EXIT_FAILURE;
  }

  EGLDisplay probe_display = get_platform_display(EGL_PLATFORM_GBM_KHR, probe_gbm->raw(), nullptr);
  if (probe_display == EGL_NO_DISPLAY) {
    drm::println(stderr, "egl_scene: eglGetPlatformDisplay (probe) failed: {}",
                 gl_strerror(eglGetError()));
    return EXIT_FAILURE;
  }
  if (eglInitialize(probe_display, nullptr, nullptr) != EGL_TRUE) {
    drm::println(stderr, "egl_scene: eglInitialize (probe) failed: {}", gl_strerror(eglGetError()));
    return EXIT_FAILURE;
  }
  const std::uint64_t modifier = pick_modifier(probe_display, scene_mods, DRM_FORMAT_ARGB8888);
  eglTerminate(probe_display);

  if (modifier == DRM_FORMAT_MOD_INVALID) {
    drm::println("egl_scene: no shared (scene, EGL) modifier — falling back to INVALID");
  } else {
    drm::println("egl_scene: negotiated modifier 0x{:016x}", modifier);
  }

  // Now build the real source + its EGL stack.
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

  EGLDisplay display = get_platform_display(EGL_PLATFORM_GBM_KHR, src_ptr->native_device(), nullptr);
  if (display == EGL_NO_DISPLAY) {
    drm::println(stderr, "egl_scene: eglGetPlatformDisplay failed: {}", gl_strerror(eglGetError()));
    return EXIT_FAILURE;
  }
  if (eglInitialize(display, nullptr, nullptr) != EGL_TRUE) {
    drm::println(stderr, "egl_scene: eglInitialize failed: {}", gl_strerror(eglGetError()));
    return EXIT_FAILURE;
  }
  if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
    drm::println(stderr, "egl_scene: eglBindAPI: {}", gl_strerror(eglGetError()));
    eglTerminate(display);
    return EXIT_FAILURE;
  }

  // ARGB8888 EGLConfig matching the GBM surface format.
  const EGLint cfg_attrs[] = {
      EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE,        8,
      EGL_GREEN_SIZE,      8,
      EGL_BLUE_SIZE,       8,
      EGL_ALPHA_SIZE,      8,
      EGL_NONE,
  };
  EGLConfig egl_config = nullptr;
  EGLint num_configs = 0;
  // Mesa's eglChooseConfig returns configs sorted by "best first"
  // without filtering on EGL_NATIVE_VISUAL_ID — manually walk and
  // pick one whose native visual matches our DRM format. Common
  // mistake: skipping this and ending up with a config that EGL will
  // accept but whose framebuffer layout doesn't match the gbm_surface.
  if (eglChooseConfig(display, cfg_attrs, nullptr, 0, &num_configs) != EGL_TRUE || num_configs == 0) {
    drm::println(stderr, "egl_scene: no matching EGLConfig");
    eglTerminate(display);
    return EXIT_FAILURE;
  }
  std::vector<EGLConfig> configs(static_cast<std::size_t>(num_configs));
  eglChooseConfig(display, cfg_attrs, configs.data(), num_configs, &num_configs);
  for (const auto cfg : configs) {
    EGLint visual_id = 0;
    if (eglGetConfigAttrib(display, cfg, EGL_NATIVE_VISUAL_ID, &visual_id) == EGL_TRUE &&
        static_cast<std::uint32_t>(visual_id) == DRM_FORMAT_ARGB8888) {
      egl_config = cfg;
      break;
    }
  }
  if (egl_config == nullptr) {
    drm::println(stderr, "egl_scene: no EGLConfig with NATIVE_VISUAL_ID == ARGB8888");
    eglTerminate(display);
    return EXIT_FAILURE;
  }

  const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext context = eglCreateContext(display, egl_config, EGL_NO_CONTEXT, ctx_attrs);
  if (context == EGL_NO_CONTEXT) {
    drm::println(stderr, "egl_scene: eglCreateContext: {}", gl_strerror(eglGetError()));
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
    // Pre-EGL-1.5 fallback. Mesa supports both entry points but some
    // older runtimes only ship the EXT.
    egl_surface = eglCreateWindowSurface(display, egl_config,
                                          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                                          reinterpret_cast<EGLNativeWindowType>(
                                              src_ptr->native_surface()),
                                          nullptr);
  }
  if (egl_surface == EGL_NO_SURFACE) {
    drm::println(stderr, "egl_scene: eglCreate{Platform,}WindowSurface: {}",
                 gl_strerror(eglGetError()));
    eglDestroyContext(display, context);
    eglTerminate(display);
    return EXIT_FAILURE;
  }

  if (eglMakeCurrent(display, egl_surface, egl_surface, context) != EGL_TRUE) {
    drm::println(stderr, "egl_scene: eglMakeCurrent: {}", gl_strerror(eglGetError()));
    eglDestroySurface(display, egl_surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    return EXIT_FAILURE;
  }
  eglSwapInterval(display, 0);

  // Render one frame upfront so the source has a front buffer to lock
  // before the scene's first commit attempts to acquire(). Mesa won't
  // populate the gbm_surface dispatch table until the producer binding
  // has actually rendered + swapped; see the source's file comment.
  glViewport(0, 0, static_cast<GLsizei>(fb_w), static_cast<GLsizei>(fb_h));
  glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  if (eglSwapBuffers(display, egl_surface) != EGL_TRUE) {
    drm::println(stderr, "egl_scene: initial eglSwapBuffers: {}", gl_strerror(eglGetError()));
    return EXIT_FAILURE;
  }

  // Add the GBM-surface layer AFTER the first swap — by which point
  // the source's first acquire() will succeed.
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

  // Render loop. Cycle the clear color through a hue ramp and pace at
  // ~60 Hz; each commit() blocks until the previous flip retires, so
  // we don't busy-spin.
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
    if (eglSwapBuffers(display, egl_surface) != EGL_TRUE) {
      drm::println(stderr, "eglSwapBuffers: {}", gl_strerror(eglGetError()));
      break;
    }
    if (auto cr = scene->commit(); !cr) {
      drm::println(stderr, "commit: {}", cr.error().message());
      break;
    }
    ++frames;
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  drm::println("egl_scene: {} frames in {}s", frames, args.seconds);

  // Tear-down. Order matters: drop the context binding before
  // destroying the surface (the source's gbm_surface is destroyed by
  // the scene's destructor when it owns the source).
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(display, egl_surface);
  eglDestroyContext(display, context);
  scene.reset();
  eglTerminate(display);
  return EXIT_SUCCESS;
}