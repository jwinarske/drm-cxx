# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

executable('overlay_planes',
  'overlay_planes/main.cpp',
  dependencies : [drm_cxx_dep],
)

executable('atomic_modeset',
  'atomic_modeset/main.cpp',
  dependencies : [drm_cxx_dep],
)

executable('hotplug_monitor',
  'hotplug_monitor/main.cpp',
  dependencies : [drm_cxx_dep],
)

# mouse_cursor + xcursor_smoke both exercise the library's drm::cursor
# module — no direct libxcursor dep needed here, the library owns that.
executable('mouse_cursor',
  'mouse_cursor/main.cpp',
  dependencies : [drm_cxx_dep],
)

executable('xcursor_smoke',
  'mouse_cursor/xcursor_smoke.cpp',
  dependencies : [drm_cxx_dep],
)

# cursor_rotate — V2 hardware-validation harness for drm::cursor.
# Exercises the theme resolve cache, HOTSPOT_X/Y writes, shared Cursor
# across multi-CRTC Renderers, set_rotation(), and software pre-
# rotation. Pure library consumer — no direct libxcursor dep.
executable('cursor_rotate',
  'cursor_rotate/main.cpp',
  dependencies : [drm_cxx_dep],
)

# capture_demo — libinput-driven snapshot demo for drm::capture. Gated
# on blend2d since it #includes drm-cxx/capture/*.hpp.
if has_blend2d
  executable('capture_demo',
    'capture_demo/main.cpp',
    dependencies : [drm_cxx_dep],
  )
endif

if get_option('vulkan')
  executable('vulkan_display',
    'vulkan_display/main.cpp',
    dependencies : [drm_cxx_dep],
  )
endif

# ThorVG Janitor example — scaffold only (see examples/thorvg_janitor/README.md).
# Upstream ThorVG is auto-detected; set -Dthorvg_janitor=disabled to skip the
# probe entirely, or -Dthorvg_janitor=enabled to hard-require ThorVG.
thorvg_feature = get_option('thorvg_janitor')
if not thorvg_feature.disabled()
  thorvg_dep = dependency('thorvg', required : thorvg_feature.enabled(),
                          version : '>=1.0.4')
  if thorvg_dep.found()
    subdir('thorvg_janitor')
  else
    message('thorvg_janitor: thorvg not found, skipping (pass -Dthorvg_janitor=enabled to hard-fail)')
  endif
endif