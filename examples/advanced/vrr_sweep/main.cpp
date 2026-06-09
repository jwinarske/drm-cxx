// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// vrr_sweep — measure whether VRR actually tracks the present rate, not just
// whether VRR_ENABLED is accepted.
//
// Method (no external instrument — the signal is the page-flip completion
// timestamp): drive a continuous present (DumbRingSource alternates buffers so
// every commit is a real flip), paced to a target rate, with
// DRM_MODE_PAGE_FLIP_EVENT. The Δ between consecutive flip completions is the
// panel's actual refresh interval. Sweep target rates across and around the
// panel's EDID vertical-refresh range and report measured Hz + jitter.
//
//   VRR on  -> measured Δ tracks the target, low jitter.
//   VRR off -> Δ quantizes to the fixed vblank -> high jitter / can't track.
//
// Pass --vrr to enable VRR_ENABLED; omit it for the fixed-refresh control run.
//
//   ./vrr_sweep [/dev/dri/cardN] [--vrr]

#include "../../common/open_output.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/display/connector_info.hpp>
#include <drm-cxx/display/edid.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/present/buffer_ring.hpp>
#include <drm-cxx/present/dumb_ring_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <time.h>  // NOLINT(modernize-deprecated-headers) — POSIX clock_nanosleep/CLOCK_MONOTONIC live here
#include <utility>
#include <vector>

namespace {

// Fetch + parse the connector's EDID to read its vertical-refresh range.
std::optional<drm::display::VrefreshRange> edid_range(int fd, std::uint32_t connector_id) {
  drmModeConnectorPtr conn = drmModeGetConnectorCurrent(fd, connector_id);
  if (conn == nullptr) {
    return std::nullopt;
  }
  std::optional<drm::display::VrefreshRange> out;
  for (int i = 0; i < conn->count_props; ++i) {
    drmModePropertyPtr p = drmModeGetProperty(fd, conn->props[i]);
    if (p == nullptr) {
      continue;
    }
    if (std::strcmp(p->name, "EDID") == 0 && conn->prop_values[i] != 0) {
      if (drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(fd, conn->prop_values[i])) {
        auto info = drm::display::parse_edid(drm::span<const std::uint8_t>(
            static_cast<const std::uint8_t*>(blob->data), blob->length));
        if (info.has_value() && info->vrefresh_range.has_value()) {
          out = info->vrefresh_range;
        }
        drmModeFreePropertyBlob(blob);
      }
    }
    drmModeFreeProperty(p);
  }
  drmModeFreeConnector(conn);
  return out;
}

[[nodiscard]] std::uint64_t now_ns() {
  struct timespec ts{};
  // NOLINTNEXTLINE(misc-include-cleaner) — CLOCK_MONOTONIC via <time.h>
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL) +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

}  // namespace

int main(int argc, char** argv) {
  bool want_vrr = false;
  {
    int write = 1;
    for (int i = 1; i < argc; ++i) {
      if (std::string_view{argv[i]} == "--vrr") {
        want_vrr = true;
      } else {
        argv[write++] = argv[i];
      }
    }
    argc = write;
  }

  auto output = drm::examples::open_and_pick_output(argc, argv);
  if (!output) {
    drm::println(stderr, "vrr_sweep: no usable output");
    return EXIT_FAILURE;
  }
  auto& dev = output->device;
  const std::uint32_t w = output->mode.hdisplay;
  const std::uint32_t h = output->mode.vdisplay;

  auto src_r = drm::present::DumbRingSource::create(dev, w, h, DRM_FORMAT_XRGB8888, 3);
  if (!src_r) {
    drm::println(stderr, "vrr_sweep: ring source: {}", src_r.error().message());
    return EXIT_FAILURE;
  }
  auto src = std::move(*src_r);
  auto* ring = src.get();

  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = output->crtc_id;
  cfg.connector_id = output->connector_id;
  cfg.mode = output->mode;
  auto scene_r = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_r) {
    drm::println(stderr, "vrr_sweep: scene: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_r);

  drm::scene::LayerDesc desc;
  desc.source = std::move(src);
  desc.display.dst_rect = {0, 0, w, h};
  if (auto r = scene->add_layer(std::move(desc)); !r) {
    drm::println(stderr, "vrr_sweep: add_layer: {}", r.error().message());
    return EXIT_FAILURE;
  }

  const auto range = edid_range(dev.fd(), output->connector_id);
  if (range) {
    drm::println("vrr_sweep: {}x{} — EDID vrefresh range {}–{} Hz — VRR {}", w, h, range->min_hz,
                 range->max_hz, want_vrr ? "ON" : "off (control)");
  } else {
    drm::println("vrr_sweep: {}x{} — no EDID vrefresh range — VRR {}", w, h,
                 want_vrr ? "ON" : "off (control)");
  }
  if (want_vrr) {
    scene->set_vrr_enabled(true);
  }

  drm::PageFlip page_flip(dev);
  std::vector<std::uint64_t> stamps;
  page_flip.set_handler([&](std::uint32_t /*crtc*/, std::uint64_t /*seq*/, std::uint64_t ts) {
    stamps.push_back(ts != 0 ? ts : now_ns());
  });

  constexpr std::array<int, 7> targets{30, 40, 48, 60, 72, 90, 110};
  constexpr int k_frames = 90;

  for (const int target : targets) {
    const std::uint64_t interval_ns = 1'000'000'000ULL / static_cast<std::uint64_t>(target);
    stamps.clear();
    std::uint64_t next = now_ns();
    int color = 0;
    for (int f = 0; f < k_frames; ++f) {
      next += interval_ns;
      struct timespec abst{};
      abst.tv_sec = static_cast<time_t>(next / 1'000'000'000ULL);
      abst.tv_nsec = static_cast<long>(next % 1'000'000'000ULL);
      // NOLINTNEXTLINE(misc-include-cleaner) — CLOCK_MONOTONIC/TIMER_ABSTIME via <time.h>
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abst, nullptr);

      const std::uint32_t c = 0x00202020U + static_cast<std::uint32_t>((color += 8) & 0xFF);
      (void)ring->paint([&](drm::BufferMapping& m, const drm::present::Repaint& /*rp*/) {
        std::memset(m.pixels().data(), static_cast<int>(c & 0xFF), m.pixels().size());
        return std::vector<drm::present::Rect>{};  // full-frame: force a real flip
      });
      if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
        drm::println(stderr, "vrr_sweep: commit @ {}Hz: {}", target, r.error().message());
        return EXIT_FAILURE;
      }
      if (auto r = page_flip.dispatch(1000); !r) {
        drm::println(stderr, "vrr_sweep: flip dispatch @ {}Hz: {}", target, r.error().message());
        return EXIT_FAILURE;
      }
    }

    // Inter-completion intervals -> measured Hz (mean) + jitter (stddev, ms).
    double sum_ms = 0.0;
    double sum_sq = 0.0;
    int n = 0;
    for (std::size_t i = 1; i < stamps.size(); ++i) {
      const double ms = static_cast<double>(stamps[i] - stamps[i - 1]) / 1.0e6;
      sum_ms += ms;
      sum_sq += ms * ms;
      ++n;
    }
    if (n > 0) {
      const double mean = sum_ms / n;
      const double var = (sum_sq / n) - (mean * mean);
      const double jitter = std::sqrt(var > 0.0 ? var : 0.0);
      drm::println(
          "  target {:>3} Hz -> measured {:5.1f} Hz  (interval {:5.2f} ms, jitter {:4.2f} ms)",
          target, 1000.0 / mean, mean, jitter);
    }
  }

  drm::println("vrr_sweep: done");
  return EXIT_SUCCESS;
}
