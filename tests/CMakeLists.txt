# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

# Prefer a consumer-provided gtest; otherwise fetch it.
find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

set(DRM_CXX_TESTS
  test_format
  test_property_store
  test_device
  test_atomic_builder
  test_sync_fence
  test_matching
  test_layer
  test_output
  test_plane_allocator
  test_input
  test_mode
  test_display
  test_gbm)

if(DRM_CXX_HAS_XCURSOR)
  list(APPEND DRM_CXX_TESTS
    test_cursor_theme
    test_cursor_cursor)
endif()

foreach(_t ${DRM_CXX_TESTS})
  add_executable(${_t} unit/${_t}.cpp)
  target_link_libraries(${_t} PRIVATE
    drm-cxx::drm-cxx
    GTest::gtest
    GTest::gtest_main)
  gtest_discover_tests(${_t})
endforeach()
