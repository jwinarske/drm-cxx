// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "shadow_cache.hpp"

#include "theme.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

namespace drm::csd {

namespace {

// FNV-1a 64-bit. Used for theme_id and the ShadowKey hash. Stable
// across runs — important for shadow cache reuse across reload cycles.
constexpr std::uint64_t k_fnv_offset = 0xCBF29CE484222325ULL;
constexpr std::uint64_t k_fnv_prime = 0x100000001B3ULL;

std::uint64_t fnv1a(const void* data, std::size_t bytes,
                    std::uint64_t seed = k_fnv_offset) noexcept {
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::uint64_t h = seed;
  for (std::size_t i = 0; i < bytes; ++i) {
    h ^= p[i];
    h *= k_fnv_prime;
  }
  return h;
}

// Append a Color to the running hash. Color is 4 packed bytes; hashing
// them in fixed order keeps theme_id endian-independent.
std::uint64_t hash_color(std::uint64_t h, const Color& c) noexcept {
  const std::uint8_t bytes[4] = {c.r, c.g, c.b, c.a};
  return fnv1a(bytes, sizeof(bytes), h);
}

// Append a 32-bit value to the running hash (network byte order so the
// hash is endian-independent).
std::uint64_t hash_u32(std::uint64_t h, std::uint32_t v) noexcept {
  const std::uint8_t bytes[4] = {
      static_cast<std::uint8_t>((v >> 24U) & 0xFFU),
      static_cast<std::uint8_t>((v >> 16U) & 0xFFU),
      static_cast<std::uint8_t>((v >> 8U) & 0xFFU),
      static_cast<std::uint8_t>(v & 0xFFU),
  };
  return fnv1a(bytes, sizeof(bytes), h);
}

// ── Rounded-rect coverage ────────────────────────────────────
//
// The shadow is the soft halo around a rounded panel. We render the
// panel as a single-channel alpha mask first, blur it, then tint it
// with the theme's shadow color at composite time. This keeps the
// blur cheap (one channel instead of four) and means the cache does
// not need Blend2D — everything is plain pixel arithmetic.

bool inside_rounded_rect(int x, int y, int rx, int ry, int rw, int rh, int radius) noexcept {
  if (x < rx || x >= rx + rw || y < ry || y >= ry + rh) {
    return false;
  }
  // Inside the inset stroke region: always covered.
  if (x >= rx + radius && x < rx + rw - radius) {
    return true;
  }
  if (y >= ry + radius && y < ry + rh - radius) {
    return true;
  }
  // Corner case — pick the nearest corner center and check the
  // squared distance against the radius. Using the squared form keeps
  // the inner loop FP-free; on cache miss we walk every pixel of the
  // patch so this is hot.
  const int cx = x < rx + radius ? rx + radius : rx + rw - 1 - radius;
  const int cy = y < ry + radius ? ry + radius : ry + rh - 1 - radius;
  const int dx = x - cx;
  const int dy = y - cy;
  return (dx * dx) + (dy * dy) <= radius * radius;
}

// ── Box blur ─────────────────────────────────────────────────
//
// Three passes of a separable, sliding-window box blur. Three box
// passes is the standard cheap Gaussian approximation; visually
// identical to a true Gaussian at the radii we use (≤ 24 px).
//
// Operates on a single-channel uint8 buffer in place via a scratch
// buffer of equal size. radius is a half-window — total window is
// (2*radius + 1) samples wide.

void box_blur_h(const std::uint8_t* src, std::uint8_t* dst, int width, int height,
                int radius) noexcept {
  if (radius <= 0) {
    std::memcpy(dst, src, static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    return;
  }
  for (int y = 0; y < height; ++y) {
    const std::uint8_t* row_src = src + (static_cast<std::ptrdiff_t>(y) * width);
    std::uint8_t* row_dst = dst + (static_cast<std::ptrdiff_t>(y) * width);
    int sum = 0;
    int count = 0;
    // Prime the window with [0, radius].
    for (int x = 0; x <= radius && x < width; ++x) {
      sum += row_src[x];
      ++count;
    }
    for (int x = 0; x < width; ++x) {
      row_dst[x] = static_cast<std::uint8_t>(sum / count);
      // Slide right: add x+radius+1 if in range, drop x-radius if in range.
      const int add_x = x + radius + 1;
      const int drop_x = x - radius;
      if (add_x < width) {
        sum += row_src[add_x];
        ++count;
      }
      if (drop_x >= 0) {
        sum -= row_src[drop_x];
        --count;
      }
    }
  }
}

void box_blur_v(const std::uint8_t* src, std::uint8_t* dst, int width, int height,
                int radius) noexcept {
  if (radius <= 0) {
    std::memcpy(dst, src, static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    return;
  }
  for (int x = 0; x < width; ++x) {
    int sum = 0;
    int count = 0;
    for (int y = 0; y <= radius && y < height; ++y) {
      sum += src[(static_cast<std::ptrdiff_t>(y) * width) + x];
      ++count;
    }
    for (int y = 0; y < height; ++y) {
      dst[(static_cast<std::ptrdiff_t>(y) * width) + x] = static_cast<std::uint8_t>(sum / count);
      const int add_y = y + radius + 1;
      const int drop_y = y - radius;
      if (add_y < height) {
        sum += src[(static_cast<std::ptrdiff_t>(add_y) * width) + x];
        ++count;
      }
      if (drop_y >= 0) {
        sum -= src[(static_cast<std::ptrdiff_t>(drop_y) * width) + x];
        --count;
      }
    }
  }
}

// Three-pass separable box blur with the given total radius. Each pass
// uses radius/3 (rounded up) so the cumulative effect approximates a
// Gaussian with the requested sigma.
void blur3(std::vector<std::uint8_t>& buf, int width, int height, int total_radius) noexcept {
  if (total_radius <= 0 || width == 0 || height == 0) {
    return;
  }
  const int per_pass = std::max(1, (total_radius + 2) / 3);
  std::vector<std::uint8_t> scratch(buf.size());
  for (int pass = 0; pass < 3; ++pass) {
    box_blur_h(buf.data(), scratch.data(), width, height, per_pass);
    box_blur_v(scratch.data(), buf.data(), width, height, per_pass);
  }
}

// ── Cache entry ──────────────────────────────────────────────
//
// Each entry holds the pre-blurred premultiplied PRGB32 pixels. The
// shadow color from the theme has already been baked in — the renderer
// just needs to SRC_OVER these pixels onto its target.

struct CacheEntry {
  ShadowKey key;
  std::vector<std::uint8_t> pixels;  // PRGB32, key.width * key.height * 4 bytes.
};

std::vector<std::uint8_t> render_shadow(const ShadowKey& key, const Theme& theme) {
  // Patch is the full shadow region; the panel sits inset by
  // shadow_extent on each side.
  const int w = static_cast<int>(key.width);
  const int h = static_cast<int>(key.height);
  const int extent = std::max(0, theme.shadow_extent);

  // Single-channel alpha mask of the panel rectangle (rounded corners).
  std::vector<std::uint8_t> mask(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
  const int panel_w = std::max(0, w - (2 * extent));
  const int panel_h = std::max(0, h - (2 * extent));
  const int radius = std::min({theme.corner_radius, panel_w / 2, panel_h / 2});

  if (panel_w > 0 && panel_h > 0) {
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        if (inside_rounded_rect(x, y, extent, extent, panel_w, panel_h, radius)) {
          mask[(static_cast<std::size_t>(y) * w) + x] = 0xFF;
        }
      }
    }
  }

  // Blur the mask to soften the panel into a halo. Unfocused windows
  // get a slightly wider, lower-contrast blur.
  blur3(mask, w, h, extent);

  // Convert single-channel alpha → premultiplied PRGB32, tinted by
  // theme.colors.shadow. Focused elevation keeps the theme's shadow
  // alpha at 1.0; blurred (unfocused) elevation drops to 70% to read
  // as recessed.
  const Color& s = theme.colors.shadow;
  const float intensity = key.elevation == Elevation::Focused ? 1.0F : 0.7F;
  const auto base_a = static_cast<float>(s.a);

  std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U, 0);
  for (int i = 0; i < w * h; ++i) {
    const auto m = static_cast<float>(mask[static_cast<std::size_t>(i)]) / 255.0F;
    const auto a8 = static_cast<std::uint8_t>(m * base_a * intensity);
    // Premultiplied: each color channel scaled by alpha/255.
    const auto pr = static_cast<std::uint8_t>((s.r * a8) / 255U);
    const auto pg = static_cast<std::uint8_t>((s.g * a8) / 255U);
    const auto pb = static_cast<std::uint8_t>((s.b * a8) / 255U);
    // PRGB32 byte layout on LE hosts: B, G, R, A (so a uint32_t reads
    // back as 0xAARRGGBB). Match BLRgba32's layout exactly so the
    // renderer's SRC_OVER blend sees the bytes the way it expects.
    const std::size_t off = static_cast<std::size_t>(i) * 4U;
    out[off + 0U] = pb;
    out[off + 1U] = pg;
    out[off + 2U] = pr;
    out[off + 3U] = a8;
  }
  return out;
}

}  // namespace

std::uint64_t theme_id(const Theme& theme) noexcept {
  std::uint64_t h = k_fnv_offset;
  // Geometry / radii.
  h = hash_u32(h, static_cast<std::uint32_t>(theme.corner_radius));
  h = hash_u32(h, static_cast<std::uint32_t>(theme.shadow_extent));
  // noise_amplitude as raw bytes — equal doubles hash equal regardless
  // of platform byte order because we hash via fnv1a's byte loop.
  const double na = theme.noise_amplitude;
  h = fnv1a(&na, sizeof(na), h);
  // Title bar geometry. Font / font_size live here too; the renderer
  // uses font_size for laying out, so a font-size change should
  // invalidate cached shadows that were rendered with a different
  // title-bar height (height = font_size + padding in glass).
  h = hash_u32(h, static_cast<std::uint32_t>(theme.title_bar.height));
  h = hash_u32(h, static_cast<std::uint32_t>(theme.title_bar.font_size));
  // Visual color tokens.
  h = hash_color(h, theme.colors.panel_top);
  h = hash_color(h, theme.colors.panel_bottom);
  h = hash_color(h, theme.colors.rim_focused);
  h = hash_color(h, theme.colors.rim_blurred);
  h = hash_color(h, theme.colors.shadow);
  h = hash_color(h, theme.colors.title_text);
  h = hash_color(h, theme.colors.title_shadow);
  h = hash_color(h, theme.buttons.close.fill);
  h = hash_color(h, theme.buttons.close.hover);
  h = hash_color(h, theme.buttons.minimize.fill);
  h = hash_color(h, theme.buttons.minimize.hover);
  h = hash_color(h, theme.buttons.maximize.fill);
  h = hash_color(h, theme.buttons.maximize.hover);
  return h;
}

namespace {

struct KeyHash {
  std::size_t operator()(const ShadowKey& k) const noexcept {
    std::uint64_t h = k_fnv_offset;
    h = hash_u32(h, k.width);
    h = hash_u32(h, k.height);
    h = hash_u32(h, static_cast<std::uint32_t>(k.elevation));
    h = fnv1a(&k.theme_id, sizeof(k.theme_id), h);
    return static_cast<std::size_t>(h);
  }
};

}  // namespace

struct ShadowCache::Impl {
  using List = std::list<CacheEntry>;
  using Map = std::unordered_map<ShadowKey, List::iterator, KeyHash>;

  std::size_t capacity{ShadowCache::k_default_capacity};
  List order;  // front = most recently used.
  Map index;
};

ShadowCache::ShadowCache(std::size_t capacity) : impl_(new Impl{}) {
  impl_->capacity = capacity == 0 ? ShadowCache::k_default_capacity : capacity;
}

ShadowCache::~ShadowCache() {
  delete impl_;
}

ShadowCache::ShadowCache(ShadowCache&& other) noexcept : impl_(other.impl_) {
  other.impl_ = nullptr;
}

ShadowCache& ShadowCache::operator=(ShadowCache&& other) noexcept {
  if (this != &other) {
    delete impl_;
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

void ShadowCache::clear() noexcept {
  if (impl_ == nullptr) {
    return;
  }
  impl_->index.clear();
  impl_->order.clear();
}

std::size_t ShadowCache::size() const noexcept {
  return impl_ == nullptr ? 0U : impl_->order.size();
}

std::size_t ShadowCache::capacity() const noexcept {
  return impl_ == nullptr ? 0U : impl_->capacity;
}

bool ShadowCache::contains(const ShadowKey& key) const noexcept {
  return impl_ != nullptr && impl_->index.find(key) != impl_->index.end();
}

bool ShadowCache::blit_into(const ShadowKey& key, const Theme& theme, const ShadowDest& dst) {
  if (impl_ == nullptr || dst.pixels == nullptr || dst.width == 0 || dst.height == 0 ||
      key.width == 0 || key.height == 0) {
    return false;
  }

  // Lookup. On hit, splice to front (LRU update). On miss, render into
  // a new entry, evict if over capacity.
  auto it = impl_->index.find(key);
  if (it == impl_->index.end()) {
    CacheEntry entry;
    entry.key = key;
    entry.pixels = render_shadow(key, theme);
    if (entry.pixels.empty()) {
      return false;
    }
    impl_->order.push_front(std::move(entry));
    impl_->index[key] = impl_->order.begin();
    while (impl_->order.size() > impl_->capacity) {
      impl_->index.erase(impl_->order.back().key);
      impl_->order.pop_back();
    }
    it = impl_->index.find(key);
  } else {
    impl_->order.splice(impl_->order.begin(), impl_->order, it->second);
  }

  const CacheEntry& entry = *it->second;

  // Manual SRC_OVER alpha composite: dst = src + dst * (1 - src.a).
  // Both src and dst are PRGB32 (premultiplied) so the math is the
  // straightforward Porter-Duff over.
  const std::uint32_t copy_w = std::min(dst.width, key.width);
  const std::uint32_t copy_h = std::min(dst.height, key.height);
  for (std::uint32_t y = 0; y < copy_h; ++y) {
    const std::uint8_t* src_row =
        entry.pixels.data() + (static_cast<std::size_t>(y) * key.width * 4U);
    std::uint8_t* dst_row = dst.pixels + (static_cast<std::size_t>(y) * dst.stride);
    for (std::uint32_t x = 0; x < copy_w; ++x) {
      const std::uint8_t* sp = src_row + (static_cast<std::size_t>(x) * 4U);
      std::uint8_t* dp = dst_row + (static_cast<std::size_t>(x) * 4U);
      const std::uint32_t inv_a = 255U - sp[3];
      dp[0] = static_cast<std::uint8_t>(sp[0] + ((dp[0] * inv_a) / 255U));
      dp[1] = static_cast<std::uint8_t>(sp[1] + ((dp[1] * inv_a) / 255U));
      dp[2] = static_cast<std::uint8_t>(sp[2] + ((dp[2] * inv_a) / 255U));
      dp[3] = static_cast<std::uint8_t>(sp[3] + ((dp[3] * inv_a) / 255U));
    }
  }
  return true;
}

}  // namespace drm::csd
