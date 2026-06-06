// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// examples/allocator/compressed_scanout/main.cpp
//
// End-to-end use of format_mod.hpp:
//   1. read the primary plane's IN_FORMATS (FormatTable)
//   2. build a candidate modifier list for XRGB8888, COMPRESSION-first
//   3. let GBM allocate the best layout it can satisfy (ScanoutBuffer)
//   4. report the chosen modifier, its BandwidthClass, and the per-frame
//      scanout-read cost vs linear (scanout_cost_bytes)
//   5. prove the plane can scan it out with an ATOMIC TEST_ONLY commit -- the
//      ground truth that catches a lying IN_FORMATS -- and memoize the verdict
//   6. if LINEAR, fill a test pattern and present; if compressed, stop at the
//      verified test (real content needs a GPU producer -- see the offload demos)
//
// Run:  ./compressed_scanout [/dev/dri/card0]

#include "../../common/kms_present.hpp"

#include <drm-cxx/fmt/format_mod.hpp>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fmt = drm::fmt;

namespace {

// Order candidates COMPRESSION first, then tiling, LINEAR guaranteed last.
std::vector<fmt::Modifier> rank_candidates(const fmt::FormatTable& tbl, std::uint32_t fourcc) {
  std::vector<fmt::Modifier> comp;
  std::vector<fmt::Modifier> tile;
  bool have_linear = false;
  for (fmt::Modifier const m : tbl.modifiers_for(fourcc)) {
    switch (fmt::classify(m)) {
      case fmt::BandwidthClass::Compression:
        comp.push_back(m);
        break;
      case fmt::BandwidthClass::Tiling:
        tile.push_back(m);
        break;
      case fmt::BandwidthClass::Linear:
        have_linear = true;
        break;
    }
  }
  std::vector<fmt::Modifier> out;
  out.insert(out.end(), comp.begin(), comp.end());
  // Diagnostic: offer GBM ONLY the display-scannable compression modifiers, to
  // prove a real dcc=1 buffer survives the atomic TEST_ONLY (the driver otherwise
  // downgrades a fresh CPU buffer to plain tiling). Set DRM_FMT_FORCE_COMPRESSION=1.
  if (std::getenv("DRM_FMT_FORCE_COMPRESSION") != nullptr) {
    return out;
  }
  out.insert(out.end(), tile.begin(), tile.end());
  if (have_linear || out.empty()) {
    out.push_back(fmt::Modifier{DRM_FORMAT_MOD_LINEAR});
  }
  return out;
}

void fill_xrgb_gradient(gbm_bo* bo, std::uint32_t w, std::uint32_t h) {
  std::uint32_t stride = 0;
  void* map = nullptr;
  void* data = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_WRITE, &stride, &map);
  if (data == nullptr) {
    return;
  }
  auto* base = static_cast<std::uint8_t*>(data);
  for (std::uint32_t y = 0; y < h; ++y) {
    auto* row = reinterpret_cast<std::uint32_t*>(base + (static_cast<std::size_t>(y) * stride));
    for (std::uint32_t x = 0; x < w; ++x) {
      row[x] = ((x * 255 / w) << 16) | ((y * 255 / h) << 8);
    }
  }
  gbm_bo_unmap(bo, map);
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "/dev/dri/card0";

  int const fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    std::perror("open");
    return 1;
  }
  drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

  auto target = kms::pick_target(fd);
  if (!target) {
    std::fprintf(stderr, "no connected output / primary plane found\n");
    close(fd);
    return 1;
  }
  const std::uint32_t w = target->mode.hdisplay;
  const std::uint32_t h = target->mode.vdisplay;
  std::printf("output: connector %u, crtc %u, primary plane %u, %ux%u\n", target->connector_id,
              target->crtc_id, target->primary_plane, w, h);

  auto tbl = fmt::FormatTable::from_plane(fd, target->primary_plane);
  if (!tbl) {
    std::fprintf(stderr, "no IN_FORMATS: %s\n", tbl.error().message().c_str());
    close(fd);
    return 1;
  }

  auto candidates = rank_candidates(*tbl, DRM_FORMAT_XRGB8888);
  std::printf("candidates (best first):\n");
  for (fmt::Modifier const m : candidates) {
    std::printf("  %s\n", fmt::describe(m).c_str());
  }

  gbm_device* gbm = gbm_create_device(fd);
  if (gbm == nullptr) {
    std::fprintf(stderr, "gbm_create_device failed\n");
    close(fd);
    return 1;
  }

  fmt::ScanoutBuffer::Desc desc;  // explicit init (C++17: no designated inits)
  desc.width = w;
  desc.height = h;
  desc.fourcc = DRM_FORMAT_XRGB8888;
  desc.modifiers = candidates;  // std::vector -> drm::span
  auto buf = fmt::ScanoutBuffer::create(gbm, fd, desc);
  if (!buf) {
    std::fprintf(stderr, "ScanoutBuffer::create: %s\n", buf.error().message().c_str());
    gbm_device_destroy(gbm);
    close(fd);
    return 1;
  }

  const auto cls = fmt::classify(buf->modifier());
  const std::uint64_t linear_bytes =
      fmt::scanout_cost_bytes(w, h, DRM_FORMAT_XRGB8888, fmt::BandwidthClass::Linear);
  const std::uint64_t this_bytes = fmt::scanout_cost_bytes(w, h, DRM_FORMAT_XRGB8888, cls);
  std::printf("chosen: %s  (%u plane(s))\n", fmt::describe(buf->modifier()).c_str(),
              buf->plane_count());
  std::printf("scanout read/frame: %llu B vs %llu B linear",
              static_cast<unsigned long long>(this_bytes),
              static_cast<unsigned long long>(linear_bytes));
  if (this_bytes < linear_bytes) {
    std::printf("  (~%llu MB/s saved @ 60Hz)\n",
                static_cast<unsigned long long>((linear_bytes - this_bytes) * 60 / 1000000));
  } else {
    std::printf("  (no scanout saving on this layout)\n");
  }

  // Ground truth: atomic TEST_ONLY, then memoize the verdict.
  fmt::ModifierProbeCache probe;
  int const test = kms::commit_fb(fd, *target, buf->fb_id(),
                                  DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET);
  probe.record(target->crtc_id, target->primary_plane, DRM_FORMAT_XRGB8888, buf->modifier(),
               test == 0);
  std::printf("TEST_ONLY commit: %s\n", test == 0 ? "ACCEPTED" : "REJECTED");

  if (test != 0) {
    std::fprintf(stderr,
                 "plane advertised this modifier but the commit was rejected -- "
                 "drop the edge and fall back.\n");
  } else if (cls == fmt::BandwidthClass::Linear) {
    fill_xrgb_gradient(buf->bo(), w, h);
    int const r = kms::commit_fb(fd, *target, buf->fb_id(), DRM_MODE_ATOMIC_ALLOW_MODESET);
    std::printf("present (linear): %s\n", r == 0 ? "ON SCREEN" : std::strerror(-r));
    if (r == 0) {
      std::printf("hold 3s...\n");
      sleep(3);
    }
  } else {
    std::printf(
        "compressed layout verified for scanout; real content requires a GPU "
        "producer rendering into this buffer (see the offload examples).\n");
  }

  // `buf` owns a bo + FB that borrow `gbm` and `fd`, so it MUST be torn down
  // before them. Release it explicitly here -- letting it live to function-scope
  // end would run gbm_bo_destroy() after gbm_device_destroy(): a use-after-free.
  {
    const fmt::ScanoutBuffer released = std::move(*buf);
  }
  gbm_device_destroy(gbm);
  close(fd);
  return 0;
}

// ===========================================================================
// Production integration with the drm-cxx native allocator (phases 1-5).
// Compile-guarded out of the runnable example because it depends on the local
// allocator headers and the planes:: API surface.
// ===========================================================================
#ifdef DRM_CXX_WITH_ALLOCATOR
#include <drm-cxx/drm-cxx.hpp>

drm::expected<void, std::error_code> build_with_allocator(drm::Device& dev, std::uint32_t crtc_id,
                                                          const fmt::ScanoutBuffer& buf,
                                                          std::uint32_t w, std::uint32_t h) {
  // PlaneRegistry should carry a FormatTable per plane (phase 1): enumerate()
  // reads IN_FORMATS once and stashes it, so layer_fits_plane() becomes the edge
  // predicate in the Hopcroft-Karp pre-solve.
  auto registry = drm::planes::PlaneRegistry::enumerate(dev).value();

  drm::planes::Layer comp_layer;
  drm::planes::Output output(crtc_id, comp_layer);

  auto& layer = output.add_layer();
  layer.set_property("FB_ID", buf.fb_id())
      .set_property("CRTC_X", 0)
      .set_property("CRTC_Y", 0)
      .set_property("CRTC_W", w)
      .set_property("CRTC_H", h)
      .set_property("SRC_X", 0)
      .set_property("SRC_Y", 0)
      .set_property("SRC_W", std::uint64_t(w) << 16)
      .set_property("SRC_H", std::uint64_t(h) << 16);

  drm::planes::Allocator allocator(dev, registry);
  drm::AtomicRequest req(dev);

  // apply() runs the matching; its failure-memoization layer is the natural home
  // for ModifierProbeCache, and a test commit inside apply() is ground truth.
  auto assigned = allocator.apply(output, req, /*flags=*/0);
  if (!assigned) return drm::unexpected(assigned.error());
  return req.commit(DRM_MODE_ATOMIC_ALLOW_MODESET);  // adapt to your API surface
}
#endif