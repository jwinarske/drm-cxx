// SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// tests/integration/test_cursor_renderer_legacy.cpp
//
// Fail-fast contract for drm::cursor::Renderer on cursor-less display
// controllers (i.MX LCDIF/LCDIFv3, tilcdc): when no CURSOR and no OVERLAY plane
// can serve the CRTC, Renderer::create() must NOT hand back a doomed legacy
// Renderer that only fails later at the first drmModeSetCursor — it must fail at
// create() so the caller can fall back to a composited cursor.
//
// The legacy probe runs only when selection falls through to drmModeSetCursor,
// which never happens on a card that exposes a CURSOR/OVERLAY plane. So:
//   - cursor-less card (real i.MX/tilcdc) → assert create() returns an error;
//   - cursor-capable card (amdgpu/i915/vkms in CI) → skip (the new branch is
//     unreachable there, and asserting create() success would need DRM master).
// Skips cleanly when no card qualifies.

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/cursor/renderer.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <xf86drmMode.h>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

static int g_fail = 0;
#define CHECK(x)                                                        \
  do {                                                                  \
    if (!(x)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
      ++g_fail;                                                         \
    }                                                                   \
  } while (0)

namespace {

struct CrtcPick {
  std::uint32_t crtc_id{0};
  std::uint32_t crtc_index{0};
  bool ok{false};
};

// First CRTC on the card, with its index into drmModeRes::crtcs[] (the index
// space PlaneCapabilities::possible_crtcs / compatible_with_crtc use).
CrtcPick first_crtc(int fd) {
  CrtcPick pick;
  drmModeRes* res = drmModeGetResources(fd);
  if (res == nullptr) {
    return pick;
  }
  if (res->count_crtcs > 0) {
    pick.crtc_id = static_cast<std::uint32_t>(res->crtcs[0]);
    pick.crtc_index = 0;
    pick.ok = true;
  }
  drmModeFreeResources(res);
  return pick;
}

// A card is "cursor-capable" for this CRTC if the registry exposes any CURSOR
// or OVERLAY plane compatible with it — i.e. select_plane() would find a real
// atomic plane and never reach the legacy fallthrough.
bool crtc_has_cursor_or_overlay(const drm::Device& dev, std::uint32_t crtc_index) {
  auto reg = drm::planes::PlaneRegistry::enumerate(dev);
  if (!reg) {
    return false;
  }
  for (const auto* cap : reg->for_crtc(crtc_index)) {
    if (cap->type == drm::planes::DRMPlaneType::CURSOR ||
        cap->type == drm::planes::DRMPlaneType::OVERLAY) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  for (int i = 0; i < 8; ++i) {
    auto dev = drm::Device::open("/dev/dri/card" + std::to_string(i));
    if (!dev) {
      continue;
    }
    if (auto r = dev->enable_universal_planes(); !r) {
      continue;  // can't see PRIMARY/CURSOR planes without the cap
    }
    (void)dev->enable_atomic();

    const CrtcPick pick = first_crtc(dev->fd());
    if (!pick.ok) {
      continue;  // headless / no CRTC on this node
    }

    if (crtc_has_cursor_or_overlay(*dev, pick.crtc_index)) {
      // The legacy fallthrough is unreachable here, so the new probe never
      // runs. Asserting create() success would require DRM master (the atomic
      // path issues a TEST_ONLY commit), which CI may not hold — skip.
      std::printf(
          "test_cursor_renderer_legacy: card%d is cursor-capable; "
          "legacy probe unreachable, skipping\n",
          i);
      return 0;
    }

    // Cursor-less controller: this is the case the fix targets.
    std::printf(
        "test_cursor_renderer_legacy: card%d has no CURSOR/OVERLAY plane "
        "for crtc %u; exercising fail-fast\n",
        i, pick.crtc_id);

    drm::cursor::RendererConfig cfg;
    cfg.crtc_id = pick.crtc_id;

    // Default config permits the legacy fallthrough; create() must still fail
    // (no_such_device with master, permission_denied without) rather than
    // returning a Renderer that dies on its first commit_position().
    cfg.allow_legacy = true;
    auto legacy_ok = drm::cursor::Renderer::create(*dev, cfg);
    CHECK(!legacy_ok.has_value());

    // And with legacy disabled, the pre-existing "no atomic plane" path also
    // errors — never returns a usable Renderer.
    cfg.allow_legacy = false;
    auto no_legacy = drm::cursor::Renderer::create(*dev, cfg);
    CHECK(!no_legacy.has_value());

    return g_fail == 0 ? 0 : 1;
  }

  std::puts("test_cursor_renderer_legacy: no DRM card qualified; skipping");
  return 0;
}
