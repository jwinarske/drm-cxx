# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

drm_cxx_sources = files(
  'core/device.cpp',
  'core/resources.cpp',
  'core/property_store.cpp',
  'core/format.cpp',
  'modeset/atomic.cpp',
  'modeset/mode.cpp',
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
  'display/edid.cpp',
  'display/hotplug_monitor.cpp',
  'session/seat.cpp',
  'time/clock.cpp',
  'gbm/device.cpp',
  'gbm/surface.cpp',
  'gbm/buffer.cpp',
  'dumb/buffer.cpp',
  'scene/composite_canvas.cpp',
  'scene/dumb_buffer_source.cpp',
  'scene/external_dma_buf_source.cpp',
  'scene/gbm_buffer_source.cpp',
  'scene/layer_scene.cpp',
  'sync/fence.cpp',
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

drm_cxx_inc = include_directories('.', '..')

if get_option('vulkan')
  drm_cxx_sources += files(
    'vulkan/display.cpp',
    'vulkan/drm_surface.cpp',
  )
  drm_cxx_deps += vk_dep
endif

# libseat dep + define — PRIVATE, the public header compiles either
# way. Stub branch kicks in on the .cpp side when the define is 0.
session_cpp_args = has_libseat \
  ? ['-DDRM_CXX_HAS_LIBSEAT=1'] \
  : ['-DDRM_CXX_HAS_LIBSEAT=0']
if has_libseat
  drm_cxx_deps += libseat_dep
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
  )
  drm_cxx_deps += xcursor_lib_dep
  if xcursor_delay_arg != ''
    cursor_cpp_args += xcursor_delay_arg
  endif
endif

# drm::capture — Blend2D-backed plane composition snapshot + PNG encode.
# Build-gated on blend2d availability; public headers only install when
# the gate is on (matches drm::cursor).
capture_cpp_args = has_blend2d \
  ? ['-DDRM_CXX_HAS_BLEND2D=1'] \
  : ['-DDRM_CXX_HAS_BLEND2D=0']
if has_blend2d
  drm_cxx_sources += files(
    'capture/snapshot.cpp',
    'capture/png.cpp',
    'csd/theme.cpp',
    'csd/surface.cpp',
  )
  drm_cxx_deps += blend2d_dep
  drm_cxx_deps += tomlplusplus_dep
endif

gbm_cpp_args = has_gbm_bo_create_with_modifiers2 \
  ? ['-DHAVE_GBM_BO_CREATE_WITH_MODIFIERS2=1'] \
  : []

drm_cxx_lib = library('drm-cxx',
  drm_cxx_sources,
  dependencies : drm_cxx_deps,
  include_directories : [drm_cxx_inc],
  cpp_args : session_cpp_args + cursor_cpp_args + capture_cpp_args + gbm_cpp_args,
  install : true,
)

drm_cxx_dep = declare_dependency(
  link_with : drm_cxx_lib,
  dependencies : drm_cxx_deps,
  include_directories : [drm_cxx_inc],
)

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
  'display/connector_info.hpp',
  'display/edid.hpp',
  'display/hdr_metadata.hpp',
  'display/hotplug_monitor.hpp',
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
endif

if has_blend2d
  install_headers(
    'capture/snapshot.hpp',
    'capture/png.hpp',
    subdir : 'drm-cxx/capture',
  )
  install_headers(
    'csd/theme.hpp',
    'csd/surface.hpp',
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
  'scene/buffer_source.hpp',
  'scene/commit_report.hpp',
  'scene/compatibility_report.hpp',
  'scene/composite_canvas.hpp',
  'scene/display_params.hpp',
  'scene/dumb_buffer_source.hpp',
  'scene/external_dma_buf_source.hpp',
  'scene/gbm_buffer_source.hpp',
  'scene/layer.hpp',
  'scene/layer_desc.hpp',
  'scene/layer_handle.hpp',
  'scene/layer_scene.hpp',
  subdir : 'drm-cxx/scene',
)

if get_option('vulkan')
  install_headers(
    'vulkan/display.hpp',
    'vulkan/drm_surface.hpp',
    subdir : 'drm-cxx/vulkan',
  )
endif

# pkg-config
pkg = import('pkgconfig')
pkg.generate(drm_cxx_lib,
  name : 'drm-cxx',
  description : 'C++17 DRM/KMS library',
  version : meson.project_version(),
)
