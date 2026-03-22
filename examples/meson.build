# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

executable('overlay_planes',
  'overlay_planes/main.cpp',
  dependencies : drm_cxx_dep,
)

executable('atomic_modeset',
  'atomic_modeset/main.cpp',
  dependencies : drm_cxx_dep,
)

executable('hotplug_monitor',
  'hotplug_monitor/main.cpp',
  dependencies : drm_cxx_dep,
)

executable('mouse_cursor',
  'mouse_cursor/main.cpp',
  dependencies : drm_cxx_dep,
)

if get_option('vulkan')
  executable('vulkan_display',
    'vulkan_display/main.cpp',
    dependencies : drm_cxx_dep,
  )
endif
