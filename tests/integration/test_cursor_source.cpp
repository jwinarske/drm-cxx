// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// tests/integration/test_cursor_source.cpp
//
// CursorSource round-trip on a real DRM device. Needs only CREATE_DUMB + AddFB2
// (no master, no commit), so it runs on any dumb-capable card — amdgpu/i915
// locally, vkms in CI. Skips cleanly when no card qualifies. Verifies the sprite
// is blitted into the scanout buffer intact (row-by-row, respecting stride) and
// that the hotspot + dimensions surface through the accessors.

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/cursor_source.hpp>

#include <drm_fourcc.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

static int g_fail = 0;
#define CHECK(x)                                                        \
  do {                                                                  \
    if (!(x)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
      ++g_fail;                                                         \
    }                                                                   \
  } while (0)

namespace {

constexpr std::uint32_t kW = 4;
constexpr std::uint32_t kH = 4;

// Distinct opaque value per pixel so a stride/offset bug shows up.
std::vector<std::uint32_t> make_pattern() {
  std::vector<std::uint32_t> px(static_cast<std::size_t>(kW) * kH);
  for (std::uint32_t i = 0; i < px.size(); ++i) {
    px[i] = 0xFF000000U | (i * 0x010203U);
  }
  return px;
}

std::optional<drm::Device> open_dumb_device() {
  const auto pattern = make_pattern();
  for (int i = 0; i < 8; ++i) {
    auto dev = drm::Device::open("/dev/dri/card" + std::to_string(i));
    if (!dev) {
      continue;
    }
    // Skip render-only nodes (e.g. PowerVR pvrsrvkm on StarFive): they accept
    // CREATE_DUMB but have no CRTC to scan out, so the cursor FB/paint path
    // later fails. A scanout test must land on a KMS-capable card.
    drmModeRes* res = drmModeGetResources(dev->fd());
    if (res == nullptr) {
      continue;
    }
    const bool kms_capable = res->count_crtcs > 0;
    drmModeFreeResources(res);
    if (!kms_capable) {
      continue;
    }
    auto probe = drm::scene::CursorSource::create_argb(*dev, pattern, kW, kH, 0, 0);
    if (probe) {
      return std::move(*dev);
    }
  }
  return std::nullopt;
}

}  // namespace

int main() {
  auto dev = open_dumb_device();
  if (!dev) {
    std::puts("test_cursor_source: no dumb-capable DRM card; skipping");
    return 0;
  }

  const auto pattern = make_pattern();
  auto src_r = drm::scene::CursorSource::create_argb(*dev, pattern, kW, kH, 1, 2);
  CHECK(src_r.has_value());
  if (!src_r) {
    return 1;
  }
  auto src = std::move(*src_r);

  CHECK(src->hotspot_x() == 1);
  CHECK(src->hotspot_y() == 2);
  CHECK(src->width() == kW);
  CHECK(src->height() == kH);

  auto acq = src->acquire();
  CHECK(acq.has_value());
  CHECK(acq && acq->fb_id != 0);

  // The sprite must land in the scanout buffer unchanged, row-by-row.
  auto m = src->map(drm::MapAccess::Read);
  CHECK(m.has_value());
  if (m) {
    const auto px = m->pixels();
    const auto stride = m->stride();
    for (std::uint32_t y = 0; y < kH; ++y) {
      const auto* row = reinterpret_cast<const std::uint32_t*>(
          px.data() + (static_cast<std::size_t>(y) * stride));
      for (std::uint32_t x = 0; x < kW; ++x) {
        CHECK(row[x] == pattern[(static_cast<std::size_t>(y) * kW) + x]);
      }
    }
  }

  if (g_fail != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_fail);
    return 1;
  }
  std::puts("all cursor_source tests passed");
  return 0;
}
