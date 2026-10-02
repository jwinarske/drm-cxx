# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# cluster_sim_vulkan — instrument-cluster sibling of cluster_sim, rendered
# through Vulkan (drm::present::VkScanoutProducer). Mirrors meson.build: gated
# on Vulkan, Blend2D (the dial / readout templates are painted with it) and
# glslangValidator, which compiles the shaders at build time into SPIR-V
# C-array headers (`--vn <name>`) that main.cpp #includes. Skipped, not
# failed, when any of the three is missing.

if(NOT DRM_CXX_VULKAN OR NOT DRM_CXX_HAS_BLEND2D)
  return()
endif()

find_program(DRM_CXX_GLSLANG_VALIDATOR glslangValidator)
if(NOT DRM_CXX_GLSLANG_VALIDATOR)
  message(STATUS "cluster_sim_vulkan: glslangValidator not found, skipping")
  return()
endif()

# <shader file> <array name> pairs; each becomes <stem>_<stage>_spv.h.
set(_csv_shaders
  bg.vert        k_bg_vert_spv
  bg.frag        k_bg_frag_spv
  textured.vert  k_textured_vert_spv
  textured.frag  k_textured_frag_spv
  needle.vert    k_needle_vert_spv
  needle.frag    k_needle_frag_spv
  tex_array.vert k_tex_array_vert_spv
  tex_array.frag k_tex_array_frag_spv
  dial_fx.frag   k_dial_fx_frag_spv)

set(_csv_spv_headers)
list(LENGTH _csv_shaders _csv_len)
math(EXPR _csv_last "${_csv_len} - 1")
foreach(_i RANGE 0 ${_csv_last} 2)
  math(EXPR _j "${_i} + 1")
  list(GET _csv_shaders ${_i} _shader)
  list(GET _csv_shaders ${_j} _var)
  string(REPLACE "." "_" _stem "${_shader}")
  set(_out ${CMAKE_CURRENT_BINARY_DIR}/${_stem}_spv.h)
  add_custom_command(
    OUTPUT ${_out}
    COMMAND ${DRM_CXX_GLSLANG_VALIDATOR} -V --vn ${_var}
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader} -o ${_out}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader}
    COMMENT "glslangValidator ${_shader}"
    VERBATIM)
  list(APPEND _csv_spv_headers ${_out})
endforeach()

add_executable(cluster_sim_vulkan main.cpp ${_csv_spv_headers})
# The Vulkan loader is dlopen'd (vulkan.hpp dynamic dispatch) and
# common/quit_signal.hpp starts a thread; before glibc 2.34 the dl* and
# pthread_* symbols live in libdl / libpthread.
target_link_libraries(cluster_sim_vulkan PRIVATE
  drm-cxx::drm-cxx
  blend2d::blend2d
  Threads::Threads
  ${CMAKE_DL_LIBS})
target_include_directories(cluster_sim_vulkan PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(cluster_sim_vulkan SYSTEM PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/Vulkan-Headers/include)
