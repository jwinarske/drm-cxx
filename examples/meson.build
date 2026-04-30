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

# scene_warm_start — pedagogical demo for CommitReport::test_commits_issued.
# Pure consumer of the existing scene API, no extra deps.
executable('scene_warm_start',
  'scene_warm_start/main.cpp',
  dependencies : [drm_cxx_dep],
)

# scene_priority — pedagogical demo for content-type / update-hint-driven
# priority eviction. Eight layers, typical 3-plane budget, no extra deps.
executable('scene_priority',
  'scene_priority/main.cpp',
  dependencies : [drm_cxx_dep],
)

# scene_formats — pedagogical demo for bipartite plane-matching across
# heterogeneous layer requirements (channel orderings + scaler need).
# Pure consumer of the existing scene API, no extra deps.
executable('scene_formats',
  'scene_formats/main.cpp',
  dependencies : [drm_cxx_dep],
)

# layered_demo — interactive LayerScene mutation tour. Up to eight
# coloured tiles toggled, moved, restacked, alpha-adjusted from the
# keyboard against a static gradient background.
executable('layered_demo',
  'layered_demo/main.cpp',
  dependencies : [drm_cxx_dep],
)

# video_grid — N×N grid of synthesised "video" cells. Stresses the
# allocator + composition fallback when cell count exceeds the plane
# budget; the runtime layout switch (1/2/3) walks add/remove churn.
executable('video_grid',
  'video_grid/main.cpp',
  dependencies : [drm_cxx_dep],
)

# camera — zero-copy libcamera -> KMS scanout viewfinder (planned).
# Today this is the --probe scaffold from docs/cam_example_plan.md:
# device + output + plane IN_FORMATS dump, no libcamera dep yet.
executable('camera',
  'camera/main.cpp',
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
  # libvulkan is reached only through dlsym(RTLD_DEFAULT, ...) inside
  # drm::vulkan::Display, so the linker's --as-needed pass strips it
  # even though it sits on the link line. Disable as-needed for this
  # target so libvulkan ends up DT_NEEDED (matches the CMake build).
  executable('vulkan_display',
    'vulkan_display/main.cpp',
    dependencies : [drm_cxx_dep],
    override_options : ['b_asneeded=false'],
  )
endif

subdir('signage_player')
subdir('test_patterns')

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