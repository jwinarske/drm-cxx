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
  test_gbm
  test_dumb_buffer)

if(DRM_CXX_HAS_XCURSOR)
  list(APPEND DRM_CXX_TESTS
    test_cursor_theme
    test_cursor_cursor)
endif()

if(DRM_CXX_HAS_BLEND2D)
  list(APPEND DRM_CXX_TESTS
    test_capture_snapshot)
endif()

foreach(_t ${DRM_CXX_TESTS})
  add_executable(${_t} unit/${_t}.cpp)
  target_link_libraries(${_t} PRIVATE
    drm-cxx::drm-cxx
    GTest::gtest
    GTest::gtest_main)
  gtest_discover_tests(${_t})
endforeach()

# VKMS integration test — self-skips when vkms is absent. Kept out of
# the loop above because its source lives under tests/integration/
# rather than tests/unit/.
if(DRM_CXX_HAS_BLEND2D)
  add_executable(test_capture_vkms integration/test_capture_vkms.cpp)
  target_link_libraries(test_capture_vkms PRIVATE
    drm-cxx::drm-cxx
    GTest::gtest
    GTest::gtest_main)
  gtest_discover_tests(test_capture_vkms)
endif()

# drm::scene unit tests — scene lives in the library now (src/scene/),
# so the test binary just links drm-cxx::drm-cxx like every other unit test.
add_executable(test_scene unit/test_scene.cpp)
target_link_libraries(test_scene PRIVATE
  drm-cxx::drm-cxx
  GTest::gtest
  GTest::gtest_main)
gtest_discover_tests(test_scene)

# signage_player playlist parser. signage_playlist still lives under
# examples/, so the playlist test only builds when examples do.
if(DRM_CXX_BUILD_EXAMPLES)
  add_executable(test_playlist unit/test_playlist.cpp)
  target_link_libraries(test_playlist PRIVATE
    signage_playlist
    GTest::gtest
    GTest::gtest_main)
  gtest_discover_tests(test_playlist)

  # test_patterns painters — same arrangement as the playlist test above.
  add_executable(test_test_patterns unit/test_test_patterns.cpp)
  target_link_libraries(test_test_patterns PRIVATE
    test_patterns_painters
    GTest::gtest
    GTest::gtest_main)
  gtest_discover_tests(test_test_patterns)
endif()
