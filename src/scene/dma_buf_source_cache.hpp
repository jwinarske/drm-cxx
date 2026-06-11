// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// dma_buf_source_cache.hpp — reuse ExternalDmaBufSource across frames.
//
// A producer that re-presents the same small set of DMA-BUF-backed buffers
// (e.g. an EGL/Vulkan swapchain's images) should not re-import each one and
// re-create its KMS framebuffer every frame. This cache keys an
// ExternalDmaBufSource by a caller-chosen buffer identity (a pointer / handle
// cast to uintptr_t) and hands back the same source — with its create()-time
// cached fb_id — on every reuse, so steady-state per-frame work stays at the
// atomic commit. It recreates a source only when the buffer's geometry/format
// changes under the same key (the producer reused the key for a new buffer).

#pragma once

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/scene/external_dma_buf_source.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <unordered_map>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

class DmaBufSourceCache {
 public:
  DmaBufSourceCache() = default;
  ~DmaBufSourceCache() = default;
  DmaBufSourceCache(const DmaBufSourceCache&) = delete;
  DmaBufSourceCache& operator=(const DmaBufSourceCache&) = delete;
  DmaBufSourceCache(DmaBufSourceCache&&) = default;
  DmaBufSourceCache& operator=(DmaBufSourceCache&&) = default;

  /// Return the source cached under `key`, creating it from the DMA-BUF tuple on
  /// first sight. If a source is already cached under `key` but its width /
  /// height / fourcc / modifier no longer match, it is recreated (the producer
  /// reused the key for a different buffer). The returned pointer is owned by the
  /// cache and stays valid until the matching evict()/clear() or destruction.
  [[nodiscard]] drm::expected<ExternalDmaBufSource*, std::error_code> get_or_create(
      std::uintptr_t key, const drm::Device& dev, std::uint32_t width, std::uint32_t height,
      std::uint32_t drm_format, std::uint64_t modifier, drm::span<const ExternalPlaneInfo> planes);

  /// The source cached under `key`, or nullptr if none.
  [[nodiscard]] ExternalDmaBufSource* find(std::uintptr_t key) const noexcept;

  /// Drop the source cached under `key` (e.g. the producer freed that buffer).
  void evict(std::uintptr_t key) noexcept;

  /// Drop every cached source (e.g. on session loss / device rebind).
  void clear() noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return cache_.size(); }

 private:
  std::unordered_map<std::uintptr_t, std::unique_ptr<ExternalDmaBufSource>> cache_;
};

}  // namespace drm::scene
