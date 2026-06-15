// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::scene::canvas_format_for_plane — the composition-canvas
// output-format negotiation. This is the device-free core of the single-plane
// composition fallback: on minimal controllers that expose exactly one plane
// (PRIMARY, no overlay/cursor) the canvas MUST adopt a format that plane can
// actually scan out, or every overflow layer is dropped. The canonical board is
// tilcdc (BeagleBone Black): no ARGB8888/XRGB8888, but RGB565 / XBGR8888 work.
// The same one-plane / format-limited shape applies to i.MX LCDIF and tiny SPI
// panels, so guarding the policy here covers all of them without the hardware.

#include "planes/plane_registry.hpp"
#include "scene/composite_canvas.hpp"

#include <drm_fourcc.h>

#include <cstdint>
#include <gtest/gtest.h>
#include <utility>
#include <vector>

namespace {

drm::planes::PlaneCapabilities plane_with(std::vector<std::uint32_t> formats) {
  drm::planes::PlaneCapabilities caps;
  caps.formats = std::move(formats);
  return caps;
}

}  // namespace

// tilcdc: no ARGB8888/XRGB8888, but XBGR8888 (and RGB565) scan out. The fix:
// pick XBGR8888 rather than be stuck with an un-armable ARGB8888 canvas. This is
// the regression the single-plane composition fallback turns on.
TEST(CanvasFormatForPlane, TilcdcPicksXbgrNotArgb) {
  const auto caps = plane_with({DRM_FORMAT_RGB565, DRM_FORMAT_XBGR8888, DRM_FORMAT_BGR888});
  const auto fmt = drm::scene::canvas_format_for_plane(caps);
  ASSERT_TRUE(fmt.has_value());
  EXPECT_EQ(fmt.value_or(0U), DRM_FORMAT_XBGR8888);
}

// Multi-plane controllers that advertise ARGB8888 keep it — byte-identical to
// the internal blend, so flush() copies with no per-row conversion.
TEST(CanvasFormatForPlane, PrefersArgbWhenAvailable) {
  const auto caps =
      plane_with({DRM_FORMAT_NV12, DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_RGB565});
  const auto fmt = drm::scene::canvas_format_for_plane(caps);
  ASSERT_TRUE(fmt.has_value());
  EXPECT_EQ(fmt.value_or(0U), DRM_FORMAT_ARGB8888);
}

// Preference order is by the canvas's conversion cost, not the plane's
// advertisement order: ARGB8888 wins even when listed after XBGR8888.
TEST(CanvasFormatForPlane, PreferenceOrderIndependentOfAdvertisementOrder) {
  const auto caps = plane_with({DRM_FORMAT_XBGR8888, DRM_FORMAT_ARGB8888});
  const auto fmt = drm::scene::canvas_format_for_plane(caps);
  ASSERT_TRUE(fmt.has_value());
  EXPECT_EQ(fmt.value_or(0U), DRM_FORMAT_ARGB8888);
}

// A tiny RGB565-only SPI panel still hosts a canvas (16bpp pack).
TEST(CanvasFormatForPlane, Rgb565OnlyPanel) {
  const auto fmt = drm::scene::canvas_format_for_plane(plane_with({DRM_FORMAT_RGB565}));
  ASSERT_TRUE(fmt.has_value());
  EXPECT_EQ(fmt.value_or(0U), DRM_FORMAT_RGB565);
}

// Each canvas-capable format is individually selectable — i.e. every format the
// negotiation can return is one flush() can emit (the canvas_output_bpp guard in
// the impl keeps the two lists from drifting apart). XRGB8888/ABGR8888/BGR565
// round out the 8888 orders + 16bpp packs not covered above.
TEST(CanvasFormatForPlane, EveryCanvasFormatIsSelectable) {
  for (const std::uint32_t fourcc : {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888, DRM_FORMAT_XBGR8888,
                                     DRM_FORMAT_ABGR8888, DRM_FORMAT_RGB565, DRM_FORMAT_BGR565}) {
    const auto fmt = drm::scene::canvas_format_for_plane(plane_with({fourcc}));
    ASSERT_TRUE(fmt.has_value()) << "fourcc not selectable: " << fourcc;
    EXPECT_EQ(fmt.value_or(0U), fourcc);
  }
}

// A plane that scans out only YUV / unsupported packed formats cannot host the
// canvas — nullopt, so the scene keeps looking (or drops the overflow) rather
// than arming an un-emittable buffer.
TEST(CanvasFormatForPlane, UnsupportablePlaneReturnsNullopt) {
  EXPECT_FALSE(drm::scene::canvas_format_for_plane(plane_with({DRM_FORMAT_NV12, DRM_FORMAT_YUYV}))
                   .has_value());
}

TEST(CanvasFormatForPlane, EmptyFormatsReturnsNullopt) {
  EXPECT_FALSE(drm::scene::canvas_format_for_plane(plane_with({})).has_value());
}
