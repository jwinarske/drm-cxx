// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// csd/shadow_cache.hpp — LRU of pre-blurred decoration shadows.
//
// The Option C shadow architecture (see docs/blend2d_plan.md) bakes
// the shadow into the decoration buffer's alpha margin so each window
// stays at one plane. The blur is the expensive part; ShadowCache
// pre-renders it once per unique (size, elevation, theme) and keeps a
// small LRU of the resulting BLImages. Focus animation re-uses the
// cached entries — a focus-change cross-fade is a blend between two
// cached shadow images, not a re-blur.
//
// The cache is consumed only by the renderer. To keep this public
// header Blend2D-free (mirrors csd/surface.hpp + csd/theme.hpp), the
// lookup API hands the cached pixels to the renderer through a
// destination-pixel-buffer interface rather than returning a BLImage:
// the renderer passes a `ShadowDest` aimed at its own BLContext-bound
// buffer, and the cache memcpy/blits the cached pixels in. Internally
// the cache stores BLImages (the blur kernel writes them directly via
// Blend2D pixel-buffer access), but consumers never see the type.
//
// theme_id() hashes only the visual fields (colors, corner_radius,
// shadow_extent, noise_amplitude). The theme `name` is intentionally
// excluded so two themes that paint identically share cache entries.

#pragma once

#include "theme.hpp"

#include <cstddef>
#include <cstdint>

namespace drm::csd {

enum class Elevation : std::uint8_t {
  Blurred,  // Unfocused window — softer, lower-contrast shadow.
  Focused,  // Focused window — sharper, slightly stronger shadow.
};

struct ShadowKey {
  std::uint32_t width{};   // Shadow patch width in pixels.
  std::uint32_t height{};  // Shadow patch height in pixels.
  Elevation elevation{Elevation::Blurred};
  std::uint64_t theme_id{};
};

inline bool operator==(const ShadowKey& a, const ShadowKey& b) noexcept {
  return a.width == b.width && a.height == b.height && a.elevation == b.elevation &&
         a.theme_id == b.theme_id;
}
inline bool operator!=(const ShadowKey& a, const ShadowKey& b) noexcept {
  return !(a == b);
}

// Stable hash over the visual fields the renderer reads. Two themes
// that produce the same pixels (same colors, corner radius, shadow
// extent, noise amplitude) hash to the same value even if their `name`
// or animation_duration_ms differ — the cache only cares about the
// pixel contribution.
[[nodiscard]] std::uint64_t theme_id(const Theme& theme) noexcept;

// Destination pixel buffer the cache writes the cached shadow into.
// The renderer points this at its target BLImage's pixel data + stride
// and the cache copies the cached pixels in (one row at a time, with
// SRC_OVER-style alpha blending). Format is fixed at PRGB32 — both the
// cache's internal storage and the renderer's BLContext target.
struct ShadowDest {
  std::uint8_t* pixels{nullptr};  // First byte of the destination region.
  std::uint32_t stride{0};        // Bytes between consecutive rows.
  std::uint32_t width{0};         // Destination region width in pixels.
  std::uint32_t height{0};        // Destination region height in pixels.
};

class ShadowCache {
 public:
  // Default capacity is 8 — one shadow per docs in a typical 4-window
  // session, doubled for blurred/focused per window. Larger sessions
  // pay only per-eviction recompute cost (single-shot blur, ~5 ms at
  // 800×600 on a modern x86 host).
  static constexpr std::size_t k_default_capacity = 8;

  explicit ShadowCache(std::size_t capacity = k_default_capacity);
  ~ShadowCache();

  ShadowCache(const ShadowCache&) = delete;
  ShadowCache& operator=(const ShadowCache&) = delete;
  ShadowCache(ShadowCache&& other) noexcept;
  ShadowCache& operator=(ShadowCache&& other) noexcept;

  // Look up (or compute on miss) the shadow for `key` rendered with
  // `theme` and SRC_OVER-blend it into `dst`. The destination region
  // must match key.width / key.height; smaller destinations clip, larger
  // destinations leave the trailing area untouched. Returns true on hit
  // OR successful miss-compute, false only if dst is empty/null or the
  // miss-path could not allocate.
  bool blit_into(const ShadowKey& key, const Theme& theme, const ShadowDest& dst);

  void clear() noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept;

  // True if the cache currently holds an entry for `key`. Diagnostic
  // only; production code should call blit_into() and trust the LRU.
  [[nodiscard]] bool contains(const ShadowKey& key) const noexcept;

 private:
  // Implementation details (BLImage storage, std::list for LRU order,
  // unordered_map index) live in the .cpp behind a forward-declared
  // Impl so the public header stays Blend2D-free.
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace drm::csd
