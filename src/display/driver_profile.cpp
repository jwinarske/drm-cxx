// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
// display/driver_profile.cpp

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/display/driver_profile.hpp>

#include <drm.h>
#include <xf86drm.h>

#include <cstddef>
#include <cstdint>
#include <system_error>

namespace drm::display {

PrimeCaps decode_prime_caps(std::uint64_t cap) noexcept {
  return PrimeCaps{(cap & DRM_PRIME_CAP_IMPORT) != 0U, (cap & DRM_PRIME_CAP_EXPORT) != 0U};
}

namespace {

bool cap_flag(int fd, std::uint64_t cap) {
  std::uint64_t value = 0;
  return drmGetCap(fd, cap, &value) == 0 && value != 0U;
}

std::uint64_t cap_value(int fd, std::uint64_t cap, std::uint64_t fallback) {
  std::uint64_t value = 0;
  return (drmGetCap(fd, cap, &value) == 0 && value != 0U) ? value : fallback;
}

}  // namespace

drm::expected<DriverProfile, std::error_code> DriverProfile::probe(const drm::Device& dev) {
  const int fd = dev.fd();

  DriverProfile profile;
  if (drmVersionPtr version = drmGetVersion(fd)) {
    if (version->name != nullptr) {
      profile.name.assign(version->name, static_cast<std::size_t>(version->name_len));
    }
    drmFreeVersion(version);
  } else {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::no_such_device));
  }

  profile.addfb2_modifiers = cap_flag(fd, DRM_CAP_ADDFB2_MODIFIERS);
  profile.async_page_flip = cap_flag(fd, DRM_CAP_ASYNC_PAGE_FLIP);
  profile.timestamp_monotonic = cap_flag(fd, DRM_CAP_TIMESTAMP_MONOTONIC);

  std::uint64_t prime = 0;
  if (drmGetCap(fd, DRM_CAP_PRIME, &prime) == 0) {
    const PrimeCaps pc = decode_prime_caps(prime);
    profile.prime_import = pc.can_import;
    profile.prime_export = pc.can_export;
  }

  profile.cursor_width = cap_value(fd, DRM_CAP_CURSOR_WIDTH, 64);
  profile.cursor_height = cap_value(fd, DRM_CAP_CURSOR_HEIGHT, 64);

  return profile;
}

}  // namespace drm::display
