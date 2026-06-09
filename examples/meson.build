# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# Examples are organized into four buckets matching docs/scene.md:
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

# video_grid — N×N grid of synthesized "video" cells. Stresses the
# allocator + composition fallback when cell count exceeds the plane
# budget; the runtime layout switch (1/2/3) walks add/remove churn.
executable('video_grid',
  'scene/video_grid/main.cpp',
  dependencies : [drm_cxx_dep],
)

# dual_display — minimal multi-CRTC SceneSet demo. Builds one
# LayerScene per connected output and lays a single shared mirrored
# layer across all of them via SceneSet::add_layer; every frame is one
# combined cross-CRTC atomic commit. Needs >=2 connected outputs (real
# hardware or scripts/vkms_dual.sh up).
executable('dual_display',
  'scene/dual_display/main.cpp',
  dependencies : [drm_cxx_dep],
)

# video_wall_multi — N×N grid of synthesized "video" cells laid out
# across every connected output on one card as one logical wall.
# Straddling cells register two-target SceneSetLayerSpec entries on a
# shared DumbBufferSource. Stresses the per-CRTC allocator and the
# composition fallback alongside the cross-CRTC commit path.
executable('video_wall_multi',
  'scene/video_wall_multi/main.cpp',
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
    camera_sources = [
      'scene/camera/main.cpp',
      'scene/camera/convert.cpp',
      'scene/camera/double_dumb_source.cpp',
      'scene/camera/libcamera_nv12_source.cpp',
      'scene/camera/status_overlay_renderer.cpp',
    ]
    if has_blend2d
      camera_deps += blend2d_dep
      camera_args += '-DCAMERA_STATUS_HAS_BLEND2D=1'
    endif
    # VAAPI MJPEG decode (auto-feature). libva + libva-drm together
    # unlock zero-copy JPEG -> NV12 -> scanout on AMD/Intel; missing
    # either falls the example back to the CPU libyuv path.
    libva_dep = dependency('libva', include_type : 'system', required : false)
    libva_drm_dep = dependency('libva-drm', include_type : 'system', required : false)
    if libva_dep.found() and libva_drm_dep.found()
      camera_deps += [libva_dep, libva_drm_dep]
      camera_sources += 'scene/camera/vaapi_jpeg_decoder.cpp'
      camera_args += '-DCAMERA_HAS_VAAPI=1'
    else
      message('camera: libva / libva-drm not found, MJPEG falls back to CPU libyuv decode')
    endif
    executable('camera',
      camera_sources,
      dependencies : camera_deps,
      cpp_args : camera_args,
    )
  else
    message('camera: libcamera>=0.3.0 or libyuv not found, skipping (pass -Dcamera=enabled to hard-fail)')
  endif
endif

subdir('scene/signage_player')
subdir('scene/v4l2_camera_demo')
subdir('scene/video_player')
subdir('scene/cluster_sim')
subdir('scene/cluster_sim_vulkan')
subdir('scene/hdr_demo')

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

# damage_present — FB_DAMAGE_CLIPS validation: a moving box reports only its
# touched regions as per-frame damage (DumbBufferSource::set_damage).
executable('damage_present',
  'advanced/damage_present/main.cpp',
  dependencies : [drm_cxx_dep],
)

# stream_probe — diagnostic CLI for drm::scene::probe_stream_capability.
# Builds on every host independent of the streams= option; the probe
# itself short-circuits to Unsupported on builds without EGL headers
# so the CLI is informative in either configuration.
executable('stream_probe',
  'advanced/stream_probe/main.cpp',
  dependencies : [drm_cxx_dep],
)

# multi_crtc_probe — enumerate connected outputs on a card and report
# whether a combined-CRTC TEST_ONLY atomic commit is accepted by the
# kernel. Foundational probe for SceneSet-style multi-display work
# (cluster_sim's passenger display, dual_display, video_wall_multi).
# `--hotplug` re-runs the probe on each udev event so the verdict
# tracks topology changes.
executable('multi_crtc_probe',
  'advanced/multi_crtc_probe/main.cpp',
  dependencies : [drm_cxx_dep],
)

# stream_demo — end-to-end EGL Streams demo. Gated on the streams
# option (the EglStreamBuilder header isn't defined without it) AND
# on glesv2.pc availability (the demo links real EGL + GLES). The
# library itself never links these — only the demo does, since it
# drives the producer side directly.
if has_egl_streams
  glesv2_dep = dependency('glesv2', required : false)
  if glesv2_dep.found() and egl_dep.found()
    executable('stream_demo',
      'advanced/stream_demo/main.cpp',
      dependencies : [drm_cxx_dep, egl_dep, glesv2_dep],
    )
  endif
endif

# egl_scene — minimal EGL/GLES 3 producer fronting a GbmSurfaceSource.
# Gated on libEGL + glesv2 like stream_demo; the library itself doesn't
# link them.
egl_scene_egl_dep = dependency('egl', required : false)
egl_scene_gles_dep = dependency('glesv2', required : false)
if egl_scene_egl_dep.found() and egl_scene_gles_dep.found()
  executable('egl_scene',
    'advanced/egl_scene/main.cpp',
    dependencies : [drm_cxx_dep, egl_scene_egl_dep, egl_scene_gles_dep],
  )
  # gl_present — GlScanoutProducer demo (EGL/GLES render -> ScanoutBackend).
  executable('gl_present',
    'advanced/gl_present/main.cpp',
    dependencies : [drm_cxx_dep, egl_scene_egl_dep, egl_scene_gles_dep],
  )
endif

# vk_present — VkScanoutProducer demo. The producer encapsulates Vulkan
# (vulkan.hpp dlopen'd at runtime inside the library), so this links only
# drm-cxx — no libvulkan, no GL. Gated on Vulkan headers being present.
if has_vulkan_headers
  executable('vk_present',
    'advanced/vk_present/main.cpp',
    dependencies : [drm_cxx_dep],
  )
  # vk_out_fence — validate OUT_FENCE (scanout-done sync_file), counterpart to
  # vk_present's IN_FENCE demo.
  executable('vk_out_fence',
    'advanced/vk_out_fence/main.cpp',
    dependencies : [drm_cxx_dep],
  )
endif

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

  # vulkan_scene — minimal Vulkan-rendered scene layer over
  # VK_EXT_image_drm_format_modifier + ExternalDmaBufSource. Links
  # libvulkan directly (no dlsym shenanigans here).
  executable('vulkan_scene',
    'advanced/vulkan_scene/main.cpp',
    dependencies : [drm_cxx_dep, vk_dep],
  )
endif

#
# format-modifier / hardware-compression examples (drm::fmt, src/fmt).
#
# plane_caps + compressed_scanout are pure drm-cxx consumers (drm + gbm
# ride in transitively via drm_cxx_dep). The two render-offload demos add
# an EGL/GLES or Vulkan producer and gate on those deps exactly like
# egl_scene / vulkan_scene above; the library itself never links them.
#

executable('plane_caps',
  'allocator/plane_caps/main.cpp',
  dependencies : [drm_cxx_dep],
)

executable('compressed_scanout',
  'allocator/compressed_scanout/main.cpp',
  dependencies : [drm_cxx_dep],
)

offload_egl_dep = dependency('egl', required : false)
offload_gles_dep = dependency('glesv2', required : false)
if offload_egl_dep.found() and offload_gles_dep.found()
  executable('egl_offload_scanout',
    'advanced/egl_offload_scanout/main.cpp',
    dependencies : [drm_cxx_dep, offload_egl_dep, offload_gles_dep],
  )
  # Compositor-path counterpart: a gbm_surface swapchain, which is the only way
  # Mesa hands out displayable-DCC (retile) on amdgpu. Same EGL/GLES gating.
  executable('gbm_surface_scanout',
    'advanced/gbm_surface_scanout/main.cpp',
    dependencies : [drm_cxx_dep, offload_egl_dep, offload_gles_dep],
  )
endif

if get_option('vulkan')
  executable('vulkan_offload_scanout',
    'advanced/vulkan_offload_scanout/main.cpp',
    dependencies : [drm_cxx_dep, vk_dep],
  )
endif