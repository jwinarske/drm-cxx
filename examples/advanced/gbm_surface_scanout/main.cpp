// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// examples/advanced/gbm_surface_scanout/main.cpp
//
// The COMPOSITOR path to displayable hardware compression, the counterpart to
// egl_offload_scanout's OFFLOAD path. The difference is the producer:
//
//   offload: render into a standalone bo -> export dma-buf -> import & scan out.
//            Mesa will only ever hand a *standalone* buffer pipe-aligned DCC,
//            which the display controller cannot read, so amdgpu falls back to
//            plain tiling (dcc=0). See egl_offload_scanout's DRM_FMT_DUMP_EGL_MODS.
//
//   this:    render through a gbm_surface swapchain (eglSwapBuffers). Mesa owns
//            the buffer lifecycle, so it can pick the DISPLAYABLE-DCC modifier
//            (dcc=1 retile) and perform the retile blit on swap -- the same thing
//            a Wayland/X compositor does. The locked front buffer then scans out
//            *compressed*.
//
// We still drive the modifier negotiation through drm::fmt: the display plane's
// FormatTable is the candidate set we hand gbm_surface, classify() ranks it
// COMPRESSION-first, describe() names whatever Mesa actually chose, and the
// atomic TEST_ONLY is the final arbiter.
//
// Unified GPU+display device only (render_fd == disp_fd): the gbm_surface front
// buffer is already on the scanout node, so we export+reimport it through the
// same drm-cxx import path the offload demo uses.
//
// Run:  ./gbm_surface_scanout [/dev/dri/card0]

#include "../../common/kms_present.hpp"

#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/fmt/format_mod.hpp>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglplatform.h>
#include <GLES2/gl2.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fmt = drm::fmt;

namespace {

const char* class_name(fmt::BandwidthClass c) {
  switch (c) {
    case fmt::BandwidthClass::Compression:
      return "COMPRESSION";
    case fmt::BandwidthClass::Tiling:
      return "tiling";
    case fmt::BandwidthClass::Linear:
      return "linear";
  }
  return "?";
}

// Rank a modifier set COMPRESSION-first, tiling next, LINEAR guaranteed last --
// the order we *prefer*; gbm_surface picks the best it can actually produce.
std::vector<fmt::Modifier> rank(drm::span<const fmt::Modifier> in) {
  std::vector<fmt::Modifier> comp;
  std::vector<fmt::Modifier> tile;
  bool lin = false;
  for (fmt::Modifier const m : in) {
    switch (fmt::classify(m)) {
      case fmt::BandwidthClass::Compression:
        comp.push_back(m);
        break;
      case fmt::BandwidthClass::Tiling:
        tile.push_back(m);
        break;
      case fmt::BandwidthClass::Linear:
        lin = true;
        break;
    }
  }
  std::vector<fmt::Modifier> out;
  out.insert(out.end(), comp.begin(), comp.end());
  out.insert(out.end(), tile.begin(), tile.end());
  if (lin || out.empty()) {
    out.push_back(fmt::Modifier{DRM_FORMAT_MOD_LINEAR});
  }
  return out;
}

// Pick an EGL config whose native visual id matches the gbm format, so the
// window surface and the gbm_surface agree on layout.
EGLConfig choose_config(EGLDisplay dpy, std::uint32_t fourcc) {
  const EGLint attr[] = {EGL_SURFACE_TYPE,
                         EGL_WINDOW_BIT,
                         EGL_RENDERABLE_TYPE,
                         EGL_OPENGL_ES2_BIT,
                         EGL_RED_SIZE,
                         8,
                         EGL_GREEN_SIZE,
                         8,
                         EGL_BLUE_SIZE,
                         8,
                         EGL_NONE};
  EGLint n = 0;
  eglChooseConfig(dpy, attr, nullptr, 0, &n);
  std::vector<EGLConfig> cfgs(static_cast<std::size_t>(n));
  eglChooseConfig(dpy, attr, cfgs.data(), n, &n);
  for (EGLConfig c : cfgs) {  // NOLINT(performance-for-range-copy) EGLConfig is a pointer
    EGLint vid = 0;
    if ((eglGetConfigAttrib(dpy, c, EGL_NATIVE_VISUAL_ID, &vid) != 0U) &&
        static_cast<std::uint32_t>(vid) == fourcc) {
      return c;
    }
  }
  return cfgs.empty() ? EGLConfig{} : cfgs.front();
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "/dev/dri/card0";
  const std::uint32_t fourcc = DRM_FORMAT_XRGB8888;

  int const fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    std::perror("open");
    return 1;
  }
  drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

  auto target = kms::pick_target(fd);
  if (!target) {
    std::fprintf(stderr, "no connected output / primary plane found\n");
    close(fd);
    return 1;
  }
  const std::uint32_t w = target->mode.hdisplay;
  const std::uint32_t h = target->mode.vdisplay;
  std::printf("display %s: crtc %u, primary plane %u, %ux%u\n", path, target->crtc_id,
              target->primary_plane, w, h);

  auto tbl = fmt::FormatTable::from_plane(fd, target->primary_plane);
  if (!tbl) {
    std::fprintf(stderr, "no IN_FORMATS: %s\n", tbl.error().message().c_str());
    close(fd);
    return 1;
  }
  auto candidates = rank(tbl->modifiers_for(fourcc));

  gbm_device* gbm = gbm_create_device(fd);
  if (gbm == nullptr) {
    std::fprintf(stderr, "gbm_create_device failed\n");
    close(fd);
    return 1;
  }

  EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
  if (dpy == EGL_NO_DISPLAY || (eglInitialize(dpy, nullptr, nullptr) == 0U)) {
    std::fprintf(stderr, "eglInitialize failed\n");
    return 1;
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLConfig cfg = choose_config(dpy, fourcc);

  // The swapchain: hand Mesa the display's full candidate set so it can choose a
  // displayable-DCC modifier (the whole point) rather than the lone pipe-aligned
  // DCC modifier it offers for standalone buffers.
  const bool force_comp = std::getenv("DRM_FMT_FORCE_COMPRESSION") != nullptr;
  std::vector<std::uint64_t> mods;
  mods.reserve(candidates.size());
  for (fmt::Modifier const m : candidates) {
    if (force_comp && fmt::classify(m) != fmt::BandwidthClass::Compression) {
      continue;  // diagnostic: can the swapchain produce displayable DCC at all?
    }
    mods.push_back(m.value);
  }
  if (mods.empty()) {
    std::fprintf(stderr, "no compression modifier offered by the display\n");
    return 1;
  }
  gbm_surface* surf = gbm_surface_create_with_modifiers2(
      gbm, w, h, fourcc, mods.data(), mods.size(), GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (surf == nullptr) {
    std::fprintf(stderr, "gbm_surface_create_with_modifiers2 failed\n");
    return 1;
  }

  EGLSurface egl_surf = eglCreatePlatformWindowSurface(dpy, cfg, surf, nullptr);
  if (egl_surf == EGL_NO_SURFACE) {
    std::fprintf(stderr, "eglCreatePlatformWindowSurface failed\n");
    return 1;
  }
  const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (ctx == EGL_NO_CONTEXT || (eglMakeCurrent(dpy, egl_surf, egl_surf, ctx) == 0U)) {
    std::fprintf(stderr, "eglMakeCurrent failed\n");
    return 1;
  }

  // Render one frame and swap: Mesa retiles the DCC into display layout here.
  glViewport(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
  glClearColor(0.10F, 0.12F, 0.16F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);
  glScissor(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h / 2));
  glClearColor(0.20F, 0.65F, 0.35F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);
  eglSwapBuffers(dpy, egl_surf);

  gbm_bo* front = gbm_surface_lock_front_buffer(surf);
  if (front == nullptr) {
    std::fprintf(stderr, "gbm_surface_lock_front_buffer failed\n");
    return 1;
  }
  const fmt::Modifier chosen{gbm_bo_get_modifier(front)};
  std::printf("swapchain front buffer: %s  [%s]\n", fmt::describe(chosen).c_str(),
              class_name(fmt::classify(chosen)));

  // Export the front buffer's planes and import them as a KMS FB through the same
  // drm-cxx path the offload demo uses (unified device: same fd both sides).
  const auto nplanes = static_cast<unsigned>(gbm_bo_get_plane_count(front));
  std::vector<fmt::ScanoutBuffer::ImportDesc::Plane> planes(nplanes);
  std::vector<int> fds(nplanes, -1);
  for (unsigned i = 0; i < nplanes; ++i) {
    const int plane = static_cast<int>(i);
    fds[i] = gbm_bo_get_fd_for_plane(front, plane);
    planes[i].dmabuf_fd = fds[i];
    planes[i].stride = gbm_bo_get_stride_for_plane(front, plane);
    planes[i].offset = gbm_bo_get_offset(front, plane);
  }
  std::printf("front buffer has %u DRM plane(s) (color%s)\n", nplanes,
              nplanes > 1 ? " + DCC metadata" : "");

  fmt::ScanoutBuffer::ImportDesc desc;  // explicit init (no designated inits)
  desc.width = w;
  desc.height = h;
  desc.fourcc = fourcc;
  desc.modifier = chosen;
  desc.planes = planes;

  auto fb = fmt::ScanoutBuffer::import_dmabuf(fd, desc);
  for (int const f : fds) {
    if (f >= 0) {
      close(f);
    }
  }
  if (!fb) {
    std::fprintf(stderr, "import_dmabuf: %s\n", fb.error().message().c_str());
    return 1;
  }

  int const test = kms::commit_fb(fd, *target, fb->fb_id(),
                                  DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET);
  std::printf("display TEST_ONLY of %s: %s\n", fmt::describe(chosen).c_str(),
              test == 0 ? "ACCEPTED" : "REJECTED");
  if (test != 0) {
    std::fprintf(stderr, "the display rejected the swapchain modifier -- renegotiate.\n");
    return 1;
  }
  if (int const r = kms::commit_fb(fd, *target, fb->fb_id(), DRM_MODE_ATOMIC_ALLOW_MODESET)) {
    std::fprintf(stderr, "present: %s\n", std::strerror(-r));
    return 1;
  }
  std::printf("on screen (%s scanned out by the display) -- 3s\n", fmt::describe(chosen).c_str());
  sleep(3);

  // Teardown order: the FB borrows `fd`, so release it before the fd closes; the
  // locked front buffer must be released back to the surface before either is
  // destroyed.
  {
    const fmt::ScanoutBuffer released = std::move(*fb);
  }
  gbm_surface_release_buffer(surf, front);
  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(dpy, ctx);
  eglDestroySurface(dpy, egl_surf);
  eglTerminate(dpy);
  gbm_surface_destroy(surf);
  gbm_device_destroy(gbm);
  close(fd);
  return 0;
}
