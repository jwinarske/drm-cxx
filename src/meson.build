# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

drm_cxx_sources = files(
  'core/device.cpp',
  'core/resources.cpp',
  'core/property_store.cpp',
  'core/format.cpp',
  'modeset/atomic.cpp',
  'modeset/mode.cpp',
  'modeset/modeset.cpp',
  'modeset/page_flip.cpp',
  'planes/plane_registry.cpp',
  'planes/layer.cpp',
  'planes/output.cpp',
  'planes/allocator.cpp',
  'planes/matching.cpp',
  'input/seat.cpp',
  'input/keyboard.cpp',
  'input/key_repeater.cpp',
  'input/pointer.cpp',
  'input/event_dispatcher.cpp',
  'display/color_pipeline_curves.cpp',
  'display/connector_capabilities.cpp',
  'display/crtc_capabilities.cpp',
  'display/crtc_color_pipeline.cpp',
  'display/edid.cpp',
  'display/hdr_metadata.cpp',
  'display/tone_mapper.cpp',
  'display/hdr_metadata_cache.cpp',
  'display/hotplug_monitor.cpp',
  'display/scanout_target.cpp',
  'display/driver_profile.cpp',
  'display/mode_list.cpp',
  'session/seat.cpp',
  'time/clock.cpp',
  'gbm/device.cpp',
  'gbm/surface.cpp',
  'gbm/buffer.cpp',
  'dumb/buffer.cpp',
  'scene/composite_canvas.cpp',
  'scene/dumb_buffer_source.cpp',
  'core/egl_loader.cpp',
  'core/gles_loader.cpp',
  'scene/gl_compositor.cpp',
  'scene/egl_stream_builder.cpp',
  'scene/egl_stream_source.cpp',
  'scene/external_dma_buf_source.cpp',
  'scene/external_dma_buf_ring.cpp',
  'scene/dma_buf_source_cache.cpp',
  'scene/gbm_buffer_source.cpp',
  'scene/gbm_surface_source.cpp',
  'scene/gst_appsink_source.cpp',
  'scene/layer_scene.cpp',
  'scene/nvbufsurface_source.cpp',
  'scene/output_signaling.cpp',
  'scene/scene_set.cpp',
  'scene/stream_capability.cpp',
  'scene/v4l2_camera_source.cpp',
  'scene/v4l2_decoder_source.cpp',
  'scene/v4l2_plane_layout.cpp',
  'sync/fence.cpp',
  'fmt/format_mod.cpp',
  'present/negotiate.cpp',
  'present/buffer_ring.cpp',
  'present/dumb_ring_source.cpp',
  'present/dumb_scanout_sink.cpp',
  'present/scanout_format.cpp',
  'present/gbm_producer.cpp',
  'present/scanout_backend.cpp',
  'present/gl_scanout_producer.cpp',
  'present/vk_scanout_producer.cpp',
)

drm_cxx_deps = [
  drm_dep,
  gbm_dep,
  input_dep,
  udev_dep,
  xkb_dep,
  rt_dep,
  dl_dep,
  di_dep,
  tl_expected_dep,
  tcb_span_dep,
  fmt_dep,
]

# Public headers live in src/ but are #include'd as <drm-cxx/...>. Expose
# src/ under a drm-cxx/ prefix by symlinking at configure time inside the
# build tree; include_directories('..') then picks up the build-side path
# too, so <drm-cxx/foo.hpp> resolves through the symlink. Linux-only.
run_command('ln', '-sfn',
  meson.project_source_root() / 'src',
  meson.project_build_root() / 'drm-cxx',
  check : true)

drm_cxx_inc = include_directories('.', '..', '../third_party/xcursor-mini')

if get_option('vulkan')
  drm_cxx_sources += files(
    'vulkan/display.cpp',
    'vulkan/drm_surface.cpp',
  )
  if vk_dep.found()
    drm_cxx_deps += vk_dep.partial_dependency(links : true)
  endif
endif

# libseat dep + define — PRIVATE, the public header compiles either
# way. Stub branch kicks in on the .cpp side when the define is 0.
session_cpp_args = has_libseat \
  ? ['-DDRM_CXX_HAS_LIBSEAT=1'] \
  : ['-DDRM_CXX_HAS_LIBSEAT=0']
if has_libseat
  drm_cxx_deps += libseat_dep
endif

# GStreamer dep + define — same dual-branch pattern as libseat. The
# public header (scene/gst_appsink_source.hpp) is GStreamer-free; the
# .cpp branches on DRM_CXX_HAS_GSTREAMER.
gstreamer_cpp_args = has_gstreamer \
  ? ['-DDRM_CXX_HAS_GSTREAMER=1'] \
  : ['-DDRM_CXX_HAS_GSTREAMER=0']
if has_gstreamer
  drm_cxx_deps += [gst_dep, gst_app_dep, gst_alloc_dep, gst_video_dep]
endif

# EGL Streams — headers-only build dependency; libEGL is dlopen'd
# at runtime by stream_capability.cpp / egl_stream_source.cpp. The
# public header (scene/stream_capability.hpp) is EGL-free; the .cpp
# branches on DRM_CXX_HAS_EGL_STREAMS so a build without EGL headers
# still compiles and reports StreamMixingMode::Unsupported at probe.
#
# We deliberately use `egl_dep.partial_dependency(compile_args, includes)`
# rather than the full dep so libEGL is NOT added to the link line of
# libdrm-cxx — making drm-cxx loadable on Mesa systems that have egl.h
# but no libEGL.so.1 installed, and keeping our resolution strictly
# dlopen-driven.
# DRM_CXX_HAS_EGL covers the render-offload producer (GlScanoutProducer) and
# is independent of EGL Streams; DRM_CXX_HAS_EGL_STREAMS gates the scene
# Streams path. Streams implies EGL (has_egl is true whenever has_egl_streams
# is). Either one pulls in the EGL headers (compile_args/includes only — libEGL
# is dlopen'd, never on libdrm-cxx's link line).
streams_cpp_args = (has_egl_streams ? ['-DDRM_CXX_HAS_EGL_STREAMS=1'] : ['-DDRM_CXX_HAS_EGL_STREAMS=0']) \
  + (has_egl ? ['-DDRM_CXX_HAS_EGL=1'] : ['-DDRM_CXX_HAS_EGL=0'])
if has_egl
  drm_cxx_deps += egl_dep.partial_dependency(compile_args : true, includes : true)
endif

# Vulkan headers for the VkScanoutProducer. vulkan.hpp's dynamic dispatcher
# dlopen's libvulkan.so.1 at runtime, so — exactly like EGL — we take the
# compile args / includes only and never add -lvulkan to libdrm-cxx's link
# line. `has_vulkan_headers` / `vk_headers_probe` come from the top meson.build.
# Tie the VkScanoutProducer (and its DRM_CXX_HAS_VULKAN guard) to the vulkan
# option. vulkan=false drops the producer entirely — which is also what makes a
# vulkan-free / 32-bit build viable: the producer's vk_image() returns a 64-bit
# VkImage handle as void*, which can't compile on ILP32 targets (e.g. armhf).
if get_option('vulkan')
  vulkan_cpp_args = ['-DDRM_CXX_HAS_VULKAN=1']
  drm_cxx_deps += vk_headers_dep
else
  vulkan_cpp_args = ['-DDRM_CXX_HAS_VULKAN=0']
endif

# NvBufSurface — link-time dependency on libnvbufsurface.so when the
# Jetson L4T headers + library are present. The header surface
# (scene/nvbufsurface_source.hpp) is wrapped in
# `#if DRM_CXX_HAS_NVBUFSURFACE`; the matching cpp_args define lets
# callers compiled against the same toolchain see the same gate. Same
# dual-branch shape as the GStreamer and EGL Streams gates.
nvbufsurface_cpp_args = has_nvbufsurface \
  ? ['-DDRM_CXX_HAS_NVBUFSURFACE=1'] \
  : ['-DDRM_CXX_HAS_NVBUFSURFACE=0']
if has_nvbufsurface
  drm_cxx_deps += nvbufsurface_dep
endif

# drm::cursor is a feature: the .cpp files only compile when libxcursor
# is present. Public headers stay in the tree but callers linking a
# cursor-less build hit an unresolved symbol at load() / create() time.
cursor_cpp_args = []
if has_xcursor
  drm_cxx_sources += files(
    'cursor/theme.cpp',
    'cursor/cursor.cpp',
    'cursor/renderer.cpp',
    # Scene source presenting a cursor as an ordinary layer (VOP2 has no
    # CURSOR plane); depends on drm::cursor, so gated with it.
    'scene/cursor_source.cpp',
    # Vendored X11-free .xcursor loader (replaces system libxcursor).
    '../third_party/xcursor-mini/xcursor_file.c',
  )
  if xcursor_delay_arg != ''
    cursor_cpp_args += xcursor_delay_arg
  endif
endif

# drm::capture — Blend2D-backed plane composition snapshot + PNG encode.
# Build-gated on blend2d availability; public headers only install when
# the gate is on (matches drm::cursor).
capture_cpp_args = (has_blend2d ? ['-DDRM_CXX_HAS_BLEND2D=1'] : ['-DDRM_CXX_HAS_BLEND2D=0']) \
  + (has_libjpeg ? ['-DDRM_CXX_HAS_LIBJPEG=1'] : ['-DDRM_CXX_HAS_LIBJPEG=0'])
if has_blend2d
  drm_cxx_sources += files(
    'capture/snapshot.cpp',
    'capture/png.cpp',
    'csd/theme.cpp',
    'csd/surface.cpp',
    'csd/shadow_cache.cpp',
    'csd/renderer.cpp',
    'csd/animator.cpp',
    'csd/overlay_reservation.cpp',
    'csd/damage.cpp',
    'csd/presenter_plane.cpp',
    'csd/presenter_composite.cpp',
    'csd/presenter_fb.cpp',
    'csd/probe_presenter.cpp',
  )
  drm_cxx_deps += blend2d_dep
  drm_cxx_deps += tomlplusplus_dep
  if has_libjpeg
    drm_cxx_sources += files('capture/jpg.cpp')
    drm_cxx_deps += libjpeg_dep
  endif
endif

gbm_cpp_args = has_gbm_bo_create_with_modifiers2 \
  ? ['-DHAVE_GBM_BO_CREATE_WITH_MODIFIERS2=1'] \
  : []

drm_cxx_lib = library('drm-cxx',
  drm_cxx_sources,
  dependencies : drm_cxx_deps,
  include_directories : [drm_cxx_inc],
  cpp_args : session_cpp_args + cursor_cpp_args + capture_cpp_args + gbm_cpp_args + gstreamer_cpp_args + streams_cpp_args + vulkan_cpp_args + nvbufsurface_cpp_args,
  install : true,
)

drm_cxx_dep = declare_dependency(
  link_with : drm_cxx_lib,
  dependencies : drm_cxx_deps,
  include_directories : [drm_cxx_inc],
  # Streams macro is part of the public ABI: consumers of the
  # installed scene/egl_stream_builder.hpp gate on it, so dependents
  # must see the same value drm-cxx was built with.
  compile_args : streams_cpp_args + vulkan_cpp_args,
)

pkg = import('pkgconfig')

# Install public headers
install_headers(
  'buffer_mapping.hpp',
  'drm-cxx.hpp',
  'log.hpp',
  subdir : 'drm-cxx',
)

install_headers(
  'detail/expected.hpp',
  'detail/format.hpp',
  'detail/span.hpp',
  subdir : 'drm-cxx/detail',
)

install_headers(
  'core/device.hpp',
  'core/resources.hpp',
  'core/property_store.hpp',
  'core/format.hpp',
  subdir : 'drm-cxx/core',
)

install_headers(
  'modeset/atomic.hpp',
  'modeset/mode.hpp',
  'modeset/modeset.hpp',
  'modeset/page_flip.hpp',
  subdir : 'drm-cxx/modeset',
)

install_headers(
  'planes/plane_registry.hpp',
  'planes/layer.hpp',
  'planes/output.hpp',
  'planes/allocator.hpp',
  'planes/matching.hpp',
  'planes/composition_layer.hpp',
  subdir : 'drm-cxx/planes',
)

install_headers(
  'input/seat.hpp',
  'input/keyboard.hpp',
  'input/key_repeater.hpp',
  'input/pointer.hpp',
  'input/event_dispatcher.hpp',
  subdir : 'drm-cxx/input',
)

install_headers(
  'display/color_pipeline_curves.hpp',
  'display/connector_capabilities.hpp',
  'display/connector_info.hpp',
  'display/crtc_capabilities.hpp',
  'display/crtc_color_pipeline.hpp',
  'display/edid.hpp',
  'display/hdr_metadata.hpp',
  'display/hdr_metadata_cache.hpp',
  'display/hotplug_monitor.hpp',
  'display/scanout_target.hpp',
  'display/driver_profile.hpp',
  'display/mode_list.hpp',
  'display/tone_mapper.hpp',
  subdir : 'drm-cxx/display',
)

install_headers(
  'session/seat.hpp',
  subdir : 'drm-cxx/session',
)

install_headers(
  'time/clock.hpp',
  subdir : 'drm-cxx/time',
)

if has_xcursor
  install_headers(
    'cursor/theme.hpp',
    'cursor/cursor.hpp',
    'cursor/renderer.hpp',
    subdir : 'drm-cxx/cursor',
  )
  # CursorSource lives in scene/ but depends on drm::cursor, so its header
  # installs only when the cursor feature is on (matches the .cpp gate).
  install_headers(
    'scene/cursor_source.hpp',
    subdir : 'drm-cxx/scene',
  )
endif

if has_blend2d
  install_headers(
    'capture/snapshot.hpp',
    'capture/png.hpp',
    subdir : 'drm-cxx/capture',
  )
  if has_libjpeg
    install_headers('capture/jpg.hpp', subdir : 'drm-cxx/capture')
  endif
  install_headers(
    'csd/theme.hpp',
    'csd/surface.hpp',
    'csd/window_state.hpp',
    'csd/shadow_cache.hpp',
    'csd/renderer.hpp',
    'csd/animator.hpp',
    'csd/overlay_reservation.hpp',
    'csd/damage.hpp',
    'csd/presenter.hpp',
    'csd/presenter_plane.hpp',
    'csd/presenter_composite.hpp',
    'csd/presenter_fb.hpp',
    'csd/probe_presenter.hpp',
    subdir : 'drm-cxx/csd',
  )
endif

install_headers(
  'gbm/device.hpp',
  'gbm/surface.hpp',
  'gbm/buffer.hpp',
  subdir : 'drm-cxx/gbm',
)

install_headers(
  'sync/fence.hpp',
  subdir : 'drm-cxx/sync',
)

install_headers(
  'dumb/buffer.hpp',
  subdir : 'drm-cxx/dumb',
)

install_headers(
  'fmt/format_mod.hpp',
  subdir : 'drm-cxx/fmt',
)

install_headers(
  'present/negotiate.hpp',
  'present/scanout_producer.hpp',
  'present/buffer_ring.hpp',
  'present/dumb_ring_source.hpp',
  'present/dumb_scanout_sink.hpp',
  'present/scanout_format.hpp',
  'present/frame_economy.hpp',
  'present/gbm_producer.hpp',
  'present/scanout_backend.hpp',
  'present/gl_scanout_producer.hpp',
  'present/vk_scanout_producer.hpp',
  subdir : 'drm-cxx/present',
)

install_headers(
  'scene/buffer_source.hpp',
  'scene/commit_report.hpp',
  'scene/compatibility_report.hpp',
  'scene/composite_canvas.hpp',
  'scene/composition_target.hpp',
  'scene/display_params.hpp',
  'scene/dumb_buffer_source.hpp',
  'scene/external_dma_buf_source.hpp',
  'scene/external_dma_buf_ring.hpp',
  'scene/dma_buf_source_cache.hpp',
  'scene/gbm_buffer_source.hpp',
  'scene/gbm_surface_source.hpp',
  'scene/layer.hpp',
  'scene/layer_desc.hpp',
  'scene/layer_handle.hpp',
  'scene/layer_scene.hpp',
  'scene/output_signaling.hpp',
  'scene/scene_set.hpp',
  'scene/stream_capability.hpp',
  'scene/v4l2_camera_source.hpp',
  'scene/v4l2_decoder_source.hpp',
  subdir : 'drm-cxx/scene',
)

# GstAppsinkSource header only installs when the gate is on — matches
# the cursor / capture / csd pattern.
if has_gstreamer
  install_headers(
    'scene/gst_appsink_source.hpp',
    subdir : 'drm-cxx/scene',
  )
endif

# EglStreamBuilder + EglStreamSource headers only install when the
# streams gate is on — both pull in EGL/egl.h and would fail to
# compile on hosts without EGL development headers. The source
# header is part of the public surface so callers can query
# producer_surface() after the scene's first commit triggers the
# deferred producer-side attachment.
if has_egl_streams
  install_headers(
    'scene/egl_stream_builder.hpp',
    'scene/egl_stream_source.hpp',
    subdir : 'drm-cxx/scene',
  )
endif

# NvBufSurfaceSource header — same gating: only installs on Jetson L4T
# hosts where libnvbufsurface is available. The header itself uses
# `#if DRM_CXX_HAS_NVBUFSURFACE` so it's safe to install unconditionally,
# but matching the pattern keeps the public-API set honest about what's
# actually buildable on the current host.
if has_nvbufsurface
  install_headers(
    'scene/nvbufsurface_source.hpp',
    subdir : 'drm-cxx/scene',
  )
endif

if get_option('vulkan')
  install_headers(
    'vulkan/display.hpp',
    'vulkan/drm_surface.hpp',
    subdir : 'drm-cxx/vulkan',
  )
endif

# pkg-config
pkg.generate(drm_cxx_lib,
  name : 'drm-cxx',
  description : 'C++17 DRM/KMS library',
  version : meson.project_version(),
)
