# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# cluster_sim_vulkan — instrument-cluster sibling of cluster_sim with
# a Vulkan-rendered background DMA-BUF. Gated on both Vulkan
# (VK_EXT_image_drm_format_modifier / VK_EXT_external_memory_dma_buf)
# and Blend2D (the CPU instruments layer still uses Blend2D for the
# dial/info/warn paint while the Vulkan side moves to a render pass).
#
# Shaders are compiled at configure-with-build time by glslangValidator
# into SPIR-V C-array headers (`--vn <name>`) and embedded via #include
# in main.cpp. We require glslangValidator because vulkan_scene proved
# the dma-buf path is OK but a render pass needs real shader modules,
# and on this Tegra L4T host glslangValidator is what's available
# (no shaderc/glslc). When glslangValidator is missing, skip the build
# rather than fail — same pattern other example gates use.

if not get_option('vulkan')
  subdir_done()
endif

if not has_blend2d
  subdir_done()
endif

glslang_validator = find_program('glslangValidator', required : false)
if not glslang_validator.found()
  subdir_done()
endif

# Compile each shader to a SPIR-V uint32_t array header. `--vn <name>`
# names the constant array; the header is `#pragma once` plus a single
# `const uint32_t <name>[] = {...};` declaration. main.cpp #includes
# the generated file directly from the build dir.
bg_vert_spv = custom_target('cluster_sim_vulkan_bg_vert_spv',
  input : 'shaders/bg.vert',
  output : 'bg_vert_spv.h',
  command : [glslang_validator, '-V', '--vn', 'k_bg_vert_spv',
             '@INPUT@', '-o', '@OUTPUT@'],
)
bg_frag_spv = custom_target('cluster_sim_vulkan_bg_frag_spv',
  input : 'shaders/bg.frag',
  output : 'bg_frag_spv.h',
  command : [glslang_validator, '-V', '--vn', 'k_bg_frag_spv',
             '@INPUT@', '-o', '@OUTPUT@'],
)
textured_vert_spv = custom_target('cluster_sim_vulkan_textured_vert_spv',
  input : 'shaders/textured.vert',
  output : 'textured_vert_spv.h',
  command : [glslang_validator, '-V', '--vn', 'k_textured_vert_spv',
             '@INPUT@', '-o', '@OUTPUT@'],
)
textured_frag_spv = custom_target('cluster_sim_vulkan_textured_frag_spv',
  input : 'shaders/textured.frag',
  output : 'textured_frag_spv.h',
  command : [glslang_validator, '-V', '--vn', 'k_textured_frag_spv',
             '@INPUT@', '-o', '@OUTPUT@'],
)
needle_vert_spv = custom_target('cluster_sim_vulkan_needle_vert_spv',
  input : 'shaders/needle.vert',
  output : 'needle_vert_spv.h',
  command : [glslang_validator, '-V', '--vn', 'k_needle_vert_spv',
             '@INPUT@', '-o', '@OUTPUT@'],
)
needle_frag_spv = custom_target('cluster_sim_vulkan_needle_frag_spv',
  input : 'shaders/needle.frag',
  output : 'needle_frag_spv.h',
  command : [glslang_validator, '-V', '--vn', 'k_needle_frag_spv',
             '@INPUT@', '-o', '@OUTPUT@'],
)
tex_array_vert_spv = custom_target('cluster_sim_vulkan_tex_array_vert_spv',
  input : 'shaders/tex_array.vert',
  output : 'tex_array_vert_spv.h',
  command : [glslang_validator, '-V', '--vn', 'k_tex_array_vert_spv',
             '@INPUT@', '-o', '@OUTPUT@'],
)
tex_array_frag_spv = custom_target('cluster_sim_vulkan_tex_array_frag_spv',
  input : 'shaders/tex_array.frag',
  output : 'tex_array_frag_spv.h',
  command : [glslang_validator, '-V', '--vn', 'k_tex_array_frag_spv',
             '@INPUT@', '-o', '@OUTPUT@'],
)
dial_fx_frag_spv = custom_target('cluster_sim_vulkan_dial_fx_frag_spv',
  input : 'shaders/dial_fx.frag',
  output : 'dial_fx_frag_spv.h',
  command : [glslang_validator, '-V', '--vn', 'k_dial_fx_frag_spv',
             '@INPUT@', '-o', '@OUTPUT@'],
)

executable('cluster_sim_vulkan',
  'main.cpp',
  bg_vert_spv, bg_frag_spv, textured_vert_spv, textured_frag_spv,
  needle_vert_spv, needle_frag_spv,
  tex_array_vert_spv, tex_array_frag_spv,
  dial_fx_frag_spv,
  dependencies : [drm_cxx_dep, vk_dep, blend2d_dep],
  include_directories : [include_directories('..'),
                         include_directories('.')],
)
