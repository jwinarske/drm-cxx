# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

project('drm-cxx', ['cpp'],
  version : '1.0.0',
  meson_version : '>=1.3.0',
  license : 'MIT',
  default_options : [
    'cpp_std=c++17',
    'b_lto=true',
    'warning_level=3',
  ],
)

cpp = meson.get_compiler('cpp')

assert(cpp.has_argument('-std=c++17'),
  'A C++17-capable compiler is required (GCC >= 9, Clang >= 10)')

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

# gbm_bo_create_with_modifiers2 — usage-flag-aware variant added in mesa
# 23. src/gbm/buffer.cpp falls back to gbm_bo_create_with_modifiers (no
# usage parameter) when the probe fails, so we still build cleanly
# against older libgbm at the cost of dropping cfg.usage on the
# modifier path.
has_gbm_bo_create_with_modifiers2 = cpp.has_function(
  'gbm_bo_create_with_modifiers2',
  dependencies : gbm_dep,
  prefix : '#include <gbm.h>',
)
input_dep   = dependency('libinput',  include_type : 'system', required : true)
udev_dep    = dependency('libudev',   include_type : 'system', required : true)
xkb_dep     = dependency('xkbcommon', include_type : 'system', required : true)
rt_dep      = cpp.find_library('rt',  required : true)
dl_dep      = cpp.find_library('dl',  required : true)

di_dep = dependency('libdisplay-info', required : false)
if not di_dep.found()
  di_proj = subproject('libdisplay-info',
    default_options : ['default_library=static'])
  di_dep = di_proj.get_variable('di_dep')
endif

# C++17-compatible polyfills for C++23 std::expected and C++20 std::span.
# The drm-cxx/detail/{expected,span}.hpp adapter headers pick the std
# versions when the compiler provides them (__cpp_lib_expected /
# __cpp_lib_span are set), otherwise they fall back to these subprojects
# — so under cpp_std=c++17 they activate, under c++23 they are dead code.
tl_expected_dep = dependency('tl-expected', required : false)
if not tl_expected_dep.found()
  tl_expected_proj = subproject('tl-expected')
  tl_expected_dep = tl_expected_proj.get_variable('tl_expected_dep')
endif

tcb_span_dep = dependency('tcb-span', required : false)
if not tcb_span_dep.found()
  tcb_span_proj = subproject('tcb-span')
  tcb_span_dep = tcb_span_proj.get_variable('tcb_span_dep')
endif

# {fmt} backs the drm-cxx/detail/format.hpp adapter when the toolchain lacks
# std::print / std::format (i.e. anything below C++23 / libstdc++ 14). Under
# cpp_std=c++17 this path is always taken; under c++23 on libstdc++ 14+
# the adapter aliases std::* directly and fmt is unused.
has_std_print = cpp.compiles(
  '#include <print>\nint main(){ std::println("{}", 1); }',
  name : 'std::print / std::println available',
)
if has_std_print
  fmt_dep = dependency('', required : false)
else
  fmt_dep = dependency('fmt', required : false)
  if not fmt_dep.found()
    # fmt ships only a CMakeLists.txt, so wrap it as a CMake subproject.
    cmake = import('cmake')
    fmt_opts = cmake.subproject_options()
    fmt_opts.add_cmake_defines({
      'FMT_TEST'                 : 'OFF',
      'FMT_DOC'                  : 'OFF',
      'CMAKE_POSITION_INDEPENDENT_CODE' : 'ON',
    })
    fmt_opts.set_override_option('werror', 'false')
    fmt_opts.set_override_option('warning_level', '0')
    fmt_proj = cmake.subproject('fmt', options : fmt_opts)
    # Wrap the subproject's static fmt target with link_whole so every
    # archive member lands in the final library. Under clang + LTO,
    # archive-based linking can fail to pull in members referenced
    # only from late-instantiated templates — e.g. fmt::vprintln from
    # a fmt::println<std::string>(FILE*, ...) call site that inlines
    # after archive resolution has finished — so the final link ends
    # with an undefined-reference error even though the symbol is in
    # libfmt.a. link_whole unconditionally pulls every member.
    fmt_dep = declare_dependency(
      link_whole : fmt_proj.target('fmt'),
      include_directories : include_directories('subprojects/fmt/include'),
    )
  endif
endif

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

# libseat probe for drm::session::Seat. Sourced strictly from the
# system via pkg-config; not vendored.
session_feature = get_option('session')
if not session_feature.disabled()
  libseat_dep = dependency('libseat', required : session_feature.enabled(),
                           include_type : 'system')
else
  libseat_dep = disabler()
endif
has_libseat = libseat_dep.found()
if not has_libseat
  message('drm-cxx: libseat not found — drm::session::Seat will be a runtime no-op')
endif

# libxcursor probe for drm::cursor. The renderer's KMS side needs no
# extra deps (just libdrm, already required), but the theme loader
# calls XcursorFilenameLoadImages to rasterize .cursor files.
cursor_feature = get_option('cursor')
if not cursor_feature.disabled()
  xcursor_lib_dep = dependency('xcursor', required : cursor_feature.enabled(),
                               include_type : 'system')
else
  xcursor_lib_dep = disabler()
endif
has_xcursor = xcursor_lib_dep.found()
if has_xcursor
  xcursor_delay_arg = cpp.has_member('XcursorImage', 'delay',
    prefix : '#include <X11/Xcursor/Xcursor.h>',
    dependencies : xcursor_lib_dep) ? '-DHAS_XCURSOR_DELAY' : ''
else
  xcursor_delay_arg = ''
  message('drm-cxx: libxcursor not found — drm::cursor will not be built')
endif

# Blend2D probe for drm::capture (and future drm::csd). Upstream ships
# a CMake config but no pkg-config file — meson's `method: 'cmake'`
# consumes the CMake config directly and produces a normal dependency
# object that can be added to drm_cxx_deps.
blend2d_feature = get_option('blend2d')
if not blend2d_feature.disabled()
  blend2d_dep = dependency('blend2d', method : 'cmake',
                           required : blend2d_feature.enabled(),
                           include_type : 'system')
else
  blend2d_dep = disabler()
endif
has_blend2d = blend2d_dep.found()
if has_blend2d
  # Some distributions ship the CMake config in a separate package from
  # the headers themselves — a tidy runner that can't reach the headers
  # will fail the whole TU. Verify the umbrella is actually includable
  # before advertising the gate as ON.
  if not cpp.has_header('blend2d/blend2d.h', dependencies : blend2d_dep) \
      and not cpp.has_header('blend2d.h', dependencies : blend2d_dep)
    has_blend2d = false
    message('drm-cxx: blend2d found via CMake config but headers not reachable — treating as not installed')
  endif
endif
if not has_blend2d and not blend2d_feature.disabled()
  message('drm-cxx: blend2d not found — drm::capture will not be built')
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
subdir('src')

if get_option('examples')
  subdir('examples')
endif

if get_option('tests')
  subdir('tests')
endif

if get_option('benchmarks')
  subdir('benchmarks')
endif
