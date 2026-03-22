# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: Apache-2.0

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
  'input/seat.cpp',
  'input/keyboard.cpp',
  'input/pointer.cpp',
  'display/edid.cpp',
  'gbm/device.cpp',
  'gbm/surface.cpp',
  'gbm/buffer.cpp',
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
]

drm_cxx_inc = include_directories('.')

if get_option('vulkan')
  drm_cxx_sources += files(
    'vulkan/display.cpp',
    'vulkan/drm_surface.cpp',
  )
  drm_cxx_deps += vk_dep
endif

drm_cxx_lib = library('drm-cxx',
  drm_cxx_sources,
  dependencies : drm_cxx_deps,
  include_directories : [drm_cxx_inc],
  install : true,
)

drm_cxx_dep = declare_dependency(
  link_with : drm_cxx_lib,
  dependencies : drm_cxx_deps,
  include_directories : [drm_cxx_inc],
)

# Install public headers
install_headers(
  'drm-cxx.hpp',
  subdir : 'drm-cxx',
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
  'planes/composition_layer.hpp',
  subdir : 'drm-cxx/planes',
)

install_headers(
  'input/seat.hpp',
  'input/keyboard.hpp',
  'input/pointer.hpp',
  'input/event_dispatcher.hpp',
  subdir : 'drm-cxx/input',
)

install_headers(
  'display/connector_info.hpp',
  'display/edid.hpp',
  'display/hdr_metadata.hpp',
  subdir : 'drm-cxx/display',
)

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
  description : 'C++23 DRM/KMS library',
  version : meson.project_version(),
)
