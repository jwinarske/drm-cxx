// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
// present/gl_blit.cpp

#include "gl_blit.hpp"

#if DRM_CXX_HAS_EGL

#include <drm-cxx/core/egl_loader.hpp>
#include <drm-cxx/core/gles_loader.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/log.hpp>
#include <drm-cxx/present/gl_scanout_producer.hpp>
#include <drm-cxx/scene/buffer_source.hpp>

#include <drm_fourcc.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglplatform.h>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::present::detail {

namespace {

using drm::detail::GLchar;
using drm::detail::GLenum;
using drm::detail::GLfloat;
using drm::detail::GLint;
using drm::detail::GLsizei;
using drm::detail::GLuint;
namespace g = drm::detail::gl;

[[nodiscard]] std::error_code err(std::errc code) noexcept {
  return std::make_error_code(code);
}

// Compile + link the blit program; 0 on failure. `p` is the clip-space
// position; the texture is sampled top row first (memory row 0 at the top).
GLuint build_program(const drm::detail::GlesLoader& gl) {
  constexpr const GLchar* k_vs =
      "attribute vec2 p;\n"
      "varying vec2 uv;\n"
      "void main() {\n"
      "  uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);\n"
      "  gl_Position = vec4(p, 0.0, 1.0);\n"
      "}\n";
  constexpr const GLchar* k_fs =
      "precision mediump float;\n"
      "varying vec2 uv;\n"
      "uniform sampler2D t;\n"
      "void main() { gl_FragColor = texture2D(t, uv); }\n";
  auto compile = [&gl](GLenum type, const GLchar* src) -> GLuint {
    const GLuint sh = gl.create_shader(type);
    gl.shader_source(sh, 1, &src, nullptr);
    gl.compile_shader(sh);
    GLint ok = 0;
    gl.get_shaderiv(sh, g::k_compile_status, &ok);
    if (ok == 0) {
      gl.delete_shader(sh);
      return 0;
    }
    return sh;
  };
  const GLuint vs = compile(g::k_vertex_shader, k_vs);
  const GLuint fs = compile(g::k_fragment_shader, k_fs);
  if (vs == 0 || fs == 0) {
    if (vs != 0) {
      gl.delete_shader(vs);
    }
    if (fs != 0) {
      gl.delete_shader(fs);
    }
    return 0;
  }
  const GLuint prog = gl.create_program();
  gl.attach_shader(prog, vs);
  gl.attach_shader(prog, fs);
  gl.link_program(prog);
  gl.delete_shader(vs);
  gl.delete_shader(fs);
  GLint linked = 0;
  gl.get_programiv(prog, g::k_link_status, &linked);
  if (linked == 0) {
    gl.delete_program(prog);
    return 0;
  }
  return prog;
}

}  // namespace

struct GlBlit::Impl {
  std::unique_ptr<GlScanoutProducer> gl;
  std::unique_ptr<scene::LayerBufferSource> source;
  EGLImageKHR image{EGL_NO_IMAGE_KHR};
  GLuint tex{0};
  GLuint prog{0};
  GLuint vbo{0};
  GLint attr{-1};
  std::uint32_t width{0};
  std::uint32_t height{0};

  ~Impl() {
    if (!gl) {
      return;
    }
    const auto& gles = drm::detail::gles_loader();
    if (gl->make_current()) {
      if (tex != 0) {
        gles.delete_textures(1, &tex);
      }
      if (vbo != 0) {
        gles.delete_buffers(1, &vbo);
      }
      if (prog != 0) {
        gles.delete_program(prog);
      }
    }
    if (image != EGL_NO_IMAGE_KHR) {
      drm::detail::egl_loader().destroy_image(static_cast<EGLDisplay>(gl->egl_display()), image);
    }
  }
};

GlBlit::GlBlit() : impl_(std::make_unique<Impl>()) {}
GlBlit::~GlBlit() = default;

drm::expected<std::unique_ptr<GlBlit>, std::error_code> GlBlit::create(
    drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
    drm::span<const std::uint64_t> allowed, const GlBlitInput& input) {
  const auto& egl = drm::detail::egl_loader();
  const auto& gles = drm::detail::gles_loader();
  if (!egl.loaded || !gles.loaded || egl.create_image == nullptr || egl.destroy_image == nullptr ||
      gles.egl_image_target_texture_2d == nullptr) {
    return drm::unexpected<std::error_code>(err(std::errc::not_supported));
  }
  auto blit = std::unique_ptr<GlBlit>(new GlBlit());
  Impl& s = *blit->impl_;
  s.width = width;
  s.height = height;

  auto producer = GlScanoutProducer::create(dev);
  if (!producer) {
    return drm::unexpected<std::error_code>(producer.error());
  }
  s.gl = std::move(*producer);
  auto source = s.gl->create_buffer(width, height, fourcc, allowed);
  if (!source) {
    return drm::unexpected<std::error_code>(source.error());
  }
  s.source = std::move(*source);
  if (!s.gl->make_current()) {
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }

  std::vector<EGLint> attrs{EGL_WIDTH,
                            static_cast<EGLint>(width),
                            EGL_HEIGHT,
                            static_cast<EGLint>(height),
                            EGL_LINUX_DRM_FOURCC_EXT,
                            static_cast<EGLint>(fourcc),
                            EGL_DMA_BUF_PLANE0_FD_EXT,
                            input.fd,
                            EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                            static_cast<EGLint>(input.offset),
                            EGL_DMA_BUF_PLANE0_PITCH_EXT,
                            static_cast<EGLint>(input.pitch)};
  // LINEAR is implied without the modifier attributes, which a stack lacking
  // EGL_EXT_image_dma_buf_import_modifiers would reject.
  if (input.modifier != DRM_FORMAT_MOD_LINEAR && input.modifier != DRM_FORMAT_MOD_INVALID) {
    attrs.insert(
        attrs.end(),
        {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, static_cast<EGLint>(input.modifier & 0xFFFFFFFFU),
         EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, static_cast<EGLint>(input.modifier >> 32U)});
  }
  attrs.push_back(EGL_NONE);
  s.image = egl.create_image(static_cast<EGLDisplay>(s.gl->egl_display()), EGL_NO_CONTEXT,
                             EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.data());
  if (s.image == EGL_NO_IMAGE_KHR) {
    drm::log_warn("GlBlit: EGL could not import the dma-buf (0x{:x})", egl.get_error());
    return drm::unexpected<std::error_code>(err(std::errc::not_supported));
  }

  s.prog = build_program(gles);
  if (s.prog == 0) {
    return drm::unexpected<std::error_code>(err(std::errc::not_supported));
  }
  s.attr = gles.get_attrib_location(s.prog, "p");
  gles.gen_textures(1, &s.tex);
  gles.bind_texture(g::k_texture_2d, s.tex);
  gles.tex_parameteri(g::k_texture_2d, g::k_tex_min_filter, static_cast<GLint>(g::k_nearest));
  gles.tex_parameteri(g::k_texture_2d, g::k_tex_mag_filter, static_cast<GLint>(g::k_nearest));
  gles.tex_parameteri(g::k_texture_2d, g::k_tex_wrap_s, static_cast<GLint>(g::k_clamp_to_edge));
  gles.tex_parameteri(g::k_texture_2d, g::k_tex_wrap_t, static_cast<GLint>(g::k_clamp_to_edge));
  constexpr std::array<GLfloat, 8> k_quad{-1.0F, -1.0F, 1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F};
  gles.gen_buffers(1, &s.vbo);
  gles.bind_buffer(g::k_array_buffer, s.vbo);
  gles.buffer_data(g::k_array_buffer, sizeof(k_quad), k_quad.data(), g::k_static_draw);
  if (s.attr < 0 || gles.get_error() != g::k_no_error) {
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }
  return blit;
}

std::unique_ptr<scene::LayerBufferSource> GlBlit::take_source() {
  return std::move(impl_->source);
}

bool GlBlit::draw() {
  const auto& gles = drm::detail::gles_loader();
  Impl& s = *impl_;
  if (!s.gl->make_current()) {
    return false;
  }
  gles.viewport(0, 0, static_cast<GLsizei>(s.width), static_cast<GLsizei>(s.height));
  gles.disable(g::k_blend);
  gles.use_program(s.prog);
  gles.bind_buffer(g::k_array_buffer, s.vbo);
  gles.vertex_attrib_pointer(static_cast<GLuint>(s.attr), 2, g::k_float, g::k_false, 0, nullptr);
  gles.enable_vertex_attrib_array(static_cast<GLuint>(s.attr));
  gles.active_texture(g::k_texture0);
  gles.bind_texture(g::k_texture_2d, s.tex);
  // Re-latch the EGLImage every frame so the driver re-reads memory the
  // producer rewrote instead of sampling a cached copy.
  gles.egl_image_target_texture_2d(g::k_texture_2d, s.image);
  gles.draw_arrays(g::k_triangle_strip, 0, 4);
  return gles.get_error() == g::k_no_error;
}

std::optional<std::array<std::uint8_t, 4>> GlBlit::read_center() {
  const auto& gles = drm::detail::gles_loader();
  if (gles.read_pixels == nullptr) {
    return std::nullopt;
  }
  std::array<std::uint8_t, 4> px{};
  gles.read_pixels(static_cast<GLint>(impl_->width / 2U), static_cast<GLint>(impl_->height / 2U), 1,
                   1, g::k_rgba, g::k_unsigned_byte, px.data());
  return px;
}

drm::expected<void, std::error_code> GlBlit::present() {
  return impl_->gl->swap_buffers();
}

}  // namespace drm::present::detail

#endif  // DRM_CXX_HAS_EGL
