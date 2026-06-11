// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// software_present — present CPU-rendered frames via present::DumbScanoutSink.
//
// Renders a moving box on the CPU into a host-memory frame each iteration and
// hands the finished frame to the sink, which copies it into a dumb-buffer ring
// and drives one atomic flip. This exercises the software-present path (no GL /
// Vulkan / GBM) end to end: a software rasterizer's "here is a finished frame"
// model rather than a paint-into-the-mapping callback.
//
// With --vsync the loop paces to the display: present() arms a page-flip event
// and the loop blocks on the PageFlip dispatcher until the flip completes,
// instead of a fixed timer. Needs DRM master, so run it from a free VT.
//
//   ./software_present [/dev/dri/cardN] [frames] [--vsync]

#include "../../common/open_output.hpp"

#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/present/dumb_scanout_sink.hpp>

#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

int main(int argc, char** argv) {
  bool vsync = false;
  int frames = 120;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--vsync") == 0) {
      vsync = true;
    } else if (const int n = std::atoi(argv[i]); n > 0) {  // device path atoi's to 0
      frames = n;
    }
  }

  auto output = drm::examples::open_and_pick_output(argc, argv);
  if (!output) {
    drm::println(stderr, "software_present: no usable output");
    return EXIT_FAILURE;
  }
  auto& dev = output->device;
  const auto w = static_cast<std::uint32_t>(output->mode.hdisplay);
  const auto h = static_cast<std::uint32_t>(output->mode.vdisplay);

  drm::present::DumbScanoutSink::Config cfg;
  cfg.buffers = 3;  // headroom so present() rarely stalls on a busy slot
  auto sink_r = drm::present::DumbScanoutSink::create(dev, output->crtc_id, output->connector_id,
                                                      output->mode, cfg);
  if (!sink_r) {
    drm::println(stderr, "software_present: sink: {}", sink_r.error().message());
    return EXIT_FAILURE;
  }
  auto sink = std::move(*sink_r);

  // Vsync pacing: arm DRM_MODE_PAGE_FLIP_EVENT on each present and block on the
  // PageFlip dispatcher until the flip lands, rather than a fixed sleep.
  std::optional<drm::PageFlip> page_flip;
  bool flip_done = false;
  if (vsync) {
    page_flip.emplace(dev);
    page_flip->set_handler([&](std::uint32_t /*crtc*/, std::uint64_t /*seq*/,
                               std::uint64_t /*ts*/) { flip_done = true; });
  }

  drm::println(
      "software_present: {}x{} @ {}Hz — CPU frames via dumb ring + atomic flip ({} pacing)", w, h,
      sink->refresh_hz(), vsync ? "vsync" : "timer");

  const std::uint32_t stride = w * 4U;
  std::vector<std::byte> frame(static_cast<std::size_t>(stride) * h);
  auto* px = reinterpret_cast<std::uint32_t*>(frame.data());

  constexpr std::int32_t k_box = 200;
  const std::int32_t span_x =
      (static_cast<std::int32_t>(w) > k_box) ? static_cast<std::int32_t>(w) - k_box : 1;
  const std::int32_t by = (static_cast<std::int32_t>(h) - k_box) / 2;

  for (int f = 0; f < frames; ++f) {
    // Software render the whole frame: grey background + a moving green box.
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
      px[i] = 0xFF202020U;
    }
    const std::int32_t bx = (f * 13) % span_x;
    const std::uint32_t color = 0xFF00FF00U | static_cast<std::uint32_t>((f * 4) & 0xFF);
    for (std::int32_t y = by; y < by + k_box; ++y) {
      for (std::int32_t x = bx; x < bx + k_box; ++x) {
        px[(static_cast<std::size_t>(y) * w) + static_cast<std::size_t>(x)] = color;
      }
    }

    const std::uint32_t flags = vsync ? DRM_MODE_PAGE_FLIP_EVENT : 0U;
    drm::PageFlip* const pf = vsync ? &*page_flip : nullptr;
    if (vsync) {
      flip_done = false;
    }

    auto r = sink->present({frame.data(), frame.size()}, stride, flags, pf);
    if (!r) {
      if (r.error() == std::make_error_code(std::errc::resource_unavailable_try_again)) {
        // Ring momentarily full — drain a pending flip (vsync) or sleep, then retry.
        if (vsync) {
          (void)page_flip->dispatch(1000);
        } else {
          ::usleep(16000);
        }
        --f;
        continue;
      }
      drm::println(stderr, "software_present: present f={}: {}", f, r.error().message());
      return EXIT_FAILURE;
    }

    if (vsync) {
      // Pace to the display: wait for the flip-complete event.
      while (!flip_done) {
        if (auto d = page_flip->dispatch(1000); !d) {
          drm::println(stderr, "software_present: flip event timed out at f={}", f);
          break;
        }
      }
    } else {
      ::usleep(16000);
    }
  }

  drm::println("software_present: presented {} frames", frames);
  return EXIT_SUCCESS;
}
