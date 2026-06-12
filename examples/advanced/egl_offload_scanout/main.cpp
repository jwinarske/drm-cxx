// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// examples/advanced/egl_offload_scanout/main.cpp
//
// Split-render / KMSRO end-to-end with format_mod.hpp: render on the GPU node
// into a modifier'd buffer, export as dma-buf, import on the display node via
// ScanoutBuffer::import_dmabuf, and let the display's TEST_ONLY commit be the
// arbiter. On Rockchip/MediaTek this confirms a Mali-produced AFBC buffer is
// scanned out by VOP2/OVL. On a unified GPU+display device render_fd == disp_fd.
//
// Run:  ./egl_offload_scanout [display=/dev/dri/card0] [render=/dev/dri/renderD128]

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
#include <GLES2/gl2ext.h>
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

// Rank a modifier set COMPRESSION-first, tiling next, LINEAR guaranteed last.
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

}  // namespace

int main(int argc, char** argv) {
  const char* disp_path = argc > 1 ? argv[1] : "/dev/dri/card0";
  const char* rend_path = argc > 2 ? argv[2] : "/dev/dri/renderD128";

  // --- display node -------------------------------------------------------
  int const disp_fd = open(disp_path, O_RDWR | O_CLOEXEC);
  if (disp_fd < 0) {
    std::perror("open display");
    return 1;
  }
  drmSetClientCap(disp_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  drmSetClientCap(disp_fd, DRM_CLIENT_CAP_ATOMIC, 1);

  auto target = kms::pick_target(disp_fd);
  if (!target) {
    std::fprintf(stderr, "no connected output\n");
    return 1;
  }
  const std::uint32_t w = target->mode.hdisplay;
  const std::uint32_t h = target->mode.vdisplay;
  const std::uint32_t fourcc = DRM_FORMAT_XRGB8888;

  auto disp_tbl = fmt::FormatTable::from_plane(disp_fd, target->primary_plane);
  if (!disp_tbl) {
    std::fprintf(stderr, "display plane has no IN_FORMATS: %s\n",
                 disp_tbl.error().message().c_str());
    return 1;
  }
  auto candidates = rank(disp_tbl->modifiers_for(fourcc));
  std::printf("display %s: crtc %u, primary plane %u, %ux%u\n", disp_path, target->crtc_id,
              target->primary_plane, w, h);

  // --- GPU render node (fall back to display node if no separate render node)
  int rend_fd = open(rend_path, O_RDWR | O_CLOEXEC);
  if (rend_fd < 0) {
    std::printf("no %s; rendering on the display node instead\n", rend_path);
    rend_fd = disp_fd;
  }
  gbm_device* gbm = gbm_create_device(rend_fd);
  if (gbm == nullptr) {
    std::fprintf(stderr, "gbm_create_device(render)\n");
    return 1;
  }

  // EGL on the render GBM, surfaceless (render to an FBO backed by our bo).
  EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
  if (dpy == EGL_NO_DISPLAY || (eglInitialize(dpy, nullptr, nullptr) == 0U)) {
    std::fprintf(stderr, "eglInitialize failed\n");
    return 1;
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_DONT_CARE, EGL_RENDERABLE_TYPE,
                             EGL_OPENGL_ES2_BIT, EGL_NONE};
  EGLConfig cfg = nullptr;
  EGLint ncfg = 0;
  eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg);
  const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (ctx == EGL_NO_CONTEXT || (eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) == 0U)) {
    std::fprintf(stderr, "surfaceless context failed (need EGL_KHR_surfaceless_context)\n");
    return 1;
  }

  auto egl_create_image =
      reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  auto egl_destroy_image =
      reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  auto gl_egl_image_target_rb_storage =
      reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
          eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
  if ((egl_create_image == nullptr) || (gl_egl_image_target_rb_storage == nullptr)) {
    std::fprintf(stderr, "missing EGL/GL image extensions\n");
    return 1;
  }

  // Diagnostic: dump the modifiers EGL/radeonsi reports as RENDERABLE for this
  // format, alongside whether the display can scan each out. Set DRM_FMT_DUMP_EGL_MODS=1.
  if (std::getenv("DRM_FMT_DUMP_EGL_MODS") != nullptr) {
    auto q = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(
        eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    if (q != nullptr) {
      EGLint n = 0;
      q(dpy, static_cast<EGLint>(fourcc), 0, nullptr, nullptr, &n);
      std::vector<EGLuint64KHR> egl_mods(static_cast<std::size_t>(n));
      q(dpy, static_cast<EGLint>(fourcc), n, egl_mods.data(), nullptr, &n);
      for (EGLuint64KHR const v : egl_mods) {
        const fmt::Modifier m{v};
        std::printf("  RENDER %#018llx %-40s display=%d\n", static_cast<unsigned long long>(v),
                    fmt::describe(m).c_str(), disp_tbl->supports(fourcc, m) ? 1 : 0);
      }
      for (fmt::Modifier const m : disp_tbl->modifiers_for(fourcc)) {
        std::printf("  DISPLAY %#018llx %-40s\n", static_cast<unsigned long long>(m.value),
                    fmt::describe(m).c_str());
      }
    }
  }

  // GBM picks a modifier it can RENDER to out of the display's scanout-capable
  // candidate list. That intersection is the whole game.
  std::vector<std::uint64_t> mods;
  mods.reserve(candidates.size());
  for (fmt::Modifier const m : candidates) {
    mods.push_back(m.value);
  }
  gbm_bo* bo = gbm_bo_create_with_modifiers2(gbm, w, h, fourcc, mods.data(), mods.size(),
                                             GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
  if (bo == nullptr) {
    // Some GPUs' GBM (e.g. Mesa PowerVR on StarFive) reject a multi-modifier
    // create outright when they only render to LINEAR. Fall back to a plain
    // LINEAR allocation — the "renegotiate toward LINEAR" path the messages
    // below describe — so the offload still completes on LINEAR-only GPUs.
    std::fprintf(stderr,
                 "gbm_bo_create_with_modifiers2 failed; retrying LINEAR "
                 "(GPU likely renders LINEAR-only)\n");
    bo = gbm_bo_create(gbm, w, h, fourcc, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (bo == nullptr) {
      std::fprintf(stderr, "gbm_bo_create (LINEAR) failed\n");
      return 1;
    }
  }
  const fmt::Modifier chosen{gbm_bo_get_modifier(bo)};
  std::printf("GPU rendered into: %s\n", fmt::describe(chosen).c_str());

  EGLImageKHR img = egl_create_image(dpy, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR,
                                     reinterpret_cast<EGLClientBuffer>(bo), nullptr);
  if (img == EGL_NO_IMAGE_KHR) {
    std::fprintf(stderr, "eglCreateImage(bo) failed\n");
    return 1;
  }

  GLuint rb = 0;
  GLuint fbo = 0;
  glGenRenderbuffers(1, &rb);
  glBindRenderbuffer(GL_RENDERBUFFER, rb);
  gl_egl_image_target_rb_storage(GL_RENDERBUFFER, img);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    std::fprintf(stderr,
                 "FBO incomplete for modifier %s -- the GPU can't render this "
                 "layout; rerun forcing LINEAR.\n",
                 fmt::describe(chosen).c_str());
    return 1;
  }
  glViewport(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
  glClearColor(0.10F, 0.12F, 0.16F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);
  glScissor(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h / 2));
  glClearColor(0.85F, 0.30F, 0.20F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);
  glFinish();  // retire the render before the display node reads it

  // --- export each plane as a dma-buf and import on the display node -------
  const auto nplanes = static_cast<unsigned>(gbm_bo_get_plane_count(bo));
  std::vector<fmt::ScanoutBuffer::ImportDesc::Plane> planes(nplanes);
  std::vector<int> fds(nplanes, -1);
  for (unsigned i = 0; i < nplanes; ++i) {
    const int plane = static_cast<int>(i);
    fds[i] = gbm_bo_get_fd_for_plane(bo, plane);  // dup'd; we own and must close
    planes[i].dmabuf_fd = fds[i];
    planes[i].stride = gbm_bo_get_stride_for_plane(bo, plane);
    planes[i].offset = gbm_bo_get_offset(bo, plane);
  }

  fmt::ScanoutBuffer::ImportDesc desc;  // explicit init (no designated inits)
  desc.width = w;
  desc.height = h;
  desc.fourcc = fourcc;
  desc.modifier = chosen;
  desc.planes = planes;  // std::vector -> drm::span

  auto fb = fmt::ScanoutBuffer::import_dmabuf(disp_fd, desc);
  for (int const fd : fds) {
    if (fd >= 0) {
      close(fd);  // import borrowed them; release ours
    }
  }
  if (!fb) {
    std::fprintf(stderr, "import_dmabuf on display node: %s\n", fb.error().message().c_str());
    return 1;
  }

  // --- ground truth + present ---------------------------------------------
  int const test = kms::commit_fb(disp_fd, *target, fb->fb_id(),
                                  DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET);
  std::printf("display TEST_ONLY of %s: %s\n", fmt::describe(chosen).c_str(),
              test == 0 ? "ACCEPTED" : "REJECTED");
  if (test != 0) {
    std::fprintf(stderr,
                 "the display node rejected the GPU's modifier -- on a real "
                 "allocator, drop this edge and renegotiate toward LINEAR.\n");
    return 1;
  }
  if (int const r = kms::commit_fb(disp_fd, *target, fb->fb_id(), DRM_MODE_ATOMIC_ALLOW_MODESET)) {
    std::fprintf(stderr, "present: %s\n", std::strerror(-r));
    return 1;
  }
  std::printf("on screen (GPU-rendered %s scanned out by the display) -- 3s\n",
              fmt::describe(chosen).c_str());
  sleep(3);

  egl_destroy_image(dpy, img);
  glDeleteFramebuffers(1, &fbo);
  glDeleteRenderbuffers(1, &rb);
  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(dpy, ctx);
  eglTerminate(dpy);
  gbm_bo_destroy(bo);
  gbm_device_destroy(gbm);
  if (rend_fd != disp_fd) {
    close(rend_fd);
  }
  // `fb` borrows disp_fd for its RmFB on teardown, so release it before the fd
  // is closed rather than at function-scope end.
  {
    const fmt::ScanoutBuffer released = std::move(*fb);
  }
  close(disp_fd);
  return 0;
}