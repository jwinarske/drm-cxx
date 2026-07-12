// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "gl_compositor.hpp"

#if DRM_CXX_HAS_EGL

#include "buffer_source.hpp"
#include "composite_canvas.hpp"
#include "composition_target.hpp"
#include "gbm_surface_source.hpp"
#include "gl_compositor_geometry.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/egl_loader.hpp>
#include <drm-cxx/core/gles_loader.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/log.hpp>

#include <drm_fourcc.h>
#include <gbm.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglplatform.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::scene {

namespace {

[[nodiscard]] std::error_code err(std::errc code) noexcept {
  return std::make_error_code(code);
}

// Opt-in: when the default GL renderer is software, retry the context with
// MESA_LOADER_DRIVER_OVERRIDE=zink to reach hardware GL via zink (GL-on-Vulkan)
// on embedded split-GPU boards whose Mesa GL on the display node is software
// (the kmsro display stub is missing) but which ship a hardware Vulkan driver.
// Off by default because zink over some PowerVR Vulkan drivers (e.g. the B-series
// on JH7110) segfaults during render; enable per-board where zink is known good.
[[nodiscard]] bool zink_retry_requested() noexcept {
  const char* val = std::getenv("DRM_CXX_COMPOSITOR_ZINK");
  if (val == nullptr) {
    return false;
  }
  const std::string_view sval(val);
  return sval == "1" || sval == "true" || sval == "yes";
}

constexpr const char* k_vertex_src = R"(
attribute vec2 a_pos;
attribute vec2 a_uv;
varying vec2 v_uv;
void main() {
  gl_Position = vec4(a_pos, 0.0, 1.0);
  v_uv = a_uv;
}
)";

// The CPU-upload path uploads BGRA-in-memory (ARGB8888/XRGB8888) as GL_RGBA and
// needs a .bgra swizzle to correct the channel order (u_bgra = 1). A dma-buf
// imported as an EGLImage is sampled in the correct channel order by the driver,
// so it must NOT swizzle (u_bgra = 0). u_opaque forces alpha=1 for XRGB. Output
// is premultiplied and per-layer alpha scales all four channels, matching
// CompositeCanvas's premultiplied SRC_OVER under
// glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
constexpr const char* k_fragment_src = R"(
precision highp float;
varying vec2 v_uv;
uniform sampler2D u_tex;
uniform float u_alpha;
uniform float u_opaque;
uniform float u_bgra;
void main() {
  vec4 s = texture2D(u_tex, v_uv);
  vec4 t = mix(s, s.bgra, u_bgra);
  float a = mix(t.a, 1.0, u_opaque);
  gl_FragColor = vec4(t.rgb, a) * u_alpha;
}
)";

// Planar-YUV (NV12) variant: samples a GL_TEXTURE_EXTERNAL_OES bound to the
// EGLImage of a YUV dma-buf. The driver does the YUV->RGB conversion inside the
// sampler, so this shader gets RGB straight (no .bgra swizzle) and treats the
// source as opaque (NV12 has no alpha). Per-layer alpha still scales all four
// channels for premultiplied SRC_OVER, matching the RGB path.
constexpr const char* k_fragment_ext_src = R"(
#extension GL_OES_EGL_image_external : require
precision highp float;
varying vec2 v_uv;
uniform samplerExternalOES u_tex;
uniform float u_alpha;
void main() {
  vec3 rgb = texture2D(u_tex, v_uv).rgb;
  gl_FragColor = vec4(rgb, 1.0) * u_alpha;
}
)";

// Compile + link a quad program with the given fragment source. Returns 0 on
// failure.
[[nodiscard]] std::uint32_t build_program(const drm::detail::GlesLoader& gl,
                                          const char* fragment_src) noexcept {
  auto compile = [&](drm::detail::GLenum type, const char* src) -> std::uint32_t {
    const std::uint32_t sh = gl.create_shader(type);
    if (sh == 0) {
      return 0;
    }
    const std::array<const char*, 1> srcs{src};
    gl.shader_source(sh, 1, srcs.data(), nullptr);
    gl.compile_shader(sh);
    drm::detail::GLint ok = 0;
    gl.get_shaderiv(sh, drm::detail::gl::k_compile_status, &ok);
    if (ok == 0) {
      std::array<char, 512> log{};
      gl.get_shader_info_log(sh, static_cast<drm::detail::GLsizei>(log.size()), nullptr,
                             log.data());
      drm::log_warn("GlCompositor: shader compile failed: {}", log.data());
      gl.delete_shader(sh);
      return 0;
    }
    return sh;
  };

  const std::uint32_t vs = compile(drm::detail::gl::k_vertex_shader, k_vertex_src);
  const std::uint32_t fs = compile(drm::detail::gl::k_fragment_shader, fragment_src);
  if (vs == 0 || fs == 0) {
    if (vs != 0) {
      gl.delete_shader(vs);
    }
    if (fs != 0) {
      gl.delete_shader(fs);
    }
    return 0;
  }
  const std::uint32_t prog = gl.create_program();
  gl.attach_shader(prog, vs);
  gl.attach_shader(prog, fs);
  gl.link_program(prog);
  gl.delete_shader(vs);
  gl.delete_shader(fs);
  drm::detail::GLint ok = 0;
  gl.get_programiv(prog, drm::detail::gl::k_link_status, &ok);
  if (ok == 0) {
    std::array<char, 512> log{};
    gl.get_program_info_log(prog, static_cast<drm::detail::GLsizei>(log.size()), nullptr,
                            log.data());
    drm::log_warn("GlCompositor: program link failed: {}", log.data());
    gl.delete_program(prog);
    return 0;
  }
  return prog;
}

}  // namespace

drm::expected<std::unique_ptr<GlCompositor>, std::error_code> GlCompositor::create(
    const drm::Device& dev, const CompositeCanvasConfig& cfg) {
  const auto& egl = drm::detail::egl_loader();
  const auto& gl = drm::detail::gles_loader();
  if (!egl.loaded || (egl.get_platform_display_core == nullptr) || !gl.loaded) {
    return drm::unexpected<std::error_code>(err(std::errc::not_supported));
  }
  if (cfg.canvas_width == 0U || cfg.canvas_height == 0U) {
    return drm::unexpected<std::error_code>(err(std::errc::invalid_argument));
  }

  std::unique_ptr<GlCompositor> out(new GlCompositor);
  out->dev_ = &dev;
  out->width_ = cfg.canvas_width;
  out->height_ = cfg.canvas_height;
  out->fourcc_ = (cfg.output_fourcc != 0U) ? cfg.output_fourcc : DRM_FORMAT_ARGB8888;
  out->allow_software_ = cfg.allow_software_renderer;

  GbmSurfaceConfig gcfg;
  gcfg.width = out->width_;
  gcfg.height = out->height_;
  gcfg.drm_format = out->fourcc_;
  gcfg.modifier = DRM_FORMAT_MOD_INVALID;  // let GBM choose; demote-on-commit-fail covers misses
  gcfg.usage = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
  auto src = GbmSurfaceSource::create(dev, gcfg);
  if (!src) {
    return drm::unexpected<std::error_code>(src.error());
  }
  out->source_ = std::move(*src);

  auto r = out->init_egl();
  // Auto-zink: if the default renderer was software (init_egl rejected it) and
  // the caller opted in via DRM_CXX_COMPOSITOR_ZINK, retry once forcing zink —
  // unless the user already pinned a driver via MESA_LOADER_DRIVER_OVERRIDE. The
  // switch needs a fresh gbm device (validated on PowerVR Rogue), so recreate the
  // source. If zink also lands on software (e.g. lavapipe), init_egl's guard
  // rejects it again and we fall back to the CPU canvas.
  if (!r && !out->allow_software_ && zink_retry_requested() &&
      (std::getenv("MESA_LOADER_DRIVER_OVERRIDE") == nullptr)) {
    drm::log_info("GlCompositor: software renderer — retrying with zink for hardware GL");
    // setenv is POSIX (declared via <cstdlib> on glibc); include-cleaner maps it
    // to <stdlib.h>, which modernize-deprecated-headers forbids — NOLINT the call.
    // NOLINTNEXTLINE(misc-include-cleaner)
    static_cast<void>(::setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1));
    auto src2 = GbmSurfaceSource::create(dev, gcfg);
    if (src2) {
      out->source_ = std::move(*src2);
      r = out->init_egl();
    }
  }
  if (!r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  return out;
}

GlCompositor::~GlCompositor() {
  // EGL down first (while the gbm_surface owned by source_ is still alive),
  // then release any locked buffer, then source_ destructs last.
  teardown_egl();
  if (held_ && source_) {
    source_->release(std::move(*held_));
  }
}

drm::expected<void, std::error_code> GlCompositor::init_egl() {
  const auto& egl = drm::detail::egl_loader();
  const auto& gl = drm::detail::gles_loader();

  EGLDisplay display =
      egl.get_platform_display_core(EGL_PLATFORM_GBM_KHR, source_->native_device(), nullptr);
  if ((display == EGL_NO_DISPLAY) || (egl.initialize(display, nullptr, nullptr) != EGL_TRUE)) {
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }
  display_ = display;
  if (egl.bind_api(EGL_OPENGL_ES_API) != EGL_TRUE) {
    teardown_egl();
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }

  const std::array<EGLint, 13> cfg_attrs{EGL_SURFACE_TYPE,
                                         EGL_WINDOW_BIT,
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
                                         EGL_NONE};
  EGLint num = 0;
  if ((egl.choose_config(display, cfg_attrs.data(), nullptr, 0, &num) != EGL_TRUE) || (num == 0)) {
    teardown_egl();
    return drm::unexpected<std::error_code>(err(std::errc::not_supported));
  }
  std::vector<EGLConfig> configs(static_cast<std::size_t>(num));
  egl.choose_config(display, cfg_attrs.data(), configs.data(), num, &num);
  EGLConfig chosen = nullptr;
  for (auto* const cand : configs) {
    EGLint vis = 0;
    if ((egl.get_config_attrib(display, cand, EGL_NATIVE_VISUAL_ID, &vis) == EGL_TRUE) &&
        (static_cast<std::uint32_t>(vis) == fourcc_)) {
      chosen = cand;
      break;
    }
  }
  if (chosen == nullptr) {
    teardown_egl();
    return drm::unexpected<std::error_code>(err(std::errc::not_supported));
  }

  const std::array<EGLint, 3> ctx_attrs{EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext context = egl.create_context(display, chosen, EGL_NO_CONTEXT, ctx_attrs.data());
  if (context == EGL_NO_CONTEXT) {
    teardown_egl();
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }
  context_ = context;

  EGLSurface surface = EGL_NO_SURFACE;
  if (egl.create_platform_window_surface != nullptr) {
    surface =
        egl.create_platform_window_surface(display, chosen, source_->native_surface(), nullptr);
  }
  if (surface == EGL_NO_SURFACE) {
    teardown_egl();
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }
  surface_ = surface;

  if (egl.make_current(display, surface, surface, context) != EGL_TRUE) {
    teardown_egl();
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }

  // Reject software renderers: a software GL path is no faster than — and usually
  // slower than — the CPU CompositeCanvas it would displace, so Auto mode must
  // fall back to the CPU canvas rather than compose through software GL. This is
  // what keeps a GPU-down boot off the GPU path (e.g. PowerVR init failure on
  // JH7110, where Mesa offers only llvmpipe on the display node). The markers
  // also catch GL-on-Vulkan (zink): `zink (PowerVR Rogue ...)` on a real GPU is
  // accepted, while zink over a software Vulkan reports the SW device in its
  // GL_RENDERER — lavapipe ("llvmpipe"), SwiftShader — and is rejected.
  if (!allow_software_ && (gl.get_string != nullptr)) {
    const auto* renderer =
        reinterpret_cast<const char*>(gl.get_string(drm::detail::gl::k_renderer));
    if (renderer != nullptr) {
      std::string name(renderer);
      std::transform(name.begin(), name.end(), name.begin(),
                     [](unsigned char chr) { return static_cast<char>(std::tolower(chr)); });
      constexpr std::array<const char*, 6> k_software_markers{
          "llvmpipe", "softpipe", "swrast", "software", "lavapipe", "swiftshader"};
      const bool is_software = std::any_of(
          k_software_markers.begin(), k_software_markers.end(),
          [&name](const char* marker) { return name.find(marker) != std::string::npos; });
      if (is_software) {
        if (!zink_retry_requested() && (std::getenv("MESA_LOADER_DRIVER_OVERRIDE") == nullptr)) {
          drm::log_info(
              "GlCompositor: software renderer ({}) — using CPU composition. Set "
              "DRM_CXX_COMPOSITOR_ZINK=1 to try hardware GL via zink if this board has a "
              "Vulkan driver.",
              renderer);
        } else {
          drm::log_info("GlCompositor: software renderer ({}) — using CPU composition instead",
                        renderer);
        }
        teardown_egl();
        return drm::unexpected<std::error_code>(err(std::errc::not_supported));
      }
    }
  }

  program_ = build_program(gl, k_fragment_src);
  if (program_ == 0) {
    teardown_egl();
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }
  loc_tex_ = gl.get_uniform_location(program_, "u_tex");
  loc_alpha_ = gl.get_uniform_location(program_, "u_alpha");
  loc_opaque_ = gl.get_uniform_location(program_, "u_opaque");
  loc_bgra_ = gl.get_uniform_location(program_, "u_bgra");
  attr_pos_ = gl.get_attrib_location(program_, "a_pos");
  attr_uv_ = gl.get_attrib_location(program_, "a_uv");
  gl.gen_buffers(1, &vbo_);
  gl.gen_textures(1, &texture_);

  // One-time DMA-BUF import capability probe. The EGLImage composition path
  // needs EGL_EXT_image_dma_buf_import advertised on this display plus the two
  // entry points it drives (eglCreateImageKHR + glEGLImageTargetTexture2DOES).
  // When any is missing the compositor keeps the CPU-upload path.
  const char* egl_exts = egl.query_string(display, EGL_EXTENSIONS);
  dmabuf_import_supported_ =
      drm::detail::extension_present(egl_exts, "EGL_EXT_image_dma_buf_import") &&
      (egl.create_image != nullptr) && (egl.destroy_image != nullptr) &&
      (gl.egl_image_target_texture_2d != nullptr);
  if (dmabuf_import_supported_) {
    drm::log_info("gl_compositor: EGL dma-buf import available (EGLImage composition path)");
  }

  // Planar-YUV (NV12) import needs the GLES GL_OES_EGL_image_external extension
  // (samplerExternalOES) on top of the dma-buf import capability. Optional: when
  // it (or its program) is unavailable, RGB import still works and NV12 sources
  // stay on the drop/CPU path. Probing GL_EXTENSIONS first avoids a spurious
  // shader-compile warning on stacks without the extension.
  if (dmabuf_import_supported_ && (gl.get_string != nullptr)) {
    // glGetString returns unsigned char*; the extension string is plain ASCII.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* gl_exts =
        reinterpret_cast<const char*>(gl.get_string(drm::detail::gl::k_extensions));
    if (drm::detail::extension_present(gl_exts, "GL_OES_EGL_image_external")) {
      program_ext_ = build_program(gl, k_fragment_ext_src);
      if (program_ext_ != 0) {
        loc_tex_ext_ = gl.get_uniform_location(program_ext_, "u_tex");
        loc_alpha_ext_ = gl.get_uniform_location(program_ext_, "u_alpha");
        attr_pos_ext_ = gl.get_attrib_location(program_ext_, "a_pos");
        attr_uv_ext_ = gl.get_attrib_location(program_ext_, "a_uv");
        gl.gen_textures(1, &texture_ext_);
        drm::log_info("gl_compositor: external-sampler NV12 dma-buf import available");
      }
    }
  }

  armable_ = true;
  return {};
}

void GlCompositor::teardown_egl() noexcept {
  const auto& egl = drm::detail::egl_loader();
  armable_ = false;
  auto* const display = static_cast<EGLDisplay>(display_);
  if (display != nullptr) {
    if (egl.make_current != nullptr) {
      egl.make_current(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if ((surface_ != nullptr) && (egl.destroy_surface != nullptr)) {
      egl.destroy_surface(display, static_cast<EGLSurface>(surface_));
    }
    if ((context_ != nullptr) && (egl.destroy_context != nullptr)) {
      egl.destroy_context(display, static_cast<EGLContext>(context_));
    }
    if (egl.terminate != nullptr) {
      egl.terminate(display);
    }
  }
  display_ = nullptr;
  context_ = nullptr;
  surface_ = nullptr;
  // GL objects belonged to the destroyed context.
  program_ = 0;
  program_ext_ = 0;
  vbo_ = 0;
  texture_ = 0;
  texture_ext_ = 0;
  frame_open_ = false;
}

void GlCompositor::begin_frame() noexcept {
  const auto& egl = drm::detail::egl_loader();
  const auto& gl = drm::detail::gles_loader();
  frame_open_ = false;
  if ((display_ == nullptr) || (surface_ == nullptr) || (context_ == nullptr)) {
    return;
  }
  if (egl.make_current(static_cast<EGLDisplay>(display_), static_cast<EGLSurface>(surface_),
                       static_cast<EGLSurface>(surface_),
                       static_cast<EGLContext>(context_)) != EGL_TRUE) {
    return;
  }
  gl.viewport(0, 0, static_cast<drm::detail::GLsizei>(width_),
              static_cast<drm::detail::GLsizei>(height_));
  gl.use_program(program_);
  gl.enable(drm::detail::gl::k_blend);
  gl.blend_func(drm::detail::gl::k_one, drm::detail::gl::k_one_minus_src_alpha);
  gl.bind_buffer(drm::detail::gl::k_array_buffer, vbo_);
  frame_open_ = true;
}

void GlCompositor::clear() noexcept {
  if (!frame_open_) {
    return;
  }
  const auto& gl = drm::detail::gles_loader();
  gl.clear_color(0.0F, 0.0F, 0.0F, 0.0F);
  gl.clear(drm::detail::gl::k_color_buffer_bit);
}

void* GlCompositor::import_dma_buf_image(const CompositeSrc& src) noexcept {
  const auto& egl = drm::detail::egl_loader();
  if (egl.create_image == nullptr) {
    return EGL_NO_IMAGE_KHR;
  }
  // Only pass an explicit modifier (and the LO/HI attribute pair) for a real
  // tiling modifier when the stack advertises the modifiers query; LINEAR /
  // INVALID import without it (the driver assumes linear).
  const bool has_modifier = (src.dma_modifier != 0U) &&
                            (src.dma_modifier != DRM_FORMAT_MOD_INVALID) &&
                            (egl.query_dma_buf_modifiers != nullptr);
  // Per-plane attribute enums (PLANE0..2 — the base dma_buf_import extension;
  // enough for NV12 (2 planes) and YUV420 (3)). The modifier LO/HI enums come
  // with import_modifiers, already gated by `has_modifier`.
  constexpr std::array<EGLint, 3> fd_attr{EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
                                          EGL_DMA_BUF_PLANE2_FD_EXT};
  constexpr std::array<EGLint, 3> off_attr{
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT};
  constexpr std::array<EGLint, 3> pitch_attr{
      EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT};
  constexpr std::array<EGLint, 3> modlo_attr{EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                                             EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                                             EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT};
  constexpr std::array<EGLint, 3> modhi_attr{EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                                             EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
                                             EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT};
  std::array<EGLint, 40> attribs{};
  std::size_t i = 0;
  attribs.at(i++) = EGL_WIDTH;
  attribs.at(i++) = static_cast<EGLint>(src.src_width);
  attribs.at(i++) = EGL_HEIGHT;
  attribs.at(i++) = static_cast<EGLint>(src.src_height);
  attribs.at(i++) = EGL_LINUX_DRM_FOURCC_EXT;
  attribs.at(i++) = static_cast<EGLint>(src.drm_fourcc);
  const std::size_t n_planes = std::min<std::size_t>(src.dma_n_planes, fd_attr.size());
  for (std::size_t p = 0; p < n_planes; ++p) {
    attribs.at(i++) = fd_attr.at(p);
    attribs.at(i++) = src.dma_fds.at(p);
    attribs.at(i++) = off_attr.at(p);
    attribs.at(i++) = static_cast<EGLint>(src.dma_offsets.at(p));
    attribs.at(i++) = pitch_attr.at(p);
    attribs.at(i++) = static_cast<EGLint>(src.dma_pitches.at(p));
    if (has_modifier) {
      attribs.at(i++) = modlo_attr.at(p);
      attribs.at(i++) = static_cast<EGLint>(src.dma_modifier & 0xFFFFFFFFU);
      attribs.at(i++) = modhi_attr.at(p);
      attribs.at(i++) = static_cast<EGLint>(src.dma_modifier >> 32U);
    }
  }
  attribs.at(i++) = EGL_NONE;
  return egl.create_image(static_cast<EGLDisplay>(display_), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                          static_cast<EGLClientBuffer>(nullptr), attribs.data());
}

bool GlCompositor::supports_dma_buf_import(std::uint32_t drm_fourcc) const noexcept {
  // blend() imports single-plane RGB (ARGB8888 / XRGB8888) via a sampler2D and,
  // when the external-sampler program built (GL_OES_EGL_image_external), NV12
  // via a GL_TEXTURE_EXTERNAL_OES sampler that the driver YUV->RGB-converts.
  return dmabuf_import_supported_ &&
         ((drm_fourcc == DRM_FORMAT_ARGB8888) || (drm_fourcc == DRM_FORMAT_XRGB8888) ||
          ((program_ext_ != 0) && (drm_fourcc == DRM_FORMAT_NV12)));
}

void GlCompositor::blend(const CompositeSrc& src, const CompositeRect& src_rect,
                         const CompositeRect& dst_rect) noexcept {
  if (!frame_open_) {
    return;
  }
  const bool is_rgb =
      (src.drm_fourcc == DRM_FORMAT_ARGB8888) || (src.drm_fourcc == DRM_FORMAT_XRGB8888);
  const bool is_nv12 = (src.drm_fourcc == DRM_FORMAT_NV12);
  if (!is_rgb && !is_nv12) {
    return;  // RGB (CPU path's set) plus NV12 via the external sampler
  }
  if ((src.src_width == 0U) || (src.src_height == 0U) || (dst_rect.w == 0U) || (dst_rect.h == 0U)) {
    return;
  }
  const auto& gl = drm::detail::gles_loader();

  // Planar-YUV path: import the multi-plane dma-buf into a
  // GL_TEXTURE_EXTERNAL_OES sampler that the driver YUV->RGB-converts. NV12 has
  // no CPU-upload fallback here (a camera/decoder layer with no map()), so a
  // failed import just drops the source this frame.
  if (is_nv12) {
    if ((program_ext_ == 0) || (src.dma_n_planes < 2U) || (src.dma_fds.at(0) < 0)) {
      return;
    }
    void* const nv12_image = import_dma_buf_image(src);
    if (nv12_image == EGL_NO_IMAGE_KHR) {
      return;
    }
    gl.active_texture(drm::detail::gl::k_texture0);
    gl.bind_texture(drm::detail::gl::k_texture_external_oes, texture_ext_);
    gl.egl_image_target_texture_2d(drm::detail::gl::k_texture_external_oes, nv12_image);
    gl.tex_parameteri(drm::detail::gl::k_texture_external_oes, drm::detail::gl::k_tex_min_filter,
                      static_cast<drm::detail::GLint>(drm::detail::gl::k_nearest));
    gl.tex_parameteri(drm::detail::gl::k_texture_external_oes, drm::detail::gl::k_tex_mag_filter,
                      static_cast<drm::detail::GLint>(drm::detail::gl::k_nearest));
    gl.tex_parameteri(drm::detail::gl::k_texture_external_oes, drm::detail::gl::k_tex_wrap_s,
                      static_cast<drm::detail::GLint>(drm::detail::gl::k_clamp_to_edge));
    gl.tex_parameteri(drm::detail::gl::k_texture_external_oes, drm::detail::gl::k_tex_wrap_t,
                      static_cast<drm::detail::GLint>(drm::detail::gl::k_clamp_to_edge));
    gl.use_program(program_ext_);
    gl.uniform1i(loc_tex_ext_, 0);
    gl.uniform1f(loc_alpha_ext_, static_cast<float>(src.plane_alpha) / 65535.0F);
    draw_source_quad(attr_pos_ext_, attr_uv_ext_, src_rect, dst_rect, src);
    const auto& egl = drm::detail::egl_loader();
    if (egl.destroy_image != nullptr) {
      egl.destroy_image(static_cast<EGLDisplay>(display_), nv12_image);
    }
    // Restore the default RGB program for any later blends this frame.
    gl.use_program(program_);
    return;
  }

  gl.active_texture(drm::detail::gl::k_texture0);
  gl.bind_texture(drm::detail::gl::k_texture_2d, texture_);

  // Direct DMA-BUF import: a single-plane RGB source carrying dma-buf fds is
  // imported as an EGLImage and sampled straight, skipping the CPU-pixel upload
  // (the escape from the map()-uncompositable trap for camera layers). Falls
  // back to the CPU path when the import fails or the stack can't do it.
  void* egl_image = EGL_NO_IMAGE_KHR;
  if ((src.dma_n_planes == 1) && dmabuf_import_supported_ && (src.dma_fds.at(0) >= 0)) {
    egl_image = import_dma_buf_image(src);
  }
  if (egl_image != EGL_NO_IMAGE_KHR) {
    gl.egl_image_target_texture_2d(drm::detail::gl::k_texture_2d, egl_image);
  } else {
    // CPU-pixel upload path. A dma-buf-only source with no CPU pixels (import
    // unavailable or failed) can't be sampled here — skip it.
    if (src.pixels.empty()) {
      return;
    }
    // GLES2 has no GL_UNPACK_ROW_LENGTH; compact to a tightly-packed staging
    // buffer when the source stride is padded.
    const std::size_t tight = static_cast<std::size_t>(src.src_width) * 4U;
    const std::uint8_t* upload = src.pixels.data();
    std::vector<std::uint8_t> packed;
    if (src.src_stride_bytes != tight) {
      packed.resize(tight * src.src_height);
      for (std::uint32_t row = 0; row < src.src_height; ++row) {
        const std::size_t so = static_cast<std::size_t>(row) * src.src_stride_bytes;
        if (so + tight > src.pixels.size()) {
          return;
        }
        std::copy_n(src.pixels.data() + so, tight,
                    packed.data() + (static_cast<std::size_t>(row) * tight));
      }
      upload = packed.data();
    } else if (src.pixels.size() < tight * src.src_height) {
      return;
    }
    gl.pixel_storei(drm::detail::gl::k_unpack_alignment, 4);
    gl.tex_image_2d(drm::detail::gl::k_texture_2d, 0,
                    static_cast<drm::detail::GLint>(drm::detail::gl::k_rgba),
                    static_cast<drm::detail::GLsizei>(src.src_width),
                    static_cast<drm::detail::GLsizei>(src.src_height), 0, drm::detail::gl::k_rgba,
                    drm::detail::gl::k_unsigned_byte, upload);
  }

  gl.tex_parameteri(drm::detail::gl::k_texture_2d, drm::detail::gl::k_tex_min_filter,
                    static_cast<drm::detail::GLint>(drm::detail::gl::k_nearest));
  gl.tex_parameteri(drm::detail::gl::k_texture_2d, drm::detail::gl::k_tex_mag_filter,
                    static_cast<drm::detail::GLint>(drm::detail::gl::k_nearest));
  gl.tex_parameteri(drm::detail::gl::k_texture_2d, drm::detail::gl::k_tex_wrap_s,
                    static_cast<drm::detail::GLint>(drm::detail::gl::k_clamp_to_edge));
  gl.tex_parameteri(drm::detail::gl::k_texture_2d, drm::detail::gl::k_tex_wrap_t,
                    static_cast<drm::detail::GLint>(drm::detail::gl::k_clamp_to_edge));

  gl.uniform1i(loc_tex_, 0);
  gl.uniform1f(loc_alpha_, static_cast<float>(src.plane_alpha) / 65535.0F);
  gl.uniform1f(loc_opaque_, (src.drm_fourcc == DRM_FORMAT_XRGB8888) ? 1.0F : 0.0F);
  // Imported dma-buf is sampled in-order by the driver; the CPU upload needs
  // the .bgra swizzle (ARGB8888 bytes uploaded as RGBA).
  gl.uniform1f(loc_bgra_, (egl_image != EGL_NO_IMAGE_KHR) ? 0.0F : 1.0F);

  draw_source_quad(attr_pos_, attr_uv_, src_rect, dst_rect, src);

  // The texture keeps its EGLImage sibling storage after the target bind, so the
  // image can be destroyed once the draw referencing it is issued. The fds in
  // the descriptor are borrowed (owned by the layer's source) and left untouched.
  if (egl_image != EGL_NO_IMAGE_KHR) {
    const auto& egl = drm::detail::egl_loader();
    if (egl.destroy_image != nullptr) {
      egl.destroy_image(static_cast<EGLDisplay>(display_), egl_image);
    }
  }
}

void GlCompositor::draw_source_quad(std::int32_t attr_pos, std::int32_t attr_uv,
                                    const CompositeRect& src_rect, const CompositeRect& dst_rect,
                                    const CompositeSrc& src) const noexcept {
  const auto& gl = drm::detail::gles_loader();
  const auto verts =
      gl_geom::quad(dst_rect.x, dst_rect.y, dst_rect.w, dst_rect.h, width_, height_, src_rect.x,
                    src_rect.y, src_rect.w, src_rect.h, src.src_width, src.src_height);
  gl.buffer_data(drm::detail::gl::k_array_buffer,
                 static_cast<drm::detail::GLsizeiptr>(sizeof(verts)), verts.data(),
                 drm::detail::gl::k_static_draw);
  const auto stride = static_cast<drm::detail::GLsizei>(sizeof(gl_geom::QuadVertex));
  gl.enable_vertex_attrib_array(static_cast<drm::detail::GLuint>(attr_pos));
  gl.vertex_attrib_pointer(static_cast<drm::detail::GLuint>(attr_pos), 2, drm::detail::gl::k_float,
                           drm::detail::gl::k_false, stride, nullptr);
  gl.enable_vertex_attrib_array(static_cast<drm::detail::GLuint>(attr_uv));
  // u,v live at byte offset 2*sizeof(float) within each vertex. The
  // attrib-offset-as-pointer is the canonical GLES2 idiom.
  // NOLINTNEXTLINE(performance-no-int-to-ptr,cppcoreguidelines-pro-type-reinterpret-cast)
  const void* uv_offset = reinterpret_cast<const void*>(2 * sizeof(float));
  gl.vertex_attrib_pointer(static_cast<drm::detail::GLuint>(attr_uv), 2, drm::detail::gl::k_float,
                           drm::detail::gl::k_false, stride, uv_offset);
  gl.draw_arrays(drm::detail::gl::k_triangle_strip, 0, 4);
}

drm::expected<void, std::error_code> GlCompositor::flush() noexcept {
  const auto& egl = drm::detail::egl_loader();
  frame_open_ = false;
  if ((display_ == nullptr) || (surface_ == nullptr)) {
    return drm::unexpected<std::error_code>(err(std::errc::not_connected));
  }
  if (egl.swap_buffers(static_cast<EGLDisplay>(display_), static_cast<EGLSurface>(surface_)) !=
      EGL_TRUE) {
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }
  auto acq = source_->acquire();
  if (!acq) {
    return drm::unexpected<std::error_code>(acq.error());
  }
  // Release the previous frame's locked buffer now that a new one is ready.
  if (held_) {
    source_->release(std::move(*held_));
  }
  fb_id_ = acq->fb_id;
  held_ = std::make_unique<AcquiredBuffer>(std::move(*acq));
  return {};
}

void GlCompositor::on_session_paused() noexcept {
  teardown_egl();
  // The fd is revoked; drop the locked buffer without an ioctl and let the
  // source forget its bos.
  held_.reset();
  fb_id_ = 0;
  if (source_) {
    source_->on_session_paused();
  }
}

drm::expected<void, std::error_code> GlCompositor::read_back(std::vector<std::uint8_t>& out) {
  if (!held_ || (held_->opaque == nullptr)) {
    return drm::unexpected<std::error_code>(err(std::errc::not_connected));
  }
  auto* bo = static_cast<gbm_bo*>(held_->opaque);
  std::uint32_t stride = 0;
  void* map_data = nullptr;
  void* ptr = gbm_bo_map(bo, 0, 0, width_, height_, GBM_BO_TRANSFER_READ, &stride, &map_data);
  if ((ptr == nullptr) || (ptr == MAP_FAILED)) {
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }
  const std::size_t row = static_cast<std::size_t>(width_) * 4U;  // canvas is 32bpp
  out.resize(row * height_);
  const auto* src = static_cast<const std::uint8_t*>(ptr);
  for (std::uint32_t y = 0; y < height_; ++y) {
    std::copy_n(src + (static_cast<std::size_t>(y) * stride), row, out.data() + (y * row));
  }
  gbm_bo_unmap(bo, map_data);
  return {};
}

drm::expected<void, std::error_code> GlCompositor::on_session_resumed(const drm::Device& new_dev) {
  dev_ = &new_dev;
  if (!source_) {
    return drm::unexpected<std::error_code>(err(std::errc::not_connected));
  }
  if (auto r = source_->on_session_resumed(new_dev); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  return init_egl();
}

}  // namespace drm::scene

#endif  // DRM_CXX_HAS_EGL
