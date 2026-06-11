// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::present::DumbScanoutSink covering what's observable
// without a live KMS device. The real present -> atomic-flip path is exercised
// by the software_present example on hardware (it needs DRM master).

#include "core/device.hpp"
#include "present/dumb_scanout_sink.hpp"

#include <xf86drmMode.h>

#include <array>
#include <cstddef>
#include <gtest/gtest.h>

namespace {

drmModeModeInfo fake_mode() {
  drmModeModeInfo m{};
  m.hdisplay = 640;
  m.vdisplay = 480;
  m.vrefresh = 60;
  return m;
}

}  // namespace

TEST(DumbScanoutSink, CreateFailsOnInvalidDevice) {
  // Building the scene probes the device; an invalid fd can't be set up.
  auto dev = drm::Device::from_fd(-1);
  const auto mode = fake_mode();
  auto sink = drm::present::DumbScanoutSink::create(dev, /*crtc_id=*/0, /*connector_id=*/0, mode);
  EXPECT_FALSE(sink.has_value());

  // If a future change defers device IO past create(), the present() argument
  // guard must still reject a frame shorter than height * stride before any IO.
  if (sink.has_value()) {
    std::array<std::byte, 16> tiny{};
    auto r = (*sink)->present(tiny, /*src_stride=*/640 * 4);
    EXPECT_FALSE(r.has_value());
  }
}
