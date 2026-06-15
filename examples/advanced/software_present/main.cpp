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
// classic embedded software-display trade. By default it reports per-frame damage
// (the moving box's old + new bands) so a damage-aware driver repaints only those
// rows — on an mipi-dbi SPI panel this cuts the flush from the full frame to a
// band, shrinking the tear window; --no-damage forces full-frame commits. Needs
// DRM master; run from a free VT, or pass --no-seat on a headless board (skips
// libseat, opens DRM directly).
//
//   ./software_present [/dev/dri/cardN] [frames] [--vsync] [--rgb565] [--no-damage] [--no-seat]

#include "../../common/open_output.hpp"

#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/present/dumb_scanout_sink.hpp>
#include <drm-cxx/present/scanout_format.hpp>
#include <drm-cxx/scene/buffer_source.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <array>
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
  bool force_rgb565 = false;
  bool use_damage = true;
  int frames = 120;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--vsync") == 0) {
      vsync = true;
    } else if (std::strcmp(argv[i], "--rgb565") == 0) {
      force_rgb565 = true;
    } else if (std::strcmp(argv[i], "--no-damage") == 0) {  // force full-frame commits
      use_damage = false;
    } else if (std::strcmp(argv[i], "--no-seat") == 0) {   // handled by open_output
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

  // Negotiate a format this renderer can produce (XRGB8888 or RGB565) against the
  // plane — e.g. tilcdc has no XRGB8888. --rgb565 forces 16-bpp.
  drm::present::DumbScanoutSink::Config cfg;
  cfg.buffers = 3;  // headroom so present() rarely stalls on a busy slot
  if (force_rgb565) {
    cfg.drm_format = DRM_FORMAT_RGB565;
  } else {
    const std::array<std::uint32_t, 2> prefs{DRM_FORMAT_XRGB8888, DRM_FORMAT_RGB565};
    const std::uint32_t fourcc =
        drm::present::negotiate_scanout_format(dev, output->crtc_id, prefs);
    cfg.drm_format = fourcc != 0 ? fourcc : DRM_FORMAT_XRGB8888;
  }
  auto sink_r = drm::present::DumbScanoutSink::create(dev, output->crtc_id, output->connector_id,
                                                      output->mode, cfg);
  if (!sink_r) {
    drm::println(stderr, "software_present: sink: {}", sink_r.error().message());
    return EXIT_FAILURE;
  }
  auto sink = std::move(*sink_r);
  const bool rgb565 = sink->drm_format() == DRM_FORMAT_RGB565;

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

  // Box scaled to ~1/4 of the panel's smaller dimension, so it stays a small
  // moving box on tiny displays (e.g. 128x128 SPI panels) instead of a fixed
  // 200px box that overflows the frame buffer (negative `by`) and segfaults.
  const std::int32_t k_box = static_cast<std::int32_t>(w < h ? w : h) / 4;
  const std::int32_t span_x =
      (static_cast<std::int32_t>(w) > k_box) ? static_cast<std::int32_t>(w) - k_box : 1;
  const std::int32_t by = (static_cast<std::int32_t>(h) - k_box) / 2;
  const std::size_t pixels = static_cast<std::size_t>(w) * h;

  // Only the box moves frame-to-frame (the grey background is constant), so the
  // damage is the box's old and new positions — report those so a damage-aware
  // driver (e.g. an mipi-dbi SPI panel) repaints ~one band instead of the whole
  // frame, cutting bandwidth and the tear window. --no-damage forces full frames.
  std::int32_t prev_bx = 0;

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

    // Damage = the erased (old) box band + the redrawn (new) box band.
    const std::array<drm::scene::DamageRect, 2> damage{{
        {prev_bx, by, static_cast<std::uint32_t>(k_box), static_cast<std::uint32_t>(k_box)},
        {bx, by, static_cast<std::uint32_t>(k_box), static_cast<std::uint32_t>(k_box)},
    }};
    auto r = use_damage ? sink->present({frame.data(), frame.size()}, stride, damage, flags, pf)
                        : sink->present({frame.data(), frame.size()}, stride, flags, pf);
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
    prev_bx = bx;  // the box now on screen — next frame's damage erases from here

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
