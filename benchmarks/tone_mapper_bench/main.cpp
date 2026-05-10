// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// tone_mapper_bench — microbenchmark for `drm::display::ToneMapper`.
//
// Times the LUT-accelerated CPU tone-mapping path across 1080p / 4K
// frames in each of the three configured directions. The design's
// acceptance criterion is "under 8 ms / 1080p frame on a 2-GHz
// Cortex-A53"; on desktop x86 the LUT path should land well below
// that.
//
// Usage:
//   tone_mapper_bench [--width W] [--height H] [--frames N]
//                     [--csv]
//
// Default 1920x1080, 16 frames each direction. CSV output is one
// line per direction with columns:
//   direction, width, height, total_us, us_per_frame, mpixels_per_sec

#include <drm-cxx/display/tone_mapper.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t k_default_width = 1920;
constexpr std::size_t k_default_height = 1080;
constexpr int k_default_frames = 16;

struct Args {
  std::size_t width{k_default_width};
  std::size_t height{k_default_height};
  int frames{k_default_frames};
  bool csv{false};
};

Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--width" && (i + 1) < argc) {
      a.width = static_cast<std::size_t>(std::atoi(argv[++i]));
    } else if (arg == "--height" && (i + 1) < argc) {
      a.height = static_cast<std::size_t>(std::atoi(argv[++i]));
    } else if (arg == "--frames" && (i + 1) < argc) {
      a.frames = std::atoi(argv[++i]);
    } else if (arg == "--csv") {
      a.csv = true;
    }
  }
  return a;
}

// Fill the buffer with a deterministic test pattern: a horizontal
// gradient plus per-row color shift, so the optimizer can't const-
// fold the input across pixels.
std::vector<std::uint64_t> make_pattern(std::size_t w, std::size_t h) {
  std::vector<std::uint64_t> px(w * h);
  std::mt19937 rng(0xCAFEU);
  for (std::size_t y = 0; y < h; ++y) {
    for (std::size_t x = 0; x < w; ++x) {
      const auto r = static_cast<std::uint16_t>((x * 65535U) / std::max<std::size_t>(w - 1, 1));
      const auto g = static_cast<std::uint16_t>((y * 65535U) / std::max<std::size_t>(h - 1, 1));
      const auto b = static_cast<std::uint16_t>(rng() & 0xFFFFU);
      const std::uint16_t a = 0xFFFFU;
      px[(y * w) + x] = static_cast<std::uint64_t>(r) | (static_cast<std::uint64_t>(g) << 16U) |
                        (static_cast<std::uint64_t>(b) << 32U) |
                        (static_cast<std::uint64_t>(a) << 48U);
    }
  }
  return px;
}

double benchmark(const drm::display::ToneMapper& tm, std::vector<std::uint64_t>& src,
                 std::vector<std::uint64_t>& dst, int frames) {
  const auto start = std::chrono::steady_clock::now();
  for (int f = 0; f < frames; ++f) {
    tm.apply(src.data(), dst.data(), src.size());
  }
  const auto end = std::chrono::steady_clock::now();
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  return static_cast<double>(us);
}

void run_direction(const char* name, const drm::display::ToneMapper& tm,
                   std::vector<std::uint64_t>& src, std::vector<std::uint64_t>& dst, int frames,
                   std::size_t w, std::size_t h, bool csv) {
  const double total_us = benchmark(tm, src, dst, frames);
  const double per_frame_us = total_us / frames;
  const double mpix_per_sec =
      (static_cast<double>(w) * static_cast<double>(h) * frames) / (total_us / 1e6) / 1e6;
  if (csv) {
    std::printf("%s,%zu,%zu,%.0f,%.1f,%.2f\n", name, w, h, total_us, per_frame_us, mpix_per_sec);
  } else {
    std::printf("%-20s  %zux%zu  total=%.1f ms  per-frame=%.2f ms  throughput=%.1f Mpix/s\n", name,
                w, h, total_us / 1000.0, per_frame_us / 1000.0, mpix_per_sec);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const auto args = parse_args(argc, argv);
  auto src = make_pattern(args.width, args.height);
  std::vector<std::uint64_t> dst(src.size());

  if (args.csv) {
    std::printf("direction,width,height,total_us,us_per_frame,mpixels_per_sec\n");
  } else {
    std::printf("ToneMapper benchmark — %zux%zu, %d frames each direction\n", args.width,
                args.height, args.frames);
  }

  run_direction("bt709->bt2020_pq", drm::display::ToneMapper::bt709_to_bt2020_pq(100.0F), src, dst,
                args.frames, args.width, args.height, args.csv);
  run_direction(
      "bt2020_pq->bt709 (Reinhard)",
      drm::display::ToneMapper::bt2020_pq_to_bt709(100.0F, drm::display::ToneMapCurve::Reinhard),
      src, dst, args.frames, args.width, args.height, args.csv);
  run_direction(
      "bt2020_pq->bt709 (Hable)",
      drm::display::ToneMapper::bt2020_pq_to_bt709(100.0F, drm::display::ToneMapCurve::Hable), src,
      dst, args.frames, args.width, args.height, args.csv);
  run_direction("hlg->bt709", drm::display::ToneMapper::hlg_to_bt709(100.0F), src, dst, args.frames,
                args.width, args.height, args.csv);

  return 0;
}
