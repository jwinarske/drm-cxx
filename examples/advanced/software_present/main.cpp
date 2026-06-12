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
// With --vsync the loop paces to the display via a page-flip event instead of a
// fixed timer. With --rgb565 the sink scans out RGB565 (16 bpp) instead of
// XRGB8888 (32 bpp), halving the dumb-buffer footprint and copy bandwidth — the
// classic embedded software-display trade. Needs DRM master; run from a free VT.
//
//   ./software_present [/dev/dri/cardN] [frames] [--vsync] [--rgb565]

#include "../../common/open_output.hpp"

#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/present/dumb_scanout_sink.hpp>

#include <drm_fourcc.h>
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

namespace {

constexpr std::uint16_t rgb565_px(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  return static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
constexpr std::uint32_t argb_px(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  return 0xFF000000U | (static_cast<std::uint32_t>(r) << 16) |
         (static_cast<std::uint32_t>(g) << 8) | b;
}

}  // namespace

int main(int argc, char** argv) {
  bool vsync = false;
  bool rgb565 = false;
  int frames = 120;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--vsync") == 0) {
      vsync = true;
    } else if (std::strcmp(argv[i], "--rgb565") == 0) {
      rgb565 = true;
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
  if (rgb565) {
    cfg.drm_format = DRM_FORMAT_RGB565;
  }
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

  drm::println("software_present: {}x{} @ {}Hz — {} via dumb ring + atomic flip ({} pacing)", w, h,
               sink->refresh_hz(), rgb565 ? "RGB565" : "XRGB8888", vsync ? "vsync" : "timer");

  const std::uint32_t bytes_pp = rgb565 ? 2U : 4U;
  const std::uint32_t stride = w * bytes_pp;
  std::vector<std::byte> frame(static_cast<std::size_t>(stride) * h);

  constexpr std::int32_t k_box = 200;
  const std::int32_t span_x =
      (static_cast<std::int32_t>(w) > k_box) ? static_cast<std::int32_t>(w) - k_box : 1;
  const std::int32_t by = (static_cast<std::int32_t>(h) - k_box) / 2;
  const std::size_t pixels = static_cast<std::size_t>(w) * h;

  for (int f = 0; f < frames; ++f) {
    // Software render the whole frame: grey background + a moving green box. The
    // green channel animates so a static screenshot still shows motion stepping.
    const std::int32_t bx = (f * 13) % span_x;
    const auto g = static_cast<std::uint8_t>(0x80 + ((f * 2) & 0x7F));
    if (rgb565) {
      auto* p = reinterpret_cast<std::uint16_t*>(frame.data());
      const std::uint16_t bg = rgb565_px(0x20, 0x20, 0x20);
      const std::uint16_t box = rgb565_px(0, g, 0);
      for (std::size_t i = 0; i < pixels; ++i) {
        p[i] = bg;
      }
      for (std::int32_t y = by; y < by + k_box; ++y) {
        for (std::int32_t x = bx; x < bx + k_box; ++x) {
          p[(static_cast<std::size_t>(y) * w) + static_cast<std::size_t>(x)] = box;
        }
      }
    } else {
      auto* p = reinterpret_cast<std::uint32_t*>(frame.data());
      const std::uint32_t bg = argb_px(0x20, 0x20, 0x20);
      const std::uint32_t box = argb_px(0, g, 0);
      for (std::size_t i = 0; i < pixels; ++i) {
        p[i] = bg;
      }
      for (std::int32_t y = by; y < by + k_box; ++y) {
        for (std::int32_t x = bx; x < bx + k_box; ++x) {
          p[(static_cast<std::size_t>(y) * w) + static_cast<std::size_t>(x)] = box;
        }
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
