// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "dma_buf_source_cache.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/external_dma_buf_source.hpp>

#include <cstdint>
#include <system_error>
#include <utility>

namespace drm::scene {

ExternalDmaBufSource* DmaBufSourceCache::find(std::uintptr_t key) const noexcept {
  const auto it = cache_.find(key);
  return it == cache_.end() ? nullptr : it->second.get();
}

void DmaBufSourceCache::evict(std::uintptr_t key) noexcept {
  cache_.erase(key);
}

void DmaBufSourceCache::clear() noexcept {
  cache_.clear();
}

drm::expected<ExternalDmaBufSource*, std::error_code> DmaBufSourceCache::get_or_create(
    std::uintptr_t key, const drm::Device& dev, std::uint32_t width, std::uint32_t height,
    std::uint32_t drm_format, std::uint64_t modifier, drm::span<const ExternalPlaneInfo> planes) {
  if (const auto it = cache_.find(key); it != cache_.end()) {
    const auto& f = it->second->format();
    if (f.width == width && f.height == height && f.drm_fourcc == drm_format &&
        f.modifier == modifier) {
      return it->second.get();  // cache hit — reuse the source + its cached fb_id
    }
    cache_.erase(it);  // same key, different buffer → recreate below
  }
  auto src = ExternalDmaBufSource::create(dev, width, height, drm_format, modifier, planes);
  if (!src) {
    return drm::unexpected<std::error_code>(src.error());
  }
  const auto result = cache_.insert_or_assign(key, std::move(*src));
  return result.first->second.get();
}

}  // namespace drm::scene
