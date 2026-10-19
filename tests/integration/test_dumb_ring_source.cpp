// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// tests/integration/test_dumb_ring_source.cpp
//
// DumbRingSource round-trip + buffer-age contract on a real DRM device. Needs
// only CREATE_DUMB + AddFB2 (no DRM master, no commit), so it runs on any card
// — amdgpu/i915 locally, vkms in CI. Skips cleanly when no card supports dumb
// buffers. The ring's age/repaint math itself is covered by test_buffer_ring;
// this checks the source glue: lazy slot alloc, the paint→acquire→release cycle,
// full-frame-first, and that reuse eventually drives a partial (non-full) repaint
// whose region surfaces as AcquiredBuffer.damage.

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/present/buffer_ring.hpp>
#include <drm-cxx/present/dumb_ring_source.hpp>
#include <drm-cxx/scene/buffer_source.hpp>

#include <drm_fourcc.h>
#include <xf86drmMode.h>

#include <cstdio>
#include <cstring>
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

// First card whose dumb-buffer ring allocates — amdgpu/i915/vkms all qualify.
std::optional<drm::Device> open_dumb_device() {
  for (int i = 0; i < 8; ++i) {
    auto dev = drm::Device::open("/dev/dri/card" + std::to_string(i));
    if (!dev) {
      continue;
    }
    // Skip render-only nodes (e.g. PowerVR pvrsrvkm on StarFive, which is card0
    // there): they accept CREATE_DUMB so the probe below would pass, but they
    // have no CRTC to scan out and the FB/paint path later fails with ENOSYS.
    // A scanout test must land on a KMS-capable card.
    drmModeRes* res = drmModeGetResources(dev->fd());
    if (res == nullptr) {
      continue;
    }
    const bool kms_capable = res->count_crtcs > 0;
    drmModeFreeResources(res);
    if (!kms_capable) {
      continue;
    }
    auto probe = drm::present::DumbRingSource::create(*dev, 64, 64, DRM_FORMAT_XRGB8888, 3);
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
    std::puts("test_dumb_ring_source: no dumb-capable DRM card; skipping");
    return 0;
  }

  auto src_r = drm::present::DumbRingSource::create(*dev, 128, 128, DRM_FORMAT_XRGB8888, 3);
  CHECK(src_r.has_value());
  if (!src_r) {
    return 1;
  }
  auto src = std::move(*src_r);

  std::optional<drm::scene::AcquiredBuffer> prev;
  bool saw_full = false;
  bool saw_partial = false;
  for (int frame = 0; frame < 8; ++frame) {
    // Each frame changes its own 16x16 cell, so a frame's change is never the
    // stale region a reused slot owes.
    const drm::present::Rect changed{frame * 16, 0, 16, 16};
    bool partial = false;
    auto pr = src->paint([&](drm::BufferMapping& m, const drm::present::Repaint& rp) {
      // Exactly one of {full, non-empty region} holds.
      CHECK(rp.full != !rp.region.empty());
      if (rp.full) {
        saw_full = true;
        std::memset(m.pixels().data(), 0, m.pixels().size());
      } else {
        saw_partial = true;
        partial = true;
      }
      return std::vector<drm::present::Rect>{changed};
    });
    CHECK(pr.has_value());

    auto acq = src->acquire();
    CHECK(acq.has_value());
    if (acq) {
      CHECK(acq->fb_id != 0);
      // The damage names what differs from the buffer on screen: this frame's
      // own change, not the stale region just repainted (#367).
      if (partial) {
        bool covered = false;
        for (const auto& d : acq->damage) {
          covered = covered || (d.x <= changed.x && d.y <= changed.y &&
                                d.x + static_cast<std::int32_t>(d.w) >=
                                    changed.x + static_cast<std::int32_t>(changed.width) &&
                                d.y + static_cast<std::int32_t>(d.h) >=
                                    changed.y + static_cast<std::int32_t>(changed.height));
        }
        CHECK(covered);
      }
      // Release the PREVIOUS frame's buffer (mirrors the scene retiring it once
      // the next frame is committed) so the ring always has a free slot.
      if (prev) {
        src->release(std::move(*prev));
      }
      prev = std::move(*acq);
    }
  }
  if (prev) {
    src->release(std::move(*prev));
  }

  // Fresh slots full-repaint; once the 3-slot ring starts reusing presented
  // slots, at least one frame must do a partial (age-driven) repaint.
  CHECK(saw_full);
  CHECK(saw_partial);

  // A resume re-allocates the buffers blank: the first paint after it must be
  // a full repaint, not the partial one the ring's old history would offer (#369).
  src->on_session_paused();
  CHECK(src->on_session_resumed(*dev).has_value());
  bool full_after_resume = false;
  CHECK(src->paint([&](drm::BufferMapping&, const drm::present::Repaint& rp) {
             full_after_resume = rp.full;
             return std::vector<drm::present::Rect>{};
           })
            .has_value());
  CHECK(full_after_resume);

  if (g_fail == 0) {
    std::puts("test_dumb_ring_source: OK");
  }
  return g_fail == 0 ? 0 : 1;
}
