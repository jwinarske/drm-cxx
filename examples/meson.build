# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# Examples are organised into four buckets matching docs/scene.md:
#
#   basics/    — minimal LayerScene introductions and small one-feature demos.
#   scene/     — substantial LayerScene workloads (the "sales deck" tier).
#   allocator/ — pedagogical demos for individual planes::Allocator features.
#   advanced/  — demos that exercise non-scene modules (Vulkan, capture, cursor).
#
# Common helpers (open_output, format_probe, vt_switch, select_*) live under
# examples/common/ and are pulled in by sources via "../../common/foo.hpp".

#
# basics/
#

executable('atomic_modeset',
  'basics/atomic_modeset/main.cpp',
  dependencies : [drm_cxx_dep],
)

executable('hotplug_monitor',
  'basics/hotplug_monitor/main.cpp',
  dependencies : [drm_cxx_dep],
)

# mouse_cursor + xcursor_smoke both exercise the library's drm::cursor
# module — no direct libxcursor dep needed here, the library owns that.
executable('mouse_cursor',
  'basics/mouse_cursor/main.cpp',
  dependencies : [drm_cxx_dep],
)

executable('xcursor_smoke',
  'basics/mouse_cursor/xcursor_smoke.cpp',
  dependencies : [drm_cxx_dep],
)

subdir('basics/test_patterns')

# keyboard — interactive keyboard-input + Unicode-entry demo. Default
# disabled because the build downloads Noto Sans at configure time,
# which doesn't work in sandboxed environments (Yocto, OE, distro
# packagers). Pass `-Dkeyboard=enabled` when network is available and
# blend2d is present.
keyboard_feature = get_option('keyboard')
if not keyboard_feature.disabled()
  if not has_blend2d
    if keyboard_feature.enabled()
      error('keyboard demo needs blend2d; either enable -Dblend2d=enabled or set -Dkeyboard=disabled')
    endif
  else
    subdir('basics/keyboard')
  endif
endif

#
# scene/
#

# layered_demo — interactive LayerScene mutation tour. Up to eight
# colored tiles toggled, moved, restacked, alpha-adjusted from the
# keyboard against a static gradient background.
executable('layered_demo',
  'scene/layered_demo/main.cpp',
  dependencies : [drm_cxx_dep],
)

# video_grid — N×N grid of synthesised "video" cells. Stresses the
# allocator + composition fallback when cell count exceeds the plane
# budget; the runtime layout switch (1/2/3) walks add/remove churn.
executable('video_grid',
  'scene/video_grid/main.cpp',
  dependencies : [drm_cxx_dep],
)

# camera — zero-copy libcamera -> KMS scanout viewfinder, with libyuv
# fallbacks for cameras whose stride / fourcc can't be scanned out
# directly (YUY2 -> NV12 repack, MJPEG -> NV12 decode-and-repack).
# Both libcamera and libyuv are example-only deps; auto-feature so a
# missing dep skips the target rather than failing the whole build
# (Ubuntu 24.04 ships libcamera 0.2.0; 0.3+ needs source/PPA).
# include_type: 'system' pushes their public headers behind -isystem
# so their warnings stay out of -Werror.
camera_feature = get_option('camera')
if not camera_feature.disabled()
  libcamera_dep = dependency('libcamera', include_type : 'system',
                             required : camera_feature.enabled(),
                             version : '>=0.3.0')
  # libyuv via pkg-config is unreliable: Fedora's libyuv-devel ships a
  # libyuv.pc but Ubuntu/Debian's libyuv-dev does not. Skip dependency()
  # entirely and use cxx.find_library + has_headers, which works on
  # both distros.
  cxx = meson.get_compiler('cpp')
  libyuv_dep = cxx.find_library('yuv',
                                has_headers : ['libyuv.h'],
                                required : camera_feature.enabled())
  if libcamera_dep.found() and libyuv_dep.found()
    camera_deps = [drm_cxx_dep, libcamera_dep, libyuv_dep]
    camera_args = []
    if has_blend2d
      camera_deps += blend2d_dep
      camera_args += '-DCAMERA_STATUS_HAS_BLEND2D=1'
    endif
    executable('camera',
      'scene/camera/main.cpp',
      'scene/camera/convert.cpp',
      'scene/camera/status_overlay_renderer.cpp',
      dependencies : camera_deps,
      cpp_args : camera_args,
    )
  else
    message('camera: libcamera>=0.3.0 or libyuv not found, skipping (pass -Dcamera=enabled to hard-fail)')
  endif
endif

subdir('scene/signage_player')
subdir('scene/video_player')
subdir('scene/cluster_sim')

# ThorVG Janitor example — scaffold only (see scene/thorvg_janitor/README.md).
# Upstream ThorVG is auto-detected; set -Dthorvg_janitor=disabled to skip the
# probe entirely, or -Dthorvg_janitor=enabled to hard-require ThorVG.
thorvg_feature = get_option('thorvg_janitor')
if not thorvg_feature.disabled()
  thorvg_dep = dependency('thorvg', required : thorvg_feature.enabled(),
                          version : '>=1.0.4')
  if thorvg_dep.found()
    subdir('scene/thorvg_janitor')
  else
    message('thorvg_janitor: thorvg not found, skipping (pass -Dthorvg_janitor=enabled to hard-fail)')
  endif
endif

#
# allocator/
#

executable('overlay_planes',
  'allocator/overlay_planes/main.cpp',
  dependencies : [drm_cxx_dep],
)

# scene_warm_start — pedagogical demo for CommitReport::test_commits_issued.
# Pure consumer of the existing scene API, no extra deps.
executable('scene_warm_start',
  'allocator/scene_warm_start/main.cpp',
  dependencies : [drm_cxx_dep],
)

# scene_priority — pedagogical demo for content-type / update-hint-driven
# priority eviction. Eight layers, typical 3-plane budget, no extra deps.
executable('scene_priority',
  'allocator/scene_priority/main.cpp',
  dependencies : [drm_cxx_dep],
)

# scene_formats — pedagogical demo for bipartite plane-matching across
# heterogeneous layer requirements (channel orderings + scaler need).
# Pure consumer of the existing scene API, no extra deps.
executable('scene_formats',
  'allocator/scene_formats/main.cpp',
  dependencies : [drm_cxx_dep],
)

#
# advanced/
#

# cursor_rotate — V2 hardware-validation harness for drm::cursor.
# Exercises the theme resolve cache, HOTSPOT_X/Y writes, shared Cursor
# across multi-CRTC Renderers, set_rotation(), and software pre-
# rotation. Pure library consumer — no direct libxcursor dep.
executable('cursor_rotate',
  'advanced/cursor_rotate/main.cpp',
  dependencies : [drm_cxx_dep],
)

# capture_demo — libinput-driven snapshot demo for drm::capture. Gated
# on blend2d since it #includes drm-cxx/capture/*.hpp.
if has_blend2d
  executable('capture_demo',
    'advanced/capture_demo/main.cpp',
    dependencies : [drm_cxx_dep],
  )

  # csd_smoke — throwaway hardware-validation harness for drm::csd.
  # Paints one glass decoration via csd::Renderer on top of a gradient
  # background and arms the resulting composition for a few seconds.
  executable('csd_smoke',
    'advanced/csd_smoke/main.cpp',
    dependencies : [drm_cxx_dep],
  )

  # mdi_demo — multi-document desktop showcase for drm::csd Tier 0.
  subdir('mdi_demo')
endif

if get_option('vulkan')
  # libvulkan is reached only through dlsym(RTLD_DEFAULT, ...) inside
  # drm::vulkan::Display, so the linker's --as-needed pass strips it
  # even though it sits on the link line. Disable as-needed for this
  # target so libvulkan ends up DT_NEEDED (matches the CMake build).
  executable('vulkan_display',
    'advanced/vulkan_display/main.cpp',
    dependencies : [drm_cxx_dep],
    override_options : ['b_asneeded=false'],
  )
endif