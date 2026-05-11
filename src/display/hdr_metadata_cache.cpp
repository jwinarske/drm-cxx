// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "hdr_metadata_cache.hpp"

#include "../core/device.hpp"
#include "hdr_metadata.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>
#include <utility>

namespace drm::display {

drm::expected<std::uint32_t, std::error_code> HdrMetadataCache::set(
    const drm::Device& dev, const std::uint32_t crtc_id,
    const std::optional<HdrSourceMetadata>& src) {
  return set_with_factory(
      crtc_id, src, [&dev](const HdrSourceMetadata& s) { return HdrMetadataBlob::create(dev, s); });
}

drm::expected<std::uint32_t, std::error_code> HdrMetadataCache::set_with_factory(
    const std::uint32_t crtc_id, const std::optional<HdrSourceMetadata>& src,
    const HdrMetadataBlobFactory& factory) {
  // Clear path: nullopt → drop any cached entry, queue old blob, return 0.
  if (!src.has_value()) {
    if (auto it = active_.find(crtc_id); it != active_.end()) {
      pending_destruction_.push_back(std::move(it->second.blob));
      active_.erase(it);
    }
    return 0U;
  }

  const std::uint64_t new_hash = hdr_metadata_hash(*src);

  // Hit: same crtc, same hash → return existing id, no kernel work.
  if (auto it = active_.find(crtc_id); it != active_.end() && it->second.hash == new_hash) {
    return it->second.blob.blob_id();
  }

  // Miss: build a new blob, evict old entry to pending_destruction_.
  auto new_blob = factory(*src);
  if (!new_blob) {
    return drm::unexpected<std::error_code>(new_blob.error());
  }
  const std::uint32_t new_blob_id = new_blob->blob_id();

  if (auto it = active_.find(crtc_id); it != active_.end()) {
    pending_destruction_.push_back(std::move(it->second.blob));
    it->second.blob = std::move(*new_blob);
    it->second.hash = new_hash;
  } else {
    active_.emplace(crtc_id, Entry{std::move(*new_blob), new_hash});
  }
  return new_blob_id;
}

void HdrMetadataCache::acknowledge_committed() noexcept {
  pending_destruction_.clear();
}

void HdrMetadataCache::clear_for_session_loss() noexcept {
  for (auto& [_, entry] : active_) {
    entry.blob.forget();
  }
  active_.clear();
  for (auto& blob : pending_destruction_) {
    blob.forget();
  }
  pending_destruction_.clear();
}

void HdrMetadataCache::flush() noexcept {
  for (auto& [_, entry] : active_) {
    pending_destruction_.push_back(std::move(entry.blob));
  }
  active_.clear();
  pending_destruction_.clear();
}

std::size_t HdrMetadataCache::pending_destruction_count() const noexcept {
  return pending_destruction_.size();
}

std::uint32_t HdrMetadataCache::current_blob_id(const std::uint32_t crtc_id) const noexcept {
  if (auto it = active_.find(crtc_id); it != active_.end()) {
    return it->second.blob.blob_id();
  }
  return 0U;
}

std::uint64_t HdrMetadataCache::current_content_hash(const std::uint32_t crtc_id) const noexcept {
  if (auto it = active_.find(crtc_id); it != active_.end()) {
    return it->second.hash;
  }
  return 0U;
}

}  // namespace drm::display
