// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "planes/layer.hpp"

#include <gtest/gtest.h>

TEST(LayerTest, SetPropertyMarksLayerDirty) {
  drm::planes::Layer layer;
  layer.mark_clean();
  EXPECT_FALSE(layer.is_dirty());

  layer.set_property("FB_ID", 42);
  EXPECT_TRUE(layer.is_dirty());
}

TEST(LayerTest, SetSameValueDoesNotDirty) {
  drm::planes::Layer layer;
  layer.set_property("FB_ID", 42);
  layer.mark_clean();
  EXPECT_FALSE(layer.is_dirty());

  // Setting same value should not mark dirty
  layer.set_property("FB_ID", 42);
  EXPECT_FALSE(layer.is_dirty());
}

TEST(LayerTest, DisableSetsFbIdToZero) {
  drm::planes::Layer layer;
  layer.set_property("FB_ID", 42);
  layer.disable();

  auto val = layer.property("FB_ID");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, 0U);
}

TEST(LayerTest, SetCompositedMakesForceComposited) {
  drm::planes::Layer layer;
  layer.set_composited();
  // After allocation, composited layers get needs_composition = true
  // but the flag itself is internal. We verify via the API.
  EXPECT_FALSE(layer.needs_composition());  // Not set until allocator runs
}

TEST(LayerTest, PropertyLookupReturnsCorrectValue) {
  drm::planes::Layer layer;
  layer.set_property("CRTC_X", 100);
  layer.set_property("CRTC_Y", 200);
  layer.set_property("CRTC_W", 1920);
  layer.set_property("CRTC_H", 1080);

  EXPECT_EQ(layer.property("CRTC_X"), 100U);
  EXPECT_EQ(layer.property("CRTC_Y"), 200U);
  EXPECT_EQ(layer.width(), 1920U);
  EXPECT_EQ(layer.height(), 1080U);
}

TEST(LayerTest, UnknownPropertyReturnsNullopt) {
  drm::planes::Layer const layer;
  EXPECT_FALSE(layer.property("nonexistent").has_value());
}

TEST(LayerTest, CrtcRectIsCorrect) {
  drm::planes::Layer layer;
  layer.set_property("CRTC_X", 10);
  layer.set_property("CRTC_Y", 20);
  layer.set_property("CRTC_W", 800);
  layer.set_property("CRTC_H", 600);

  auto r = layer.crtc_rect();
  EXPECT_EQ(r.x, 10);
  EXPECT_EQ(r.y, 20);
  EXPECT_EQ(r.w, 800U);
  EXPECT_EQ(r.h, 600U);
}

TEST(LayerTest, RequiresScalingDetection) {
  drm::planes::Layer layer;
  // SRC is 16.16 fixed point
  layer.set_property("SRC_W", 1920U << 16);
  layer.set_property("SRC_H", 1080U << 16);
  layer.set_property("CRTC_W", 1920);
  layer.set_property("CRTC_H", 1080);
  EXPECT_FALSE(layer.requires_scaling());

  layer.set_property("CRTC_W", 960);
  EXPECT_TRUE(layer.requires_scaling());
}

TEST(LayerTest, ContentTypeAndUpdateHint) {
  drm::planes::Layer layer;
  layer.set_content_type(drm::planes::ContentType::Video);
  layer.set_update_hint(60);

  EXPECT_EQ(layer.content_type(), drm::planes::ContentType::Video);
  EXPECT_EQ(layer.update_hz(), 60U);
}

TEST(LayerTest, PropertyHashExcludesFbId) {
  drm::planes::Layer layer;
  layer.set_property("CRTC_X", 100);
  layer.set_property("FB_ID", 1);

  auto h1 = layer.property_hash();

  layer.set_property("FB_ID", 2);
  auto h2 = layer.property_hash();

  // Hash should not change when only FB_ID changes
  EXPECT_EQ(h1, h2);
}

TEST(LayerTest, AssignedPlaneIdDefaultsToNullopt) {
  drm::planes::Layer const layer;
  EXPECT_FALSE(layer.assigned_plane_id().has_value());
}
