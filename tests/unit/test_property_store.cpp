// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "core/property_store.hpp"

#include <drm_mode.h>

#include <gtest/gtest.h>

TEST(PropertyStoreTest, LookupOnEmptyStoreReturnsError) {
  drm::PropertyStore const store;
  auto result = store.property_id(42, "type");
  ASSERT_FALSE(result.has_value());
}

TEST(PropertyStoreTest, PropertyValueOnEmptyStoreReturnsError) {
  drm::PropertyStore const store;
  auto result = store.property_value(42, "type");
  ASSERT_FALSE(result.has_value());
}

TEST(PropertyStoreTest, PropertiesOnEmptyStoreReturnsNull) {
  drm::PropertyStore const store;
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
