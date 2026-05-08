// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// hdr_metadata_cache.hpp — per-CRTC HDR_OUTPUT_METADATA blob cache.
//
// `HdrMetadataCache` owns the most-recently-emitted property blob for
// each CRTC and dedups by content hash so unchanged metadata doesn't
// churn the kernel blob every frame. Replacement blobs queue the
// outgoing blob for destruction; the kernel may keep the old blob
// alive between `drmModeCreatePropertyBlob` and the atomic commit
// that switches the connector property to the new id, so callers
// invoke `acknowledge_committed()` AFTER the commit lands to release
// the queue. Until acknowledged, the old blobs remain alive.

#pragma once

#include "hdr_metadata.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace drm {
class Device;
}  // namespace drm

namespace drm::display {

/// Factory callback for `HdrMetadataCache::set_with_factory`. Returns
/// an owning `HdrMetadataBlob` (or an error) for `src`. Tests
/// substitute a fake factory that synthesizes blobs without libdrm;
/// production callers route through `HdrMetadataCache::set` which
/// supplies a factory wrapping `HdrMetadataBlob::create`.
using HdrMetadataBlobFactory =
    std::function<drm::expected<HdrMetadataBlob, std::error_code>(const HdrSourceMetadata&)>;

class HdrMetadataCache {
 public:
  HdrMetadataCache() = default;
  ~HdrMetadataCache() = default;

  HdrMetadataCache(HdrMetadataCache&&) noexcept = default;
  HdrMetadataCache& operator=(HdrMetadataCache&&) noexcept = default;
  HdrMetadataCache(const HdrMetadataCache&) = delete;
  HdrMetadataCache& operator=(const HdrMetadataCache&) = delete;

  /// Set or clear HDR metadata for `crtc_id` and return the blob id
  /// to write to the connector's `HDR_OUTPUT_METADATA` property.
  ///
  /// `nullopt` clears: returns 0 and queues any prior blob for the
  /// crtc for post-commit destruction. A non-null `src` whose hash
  /// matches the most-recently-set value for `crtc_id` is a no-op
  /// (returns the cached blob id, no kernel work). Otherwise creates
  /// a fresh blob via `HdrMetadataBlob::create` and queues the old
  /// blob (if any) for destruction.
  drm::expected<std::uint32_t, std::error_code> set(const drm::Device& dev, std::uint32_t crtc_id,
                                                    const std::optional<HdrSourceMetadata>& src);

  /// As `set` but with caller-supplied blob factory. The factory is
  /// invoked only when the cache decides a fresh blob is needed (i.e.
  /// no cached entry, or hash mismatch). Existing tests use this to
  /// exercise the cache logic with synthetic blobs.
  drm::expected<std::uint32_t, std::error_code> set_with_factory(
      std::uint32_t crtc_id, const std::optional<HdrSourceMetadata>& src,
      const HdrMetadataBlobFactory& factory);

  /// Drop pending-destruction blobs. Callers invoke this AFTER an
  /// atomic commit referencing the new `HDR_OUTPUT_METADATA` blob has
  /// landed: at that point the connector property has switched away
  /// from any prior blob ids, so destroying them is safe.
  void acknowledge_committed() noexcept;

  /// Number of blobs queued for post-commit destruction.
  [[nodiscard]] std::size_t pending_destruction_count() const noexcept;

  /// Currently-cached blob id for `crtc_id`. Returns 0 when no
  /// metadata is cached for the crtc (or it was last cleared).
  [[nodiscard]] std::uint32_t current_blob_id(std::uint32_t crtc_id) const noexcept;

  /// Currently-cached content hash for `crtc_id`. Returns 0 when no
  /// metadata is cached. Note that a real-content hash of exactly 0
  /// is theoretically possible but vanishingly unlikely with FNV-1a;
  /// callers should use `current_blob_id() != 0` as the primary
  /// "is anything cached" probe.
  [[nodiscard]] std::uint64_t current_content_hash(std::uint32_t crtc_id) const noexcept;

 private:
  struct Entry {
    HdrMetadataBlob blob;
    std::uint64_t hash{0};
  };

  std::unordered_map<std::uint32_t, Entry> active_;
  std::vector<HdrMetadataBlob> pending_destruction_;
};

}  // namespace drm::display
