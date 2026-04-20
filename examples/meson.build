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

executable('mouse_cursor',
  'mouse_cursor/main.cpp',
  dependencies : [drm_cxx_dep],
)

# XCursor loader + smoke test for the mouse_cursor example.
# mouse_cursor itself picks up the dep in a later step of xcursor_plan.md.
xcursor_dep = dependency('xcursor', required : true)

xcursor_smoke_args = []
if cpp.has_member('XcursorImage', 'delay',
    prefix : '#include <X11/Xcursor/Xcursor.h>',
    dependencies : xcursor_dep)
  xcursor_smoke_args += '-DHAS_XCURSOR_DELAY'
endif

executable('xcursor_smoke',
  ['mouse_cursor/xcursor_smoke.cpp', 'mouse_cursor/xcursor_loader.cpp'],
  dependencies : xcursor_dep,
  cpp_args : xcursor_smoke_args,
)

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