# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: Apache-2.0

project('drm-cxx', ['cpp'],
  version : '1.0.0',
  meson_version : '>=1.3.0',
  license : 'Apache-2.0',
  default_options : [
    'cpp_std=c++23',
    'b_lto=true',
    'warning_level=3',
  ],
)

cpp = meson.get_compiler('cpp')

# Enforce C++23 features are truly available
assert(cpp.has_argument('-std=c++23'),
  'A C++23-capable compiler is required (GCC >= 13, Clang >= 16)')

add_project_arguments(
  cpp.get_supported_arguments([
    '-Wno-unused-parameter',
    '-Wno-missing-field-initializers',
  ]),
  language : 'cpp',
)

#── Dependencies ──────────────────────────────────────────────
drm_dep     = dependency('libdrm',    include_type : 'system', required : true)
gbm_dep     = dependency('gbm',       include_type : 'system', required : true)
input_dep   = dependency('libinput',  include_type : 'system', required : true)
udev_dep    = dependency('libudev',   include_type : 'system', required : true)
xkb_dep     = dependency('xkbcommon', include_type : 'system', required : true)
rt_dep      = cpp.find_library('rt',  required : true)
dl_dep      = cpp.find_library('dl',  required : true)

di_proj = subproject('libdisplay-info',
  default_options : ['default_library=static'])
di_dep = di_proj.get_variable('di_dep')

if get_option('vulkan')
  # Try system vulkan headers first, fall back to subproject
  vk_dep = dependency('vulkan', required : false)
  if not vk_dep.found()
    cmake = import('cmake')
    vk_opts = cmake.subproject_options()
    vk_headers = cmake.subproject('Vulkan-Headers', options : vk_opts)
    vk_dep = vk_headers.dependency('Vulkan-Headers')
  endif
endif

#── Configuration ─────────────────────────────────────────────
conf = configuration_data()
conf.set('DRM_CXX_VERSION', meson.project_version())
conf.set('DRM_CXX_VERSION_MAJOR', meson.project_version().split('.')[0].to_int())
conf.set('DRM_CXX_VERSION_MINOR', meson.project_version().split('.')[1].to_int())
conf.set('DRM_CXX_VERSION_PATCH', meson.project_version().split('.')[2].to_int())
conf.set10('HAVE_VULKAN', get_option('vulkan'))

config_h = configure_file(
  input : 'config.h.meson',
  output : 'config.h',
  configuration : conf,
)

#── Source ────────────────────────────────────────────────────
subdir('drm-cxx')

if get_option('examples')
  subdir('examples')
endif

if get_option('tests')
  subdir('tests')
endif
