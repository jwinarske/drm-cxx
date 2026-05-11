// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "display/hdr_metadata.hpp"
#include "display/hdr_metadata_cache.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <functional>
#include <gtest/gtest.h>
#include <optional>
#include <system_error>

namespace {

using drm::display::HdrMetadataBlob;
using drm::display::HdrMetadataCache;
using drm::display::HdrSourceMetadata;
using drm::display::TransferFunction;

/// Build a minimally distinct HdrSourceMetadata. Each call increments
/// `seed` so successive samples have different hashes (used to drive
/// the "second set with different content evicts" path).
HdrSourceMetadata sample(std::uint16_t seed) {
  HdrSourceMetadata src;
  src.eotf = TransferFunction::SmpteSt2084Pq;
  src.max_content_light_level = seed;
  return src;
}

/// Factory that mints synthetic blobs with monotonically increasing
/// ids, recording how many times it was invoked. Lets the unit test
/// see whether the cache skipped or invoked the blob factory.
class CountingFactory {
 public:
  drm::expected<HdrMetadataBlob, std::error_code> operator()(
      const HdrSourceMetadata& src) noexcept {
    ++calls_;
    const std::uint32_t id = next_id_++;
    return HdrMetadataBlob::synthesize_for_test(id, drm::display::hdr_metadata_hash(src));
  }
  [[nodiscard]] int calls() const noexcept { return calls_; }
  [[nodiscard]] std::uint32_t next_id() const noexcept { return next_id_; }

 private:
  int calls_{0};
  std::uint32_t next_id_{1000};  // start far from 0 so we don't collide with "no entry" sentinel
};

TEST(HdrMetadataCacheTest, EmptyCacheReportsNoEntries) {
  const HdrMetadataCache cache;
  EXPECT_EQ(cache.current_blob_id(1), 0U);
  EXPECT_EQ(cache.current_blob_id(42), 0U);
  EXPECT_EQ(cache.current_content_hash(1), 0U);
  EXPECT_EQ(cache.pending_destruction_count(), 0U);
}

TEST(HdrMetadataCacheTest, FirstSetCreatesBlob) {
  HdrMetadataCache cache;
  CountingFactory factory;
  const auto src = sample(1000);

  auto r = cache.set_with_factory(7, src, std::ref(factory));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 1000U);
  EXPECT_EQ(factory.calls(), 1);
  EXPECT_EQ(cache.current_blob_id(7), 1000U);
  EXPECT_EQ(cache.current_content_hash(7), drm::display::hdr_metadata_hash(src));
  EXPECT_EQ(cache.pending_destruction_count(), 0U);
}

TEST(HdrMetadataCacheTest, IdenticalContentReusesCachedBlob) {
  HdrMetadataCache cache;
  CountingFactory factory;
  const auto src = sample(500);

  auto r1 = cache.set_with_factory(3, src, std::ref(factory));
  ASSERT_TRUE(r1.has_value());
  const auto first_id = *r1;

  auto r2 = cache.set_with_factory(3, src, std::ref(factory));
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(*r2, first_id) << "same content must return cached id";
  EXPECT_EQ(factory.calls(), 1) << "factory should not be invoked on cache hit";
  EXPECT_EQ(cache.pending_destruction_count(), 0U);
}

TEST(HdrMetadataCacheTest, ChangedContentEvictsToPendingDestruction) {
  HdrMetadataCache cache;
  CountingFactory factory;

  auto first = cache.set_with_factory(5, sample(100), std::ref(factory));
  ASSERT_TRUE(first.has_value());
  const auto first_id = *first;

  auto second = cache.set_with_factory(5, sample(200), std::ref(factory));
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(*second, first_id);
  EXPECT_EQ(factory.calls(), 2);
  EXPECT_EQ(cache.pending_destruction_count(), 1U);
  EXPECT_EQ(cache.current_blob_id(5), *second);
}

TEST(HdrMetadataCacheTest, ClearReturnsZeroAndQueuesOldBlob) {
  HdrMetadataCache cache;
  CountingFactory factory;

  auto created = cache.set_with_factory(9, sample(1), std::ref(factory));
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(cache.current_blob_id(9), *created);

  auto cleared = cache.set_with_factory(9, std::nullopt, std::ref(factory));
  ASSERT_TRUE(cleared.has_value());
  EXPECT_EQ(*cleared, 0U);
  EXPECT_EQ(factory.calls(), 1) << "clear must not invoke the factory";
  EXPECT_EQ(cache.current_blob_id(9), 0U);
  EXPECT_EQ(cache.current_content_hash(9), 0U);
  EXPECT_EQ(cache.pending_destruction_count(), 1U);
}

TEST(HdrMetadataCacheTest, ClearOnEmptyEntryIsNoOp) {
  HdrMetadataCache cache;
  CountingFactory factory;

  auto r = cache.set_with_factory(11, std::nullopt, std::ref(factory));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 0U);
  EXPECT_EQ(factory.calls(), 0);
  EXPECT_EQ(cache.pending_destruction_count(), 0U);
}

TEST(HdrMetadataCacheTest, AcknowledgeCommittedDrainsPending) {
  HdrMetadataCache cache;
  CountingFactory factory;

  cache.set_with_factory(1, sample(1), std::ref(factory)).value();
  cache.set_with_factory(1, sample(2), std::ref(factory)).value();
  cache.set_with_factory(1, sample(3), std::ref(factory)).value();
  EXPECT_EQ(cache.pending_destruction_count(), 2U);

  cache.acknowledge_committed();
  EXPECT_EQ(cache.pending_destruction_count(), 0U);
  // Active entry survives acknowledge_committed().
  EXPECT_NE(cache.current_blob_id(1), 0U);
}

TEST(HdrMetadataCacheTest, AcknowledgeOnEmptyQueueIsNoOp) {
  HdrMetadataCache cache;
  cache.acknowledge_committed();
  EXPECT_EQ(cache.pending_destruction_count(), 0U);
  cache.acknowledge_committed();
  EXPECT_EQ(cache.pending_destruction_count(), 0U);
}

TEST(HdrMetadataCacheTest, MultipleCrtcsAreIndependent) {
  HdrMetadataCache cache;
  CountingFactory factory;

  auto a = cache.set_with_factory(1, sample(10), std::ref(factory));
  auto b = cache.set_with_factory(2, sample(20), std::ref(factory));
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_NE(*a, *b);
  EXPECT_EQ(factory.calls(), 2);
  EXPECT_EQ(cache.pending_destruction_count(), 0U);

  // Replacing crtc 1's metadata doesn't touch crtc 2's blob.
  auto a2 = cache.set_with_factory(1, sample(11), std::ref(factory));
  ASSERT_TRUE(a2.has_value());
  EXPECT_NE(*a2, *a);
  EXPECT_EQ(cache.current_blob_id(1), *a2);
  EXPECT_EQ(cache.current_blob_id(2), *b) << "crtc 2 unchanged";
  EXPECT_EQ(cache.pending_destruction_count(), 1U);
}

TEST(HdrMetadataCacheTest, FactoryFailureLeavesCacheUnchanged) {
  HdrMetadataCache cache;
  CountingFactory factory;

  cache.set_with_factory(4, sample(1), std::ref(factory)).value();
  const auto good_id = cache.current_blob_id(4);
  const auto good_hash = cache.current_content_hash(4);

  // Factory that always fails — simulates an EINVAL from the kernel.
  auto bad_factory =
      [](const HdrSourceMetadata&) -> drm::expected<HdrMetadataBlob, std::error_code> {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  };
  auto r = cache.set_with_factory(4, sample(2), bad_factory);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
  // Cache state is unchanged: prior entry still there, queue empty.
  EXPECT_EQ(cache.current_blob_id(4), good_id);
  EXPECT_EQ(cache.current_content_hash(4), good_hash);
  EXPECT_EQ(cache.pending_destruction_count(), 0U);
}

TEST(HdrMetadataCacheTest, ClearAndRecreateRequeuesAndRebuilds) {
  HdrMetadataCache cache;
  CountingFactory factory;

  cache.set_with_factory(8, sample(1), std::ref(factory)).value();
  cache.set_with_factory(8, std::nullopt, std::ref(factory)).value();
  EXPECT_EQ(cache.current_blob_id(8), 0U);
  EXPECT_EQ(cache.pending_destruction_count(), 1U);

  // Re-setting after clear creates a fresh blob.
  auto r = cache.set_with_factory(8, sample(1), std::ref(factory));
  ASSERT_TRUE(r.has_value());
  EXPECT_NE(*r, 0U);
  EXPECT_EQ(factory.calls(), 2);
  EXPECT_EQ(cache.pending_destruction_count(), 1U);
}

}  // namespace
