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
// Needs DRM master, so run it from a free VT.
//
//   ./software_present [/dev/dri/cardN] [frames]

#include "../../common/open_output.hpp"

#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/present/dumb_scanout_sink.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

int main(int argc, char** argv) {
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

  drm::println("software_present: {}x{} @ {}Hz — CPU frames via dumb ring + atomic flip", w, h,
               sink->refresh_hz());

  const std::uint32_t stride = w * 4U;
  std::vector<std::byte> frame(static_cast<std::size_t>(stride) * h);
  auto* px = reinterpret_cast<std::uint32_t*>(frame.data());

  constexpr std::int32_t k_box = 200;
  const std::int32_t span_x =
      (static_cast<std::int32_t>(w) > k_box) ? static_cast<std::int32_t>(w) - k_box : 1;
  const std::int32_t by = (static_cast<std::int32_t>(h) - k_box) / 2;
  const int frames = (argc > 2) ? std::atoi(argv[2]) : 120;

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

    auto r = sink->present({frame.data(), frame.size()}, stride);
    if (!r) {
      if (r.error() == std::make_error_code(std::errc::resource_unavailable_try_again)) {
        ::usleep(16000);  // ring momentarily full — retry this frame next vblank
        --f;
        continue;
      }
      drm::println(stderr, "software_present: present f={}: {}", f, r.error().message());
      return EXIT_FAILURE;
    }
    ::usleep(16000);
  }

  drm::println("software_present: presented {} frames", frames);
  return EXIT_SUCCESS;
}
