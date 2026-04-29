# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# Benchmark binaries — synthetic workloads that produce CSV / pass-fail
# data the project quotes in technical writing. Off by default in both
# build systems; flipped on with -DDRM_CXX_BUILD_BENCHMARKS=ON (CMake)
# or `meson configure -Dbenchmarks=true`.

foreach(_bench plane_stress allocator_torture)
  add_executable(${_bench} ${_bench}/main.cpp)
  target_link_libraries(${_bench} PRIVATE drm-cxx::drm-cxx)
  # Reuse the example helpers (open_output, format_probe) — benchmarks
  # are internal tools, not consumers of the public API only.
  target_include_directories(${_bench} PRIVATE
    ${CMAKE_SOURCE_DIR}/examples)
endforeach()