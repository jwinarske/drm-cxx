// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "png.hpp"

#include "snapshot.hpp"

// Blend2D's header layout varies by distro / install method:
//   * upstream source install and Fedora:  /usr/include/blend2d/blend2d.h
//   * some older Debian/Ubuntu packages:   /usr/include/blend2d.h
// Pick whichever the preprocessor can reach. NOLINT is required because
// the umbrella re-exports sub-headers — misc-include-cleaner can't
// attribute any symbol directly to it.
//
// When *neither* path is reachable we deliberately DO NOT #error: some
// CI configurations run clang-tidy across every source file regardless
// of whether the build system would exclude this TU, and the tidy run
// does not inherit Blend2D's `-isystem` flags. Degrading to an empty
// TU keeps those tidy passes green. A real build where Blend2D is
// expected will still fail loudly at link-time (missing symbol) if
// the body doesn't compile.
#if __has_include(<blend2d/blend2d.h>)
#include <blend2d/blend2d.h>  // NOLINT(misc-include-cleaner)
#define DRM_CXX_CAPTURE_HAS_BL2D
#elif __has_include(<blend2d.h>)
#include <blend2d.h>  // NOLINT(misc-include-cleaner)
#define DRM_CXX_CAPTURE_HAS_BL2D
#endif

// drm::expected, drm::unexpected, std::error_code, std::make_error_code
// and std::errc all reach this TU transitively through "png.hpp" →
// "snapshot.hpp", so including <drm-cxx/detail/expected.hpp> or
// <system_error> here would trip misc-include-cleaner's redundant-include
// check on CI's older clang-tidy.
#include <cstdint>
#include <string>
#include <string_view>

namespace drm::capture {

#ifdef DRM_CXX_CAPTURE_HAS_BL2D

// Blend2D surfaces its public API through <blend2d/blend2d.h>, which
// misc-include-cleaner cannot attribute as a symbol provider — see the
// matching comment in snapshot.cpp. Suppress the check for the body that
// touches Blend2D types.
// NOLINTBEGIN(misc-include-cleaner)

// NOLINTNEXTLINE(misc-use-internal-linkage)
drm::expected<void, std::error_code> write_png(const Image& image, std::string_view path) {
  if (image.empty()) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  // Wrap our pixel buffer as a BLImage for the duration of the write.
  // BL_DATA_ACCESS_READ + null destroy callback means Blend2D never
  // writes to our storage and won't free it when the BLImage destructs.
  // The C API takes void* even for read-only access, so we shed const
  // at the last moment — the READ flag documents the intent.
  BLImage bl;
  auto* pixel_data = const_cast<std::uint32_t*>(  // NOLINT(cppcoreguidelines-pro-type-const-cast)
      image.pixels().data());
  const BLResult cr =
      bl.create_from_data(static_cast<int>(image.width()), static_cast<int>(image.height()),
                          BL_FORMAT_PRGB32, pixel_data, static_cast<intptr_t>(image.stride_bytes()),
                          BL_DATA_ACCESS_READ, nullptr, nullptr);
  if (cr != BL_SUCCESS) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::io_error));
  }

  BLImageCodec codec;
  if (codec.find_by_name("PNG") != BL_SUCCESS) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::function_not_supported));
  }

  // BLImage::write_to_file takes a null-terminated path; copy the view.
  const std::string null_terminated(path);
  if (bl.write_to_file(null_terminated.c_str(), codec) != BL_SUCCESS) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::io_error));
  }

  return {};
}

// NOLINTEND(misc-include-cleaner)

#endif  // DRM_CXX_CAPTURE_HAS_BL2D

}  // namespace drm::capture