# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

# Optional logind integration via sdbus-c++ (>= 2.0). Treat the headers
# as 'system' so sdbus-c++'s own warnings don't surface under -Werror.
logind_feature = get_option('examples_logind')
if not logind_feature.disabled()
  sdbus_cpp_dep = dependency('sdbus-c++', required : logind_feature.enabled(),
                             version : '>=2.0', include_type : 'system')
else
  sdbus_cpp_dep = disabler()
endif

logind_compile_args = sdbus_cpp_dep.found() \
  ? ['-DDRM_CXX_EXAMPLES_HAVE_LOGIND=1'] \
  : ['-DDRM_CXX_EXAMPLES_HAVE_LOGIND=0']

examples_logind_lib = static_library('examples_logind',
  'logind_session.cpp',
  dependencies : sdbus_cpp_dep.found() ? [drm_cxx_dep, sdbus_cpp_dep] : [drm_cxx_dep],
  cpp_args : logind_compile_args,
)

examples_logind_dep = declare_dependency(
  link_with : examples_logind_lib,
  include_directories : include_directories('.'),
  compile_args : logind_compile_args,
  dependencies : sdbus_cpp_dep.found() ? [sdbus_cpp_dep] : [],
)

if not sdbus_cpp_dep.found()
  message('examples: sdbus-c++ not found — LogindSession will be a runtime no-op')
endif

executable('overlay_planes',
  'overlay_planes/main.cpp',
  dependencies : [drm_cxx_dep, examples_logind_dep],
)

executable('atomic_modeset',
  'atomic_modeset/main.cpp',
  dependencies : [drm_cxx_dep, examples_logind_dep],
)

executable('hotplug_monitor',
  'hotplug_monitor/main.cpp',
  dependencies : [drm_cxx_dep, examples_logind_dep],
)

executable('mouse_cursor',
  'mouse_cursor/main.cpp',
  dependencies : [drm_cxx_dep, examples_logind_dep],
)

if get_option('vulkan')
  executable('vulkan_display',
    'vulkan_display/main.cpp',
    dependencies : [drm_cxx_dep, examples_logind_dep],
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
