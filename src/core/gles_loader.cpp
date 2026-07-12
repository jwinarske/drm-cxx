// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "gles_loader.hpp"

#if DRM_CXX_HAS_EGL

#include "egl_loader.hpp"

#include <drm-cxx/log.hpp>

#include <dlfcn.h>
#include <mutex>

namespace drm::detail {

namespace {

// Resolve a GLES2 entry point: try dlsym on libGLESv2 first, then fall back to
// eglGetProcAddress (some glvnd stacks only expose GLES core via the EGL
// dispatcher). Returns nullptr if neither resolves it.
template <typename Fn>
Fn resolve_gl(void* handle, const char* name) noexcept {
  if (void* sym = ::dlsym(handle, name); sym != nullptr) {
    return reinterpret_cast<Fn>(sym);
  }
  const auto& egl = egl_loader();
  if (egl.loaded && (egl.get_proc_address != nullptr)) {
    return reinterpret_cast<Fn>(egl.get_proc_address(name));
  }
  return nullptr;
}

void initialize_runtime(GlesLoader& rt) noexcept {
  rt.handle = ::dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
  if (rt.handle == nullptr) {
    drm::log_info("gles_loader: libGLESv2.so.2 not loadable ({}) — GPU composition unavailable",
                  ::dlerror() != nullptr ? ::dlerror() : "no error reported");
    return;
  }

  rt.create_shader = resolve_gl<decltype(rt.create_shader)>(rt.handle, "glCreateShader");
  rt.shader_source = resolve_gl<decltype(rt.shader_source)>(rt.handle, "glShaderSource");
  rt.compile_shader = resolve_gl<decltype(rt.compile_shader)>(rt.handle, "glCompileShader");
  rt.get_shaderiv = resolve_gl<decltype(rt.get_shaderiv)>(rt.handle, "glGetShaderiv");
  rt.get_shader_info_log =
      resolve_gl<decltype(rt.get_shader_info_log)>(rt.handle, "glGetShaderInfoLog");
  rt.create_program = resolve_gl<decltype(rt.create_program)>(rt.handle, "glCreateProgram");
  rt.attach_shader = resolve_gl<decltype(rt.attach_shader)>(rt.handle, "glAttachShader");
  rt.link_program = resolve_gl<decltype(rt.link_program)>(rt.handle, "glLinkProgram");
  rt.get_programiv = resolve_gl<decltype(rt.get_programiv)>(rt.handle, "glGetProgramiv");
  rt.get_program_info_log =
      resolve_gl<decltype(rt.get_program_info_log)>(rt.handle, "glGetProgramInfoLog");
  rt.use_program = resolve_gl<decltype(rt.use_program)>(rt.handle, "glUseProgram");
  rt.delete_shader = resolve_gl<decltype(rt.delete_shader)>(rt.handle, "glDeleteShader");
  rt.delete_program = resolve_gl<decltype(rt.delete_program)>(rt.handle, "glDeleteProgram");
  rt.gen_buffers = resolve_gl<decltype(rt.gen_buffers)>(rt.handle, "glGenBuffers");
  rt.bind_buffer = resolve_gl<decltype(rt.bind_buffer)>(rt.handle, "glBindBuffer");
  rt.buffer_data = resolve_gl<decltype(rt.buffer_data)>(rt.handle, "glBufferData");
  rt.delete_buffers = resolve_gl<decltype(rt.delete_buffers)>(rt.handle, "glDeleteBuffers");
  rt.vertex_attrib_pointer =
      resolve_gl<decltype(rt.vertex_attrib_pointer)>(rt.handle, "glVertexAttribPointer");
  rt.enable_vertex_attrib_array =
      resolve_gl<decltype(rt.enable_vertex_attrib_array)>(rt.handle, "glEnableVertexAttribArray");
  rt.get_attrib_location =
      resolve_gl<decltype(rt.get_attrib_location)>(rt.handle, "glGetAttribLocation");
  rt.gen_textures = resolve_gl<decltype(rt.gen_textures)>(rt.handle, "glGenTextures");
  rt.bind_texture = resolve_gl<decltype(rt.bind_texture)>(rt.handle, "glBindTexture");
  rt.tex_image_2d = resolve_gl<decltype(rt.tex_image_2d)>(rt.handle, "glTexImage2D");
  rt.tex_parameteri = resolve_gl<decltype(rt.tex_parameteri)>(rt.handle, "glTexParameteri");
  rt.pixel_storei = resolve_gl<decltype(rt.pixel_storei)>(rt.handle, "glPixelStorei");
  rt.active_texture = resolve_gl<decltype(rt.active_texture)>(rt.handle, "glActiveTexture");
  rt.delete_textures = resolve_gl<decltype(rt.delete_textures)>(rt.handle, "glDeleteTextures");
  rt.get_uniform_location =
      resolve_gl<decltype(rt.get_uniform_location)>(rt.handle, "glGetUniformLocation");
  rt.uniform1i = resolve_gl<decltype(rt.uniform1i)>(rt.handle, "glUniform1i");
  rt.uniform1f = resolve_gl<decltype(rt.uniform1f)>(rt.handle, "glUniform1f");
  rt.uniform4f = resolve_gl<decltype(rt.uniform4f)>(rt.handle, "glUniform4f");
  rt.viewport = resolve_gl<decltype(rt.viewport)>(rt.handle, "glViewport");
  rt.clear_color = resolve_gl<decltype(rt.clear_color)>(rt.handle, "glClearColor");
  rt.clear = resolve_gl<decltype(rt.clear)>(rt.handle, "glClear");
  rt.enable = resolve_gl<decltype(rt.enable)>(rt.handle, "glEnable");
  rt.disable = resolve_gl<decltype(rt.disable)>(rt.handle, "glDisable");
  rt.blend_func = resolve_gl<decltype(rt.blend_func)>(rt.handle, "glBlendFunc");
  rt.draw_arrays = resolve_gl<decltype(rt.draw_arrays)>(rt.handle, "glDrawArrays");
  rt.finish = resolve_gl<decltype(rt.finish)>(rt.handle, "glFinish");
  rt.get_error = resolve_gl<decltype(rt.get_error)>(rt.handle, "glGetError");
  rt.get_string = resolve_gl<decltype(rt.get_string)>(rt.handle, "glGetString");
  // Optional OES extension for the EGLImage-dmabuf composition path.
  // Deliberately NOT part of all_present below — a stack without
  // GL_OES_EGL_image still runs the CPU-upload compositor.
  rt.egl_image_target_texture_2d = resolve_gl<decltype(rt.egl_image_target_texture_2d)>(
      rt.handle, "glEGLImageTargetTexture2DOES");

  // All entry points are mandatory for the compositor; if any is missing treat
  // the whole stack as unusable so the caller falls back to the CPU path.
  const bool all_present =
      (rt.create_shader != nullptr) && (rt.shader_source != nullptr) &&
      (rt.compile_shader != nullptr) && (rt.get_shaderiv != nullptr) &&
      (rt.get_shader_info_log != nullptr) && (rt.create_program != nullptr) &&
      (rt.attach_shader != nullptr) && (rt.link_program != nullptr) &&
      (rt.get_programiv != nullptr) && (rt.get_program_info_log != nullptr) &&
      (rt.use_program != nullptr) && (rt.delete_shader != nullptr) &&
      (rt.delete_program != nullptr) && (rt.gen_buffers != nullptr) &&
      (rt.bind_buffer != nullptr) && (rt.buffer_data != nullptr) &&
      (rt.delete_buffers != nullptr) && (rt.vertex_attrib_pointer != nullptr) &&
      (rt.enable_vertex_attrib_array != nullptr) && (rt.get_attrib_location != nullptr) &&
      (rt.gen_textures != nullptr) && (rt.bind_texture != nullptr) &&
      (rt.tex_image_2d != nullptr) && (rt.tex_parameteri != nullptr) &&
      (rt.pixel_storei != nullptr) && (rt.active_texture != nullptr) &&
      (rt.delete_textures != nullptr) && (rt.get_uniform_location != nullptr) &&
      (rt.uniform1i != nullptr) && (rt.uniform1f != nullptr) && (rt.uniform4f != nullptr) &&
      (rt.viewport != nullptr) && (rt.clear_color != nullptr) && (rt.clear != nullptr) &&
      (rt.enable != nullptr) && (rt.disable != nullptr) && (rt.blend_func != nullptr) &&
      (rt.draw_arrays != nullptr) && (rt.finish != nullptr) && (rt.get_error != nullptr) &&
      (rt.get_string != nullptr);
  if (!all_present) {
    drm::log_warn("gles_loader: libGLESv2.so.2 loaded but missing entry points — unsupported");
    rt.handle = nullptr;
    return;
  }
  rt.loaded = true;
}

}  // namespace

const GlesLoader& gles_loader() noexcept {
  static GlesLoader rt;
  static std::once_flag once;
  std::call_once(once, [] { initialize_runtime(rt); });
  return rt;
}

}  // namespace drm::detail

#endif  // DRM_CXX_HAS_EGL
