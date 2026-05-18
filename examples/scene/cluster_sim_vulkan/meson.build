# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# cluster_sim_vulkan — instrument-cluster sibling of cluster_sim with
# a Vulkan-rendered background DMA-BUF. Gated on both Vulkan
# (vkCmdClearColorImage / VK_EXT_image_drm_format_modifier /
# VK_EXT_external_memory_dma_buf) and Blend2D (the instruments layer
# paint reuses cluster_sim's Blend2D path).

if not get_option('vulkan')
  subdir_done()
endif

if not has_blend2d
  subdir_done()
endif

executable('cluster_sim_vulkan',
  'main.cpp',
  dependencies : [drm_cxx_dep, vk_dep, blend2d_dep],
  include_directories : include_directories('..'),
)
