// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "planes/output.hpp"

TEST(OutputTest, ConstructWithCompositionLayer) {
  drm::planes::Layer comp_layer;
  drm::planes::Output output(42, comp_layer);

  EXPECT_EQ(output.crtc_id(), 42u);
  EXPECT_EQ(output.composition_layer(), &comp_layer);
  EXPECT_TRUE(comp_layer.is_composition_layer());
}

TEST(OutputTest, AddAndRemoveLayer) {
  drm::planes::Layer comp_layer;
  drm::planes::Output output(1, comp_layer);

  auto& layer = output.add_layer();
  EXPECT_EQ(output.layers().size(), 1u);

  output.remove_layer(layer);
  EXPECT_EQ(output.layers().size(), 0u);
}

TEST(OutputTest, AnyLayerDirtyDetection) {
  drm::planes::Layer comp_layer;
  drm::planes::Output output(1, comp_layer);

  auto& layer = output.add_layer();
  EXPECT_TRUE(output.any_layer_dirty()); // New layers are dirty

  output.mark_clean();
  EXPECT_FALSE(output.any_layer_dirty());

  layer.set_property("FB_ID", 99);
  EXPECT_TRUE(output.any_layer_dirty());
}

TEST(OutputTest, ChangedLayersReturnsOnlyDirty) {
  drm::planes::Layer comp_layer;
  drm::planes::Output output(1, comp_layer);

  auto& l1 = output.add_layer();
  auto& l2 = output.add_layer();
  output.mark_clean();

  l1.set_property("FB_ID", 1);

  auto changed = output.changed_layers();
  EXPECT_EQ(changed.size(), 1u);
  EXPECT_EQ(changed[0], &l1);

  // l2 should not be in changed
  for (auto* l : changed) {
    EXPECT_NE(l, &l2);
  }
}

TEST(OutputTest, SortLayersByZpos) {
  drm::planes::Layer comp_layer;
  drm::planes::Output output(1, comp_layer);

  auto& l1 = output.add_layer();
  auto& l2 = output.add_layer();
  auto& l3 = output.add_layer();

  l1.set_property("zpos", 3);
  l2.set_property("zpos", 1);
  l3.set_property("zpos", 2);

  output.sort_layers_by_zpos();

  auto& layers = output.layers();
  ASSERT_EQ(layers.size(), 3u);
  EXPECT_EQ(layers[0], &l2); // zpos 1
  EXPECT_EQ(layers[1], &l3); // zpos 2
  EXPECT_EQ(layers[2], &l1); // zpos 3
}

TEST(OutputTest, SetCompositionLayerUpdatesFlag) {
  drm::planes::Layer comp1;
  drm::planes::Layer comp2;
  drm::planes::Output output(1, comp1);

  EXPECT_TRUE(comp1.is_composition_layer());
  EXPECT_FALSE(comp2.is_composition_layer());

  output.set_composition_layer(comp2);
  EXPECT_FALSE(comp1.is_composition_layer());
  EXPECT_TRUE(comp2.is_composition_layer());
  EXPECT_EQ(output.composition_layer(), &comp2);
}
