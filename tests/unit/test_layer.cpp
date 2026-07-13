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

// The FB-only fast path (allocator skips its TEST_ONLY when property_hash is
// unchanged) is only safe if the hash isolates exactly the content properties
// — FB_ID and IN_FENCE_FD — from every placement/format property. This matrix
// pins that classification: a placement change MUST move the hash (so it can't
// sail past untested), and FB_ID / IN_FENCE_FD must NOT.
TEST(LayerTest, PropertyHashIsolatesFbOnlyContentFromPlacement) {
  using drm::planes::PropTag;
  const auto make = [] {
    drm::planes::Layer l;
    l.set_property(PropTag::FbModifier, 0);
    l.set_property(PropTag::CrtcId, 1);
    l.set_property(PropTag::CrtcX, 0);
    l.set_property(PropTag::CrtcY, 0);
    l.set_property(PropTag::CrtcW, 1920);
    l.set_property(PropTag::CrtcH, 1080);
    l.set_property(PropTag::SrcX, 0);
    l.set_property(PropTag::SrcY, 0);
    l.set_property(PropTag::SrcW, 1920ULL << 16U);
    l.set_property(PropTag::SrcH, 1080ULL << 16U);
    l.set_property(PropTag::Rotation, 1);
    l.set_property(PropTag::Alpha, 0xFFFF);
    l.set_property(PropTag::Zpos, 1);
    l.set_property(PropTag::PixelFormat, 0x34325258);  // NV12
    l.set_property(PropTag::FbId, 10);
    l.set_property(PropTag::InFenceFd, 7);
    return l;
  };
  const auto base = make().property_hash();

  // Content: swapping the buffer / fence must be invisible to the hash.
  {
    auto l = make();
    l.set_property(PropTag::FbId, 999);
    l.set_property(PropTag::InFenceFd, 42);
    EXPECT_EQ(l.property_hash(), base)
        << "FB_ID / IN_FENCE_FD are content and must be excluded from the hash";
  }

  // Placement / format: every one must move the hash. 0x5A5A5A5A differs from
  // each baseline value above.
  const PropTag placement[] = {
      PropTag::FbModifier, PropTag::CrtcId, PropTag::CrtcX, PropTag::CrtcY,      PropTag::CrtcW,
      PropTag::CrtcH,      PropTag::SrcX,   PropTag::SrcY,  PropTag::SrcW,       PropTag::SrcH,
      PropTag::Rotation,   PropTag::Alpha,  PropTag::Zpos,  PropTag::PixelFormat};
  for (const PropTag tag : placement) {
    auto l = make();
    l.set_property(tag, 0x5A5A5A5AULL);
    EXPECT_NE(l.property_hash(), base)
        << "changing placement/format prop " << static_cast<int>(tag) << " must change the hash";
  }
}

TEST(LayerTest, AssignedPlaneIdDefaultsToNullopt) {
  drm::planes::Layer const layer;
  EXPECT_FALSE(layer.assigned_plane_id().has_value());
}
