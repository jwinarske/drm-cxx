// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "core/property_store.hpp"

#include <xf86drmMode.h>

#include <gtest/gtest.h>

TEST(PropertyStoreTest, LookupOnEmptyStoreReturnsError) {
  drm::PropertyStore store;
  auto result = store.property_id(42, "type");
  ASSERT_FALSE(result.has_value());
}

TEST(PropertyStoreTest, PropertyValueOnEmptyStoreReturnsError) {
  drm::PropertyStore store;
  auto result = store.property_value(42, "type");
  ASSERT_FALSE(result.has_value());
}

TEST(PropertyStoreTest, PropertiesOnEmptyStoreReturnsNull) {
  drm::PropertyStore store;
  EXPECT_EQ(store.properties(42), nullptr);
}

TEST(PropertyStoreTest, ClearEmptiesStore) {
  drm::PropertyStore store;
  store.clear();
  EXPECT_EQ(store.properties(42), nullptr);
}

TEST(PropertyStoreTest, CacheWithInvalidFdReturnsError) {
  drm::PropertyStore store;
  auto result = store.cache_properties(-1, 42, DRM_MODE_OBJECT_CONNECTOR);
  ASSERT_FALSE(result.has_value());
}
