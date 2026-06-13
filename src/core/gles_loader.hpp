// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// gles_loader.hpp — process-singleton libGLESv2.so.2 dlopen wrapper, the
// GLES2 counterpart of egl_loader.hpp. Used by the GPU compositor
// (scene/gl_compositor.cpp).
//
// The library must stay loadable on GPU-less systems, so it never links
// libGLESv2; the entry points are resolved at runtime via dlsym (with an
// eglGetProcAddress fallback for glvnd setups). To avoid a build-time
// dependency on the GLES2 headers (the EGL gate only guarantees EGL
// headers), the small slice of the GLES2 ABI the compositor needs — the
// scalar types, the constants, and the function-pointer typedefs — is
// declared locally below. These are stable Khronos ABI values.
//
// Internal header; not installed. Gated on DRM_CXX_HAS_EGL (GPU
// composition rides the EGL feature).

#pragma once

#if DRM_CXX_HAS_EGL

#include <cstddef>
#include <cstdint>

namespace drm::detail {

// ── Minimal GLES2 ABI (stable Khronos values) ───────────────────────────
using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLint = int;
using GLsizei = int;
using GLuint = unsigned int;
using GLfloat = float;
using GLchar = char;
using GLsizeiptr = std::intptr_t;

namespace gl {
inline constexpr GLenum k_false = 0;
inline constexpr GLenum k_no_error = 0;
inline constexpr GLenum k_vertex_shader = 0x8B31;
inline constexpr GLenum k_fragment_shader = 0x8B30;
inline constexpr GLenum k_compile_status = 0x8B81;
inline constexpr GLenum k_link_status = 0x8B82;
inline constexpr GLenum k_array_buffer = 0x8892;
inline constexpr GLenum k_static_draw = 0x88E4;
inline constexpr GLenum k_float = 0x1406;
inline constexpr GLenum k_unsigned_byte = 0x1401;
inline constexpr GLenum k_texture_2d = 0x0DE1;
inline constexpr GLenum k_texture0 = 0x84C0;
inline constexpr GLenum k_rgba = 0x1908;
inline constexpr GLenum k_tex_min_filter = 0x2801;
inline constexpr GLenum k_tex_mag_filter = 0x2800;
inline constexpr GLenum k_nearest = 0x2600;
inline constexpr GLenum k_tex_wrap_s = 0x2802;
inline constexpr GLenum k_tex_wrap_t = 0x2803;
inline constexpr GLenum k_clamp_to_edge = 0x812F;
inline constexpr GLenum k_unpack_alignment = 0x0CF5;
inline constexpr GLenum k_triangle_strip = 0x0005;
inline constexpr GLenum k_blend = 0x0BE2;
inline constexpr GLenum k_one = 1;
inline constexpr GLenum k_one_minus_src_alpha = 0x0303;
inline constexpr GLbitfield k_color_buffer_bit = 0x00004000;
}  // namespace gl

// ── Function-pointer typedefs ────────────────────────────────────────────
using PFN_glCreateShader = GLuint (*)(GLenum);
using PFN_glShaderSource = void (*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glCompileShader = void (*)(GLuint);
using PFN_glGetShaderiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glGetShaderInfoLog = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glCreateProgram = GLuint (*)();
using PFN_glAttachShader = void (*)(GLuint, GLuint);
using PFN_glLinkProgram = void (*)(GLuint);
using PFN_glGetProgramiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glGetProgramInfoLog = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glUseProgram = void (*)(GLuint);
using PFN_glDeleteShader = void (*)(GLuint);
using PFN_glDeleteProgram = void (*)(GLuint);
using PFN_glGenBuffers = void (*)(GLsizei, GLuint*);
using PFN_glBindBuffer = void (*)(GLenum, GLuint);
using PFN_glBufferData = void (*)(GLenum, GLsizeiptr, const void*, GLenum);
using PFN_glDeleteBuffers = void (*)(GLsizei, const GLuint*);
using PFN_glVertexAttribPointer = void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using PFN_glEnableVertexAttribArray = void (*)(GLuint);
using PFN_glGetAttribLocation = GLint (*)(GLuint, const GLchar*);
using PFN_glGenTextures = void (*)(GLsizei, GLuint*);
using PFN_glBindTexture = void (*)(GLenum, GLuint);
using PFN_glTexImage2D = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                  const void*);
using PFN_glTexParameteri = void (*)(GLenum, GLenum, GLint);
using PFN_glPixelStorei = void (*)(GLenum, GLint);
using PFN_glActiveTexture = void (*)(GLenum);
using PFN_glDeleteTextures = void (*)(GLsizei, const GLuint*);
using PFN_glGetUniformLocation = GLint (*)(GLuint, const GLchar*);
using PFN_glUniform1i = void (*)(GLint, GLint);
using PFN_glUniform1f = void (*)(GLint, GLfloat);
using PFN_glUniform4f = void (*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glViewport = void (*)(GLint, GLint, GLsizei, GLsizei);
using PFN_glClearColor = void (*)(GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glClear = void (*)(GLbitfield);
using PFN_glEnable = void (*)(GLenum);
using PFN_glDisable = void (*)(GLenum);
using PFN_glBlendFunc = void (*)(GLenum, GLenum);
using PFN_glDrawArrays = void (*)(GLenum, GLint, GLsizei);
using PFN_glFinish = void (*)();
using PFN_glGetError = GLenum (*)();

/// Resolved GLES2 entry-point table. `loaded == true` iff libGLESv2.so.2
/// dlopened and every entry point below resolved.
struct GlesLoader {
  void* handle{nullptr};
  bool loaded{false};

  PFN_glCreateShader create_shader{nullptr};
  PFN_glShaderSource shader_source{nullptr};
  PFN_glCompileShader compile_shader{nullptr};
  PFN_glGetShaderiv get_shaderiv{nullptr};
  PFN_glGetShaderInfoLog get_shader_info_log{nullptr};
  PFN_glCreateProgram create_program{nullptr};
  PFN_glAttachShader attach_shader{nullptr};
  PFN_glLinkProgram link_program{nullptr};
  PFN_glGetProgramiv get_programiv{nullptr};
  PFN_glGetProgramInfoLog get_program_info_log{nullptr};
  PFN_glUseProgram use_program{nullptr};
  PFN_glDeleteShader delete_shader{nullptr};
  PFN_glDeleteProgram delete_program{nullptr};
  PFN_glGenBuffers gen_buffers{nullptr};
  PFN_glBindBuffer bind_buffer{nullptr};
  PFN_glBufferData buffer_data{nullptr};
  PFN_glDeleteBuffers delete_buffers{nullptr};
  PFN_glVertexAttribPointer vertex_attrib_pointer{nullptr};
  PFN_glEnableVertexAttribArray enable_vertex_attrib_array{nullptr};
  PFN_glGetAttribLocation get_attrib_location{nullptr};
  PFN_glGenTextures gen_textures{nullptr};
  PFN_glBindTexture bind_texture{nullptr};
  PFN_glTexImage2D tex_image_2d{nullptr};
  PFN_glTexParameteri tex_parameteri{nullptr};
  PFN_glPixelStorei pixel_storei{nullptr};
  PFN_glActiveTexture active_texture{nullptr};
  PFN_glDeleteTextures delete_textures{nullptr};
  PFN_glGetUniformLocation get_uniform_location{nullptr};
  PFN_glUniform1i uniform1i{nullptr};
  PFN_glUniform1f uniform1f{nullptr};
  PFN_glUniform4f uniform4f{nullptr};
  PFN_glViewport viewport{nullptr};
  PFN_glClearColor clear_color{nullptr};
  PFN_glClear clear{nullptr};
  PFN_glEnable enable{nullptr};
  PFN_glDisable disable{nullptr};
  PFN_glBlendFunc blend_func{nullptr};
  PFN_glDrawArrays draw_arrays{nullptr};
  PFN_glFinish finish{nullptr};
  PFN_glGetError get_error{nullptr};
};

/// Process-singleton GLES2 runtime accessor. First call dlopens +
/// resolves under std::once_flag; later calls are zero-cost reads. If
/// libGLESv2.so.2 isn't loadable (or a symbol is missing), the returned
/// runtime has `loaded == false`.
[[nodiscard]] const GlesLoader& gles_loader() noexcept;

}  // namespace drm::detail

#endif  // DRM_CXX_HAS_EGL
