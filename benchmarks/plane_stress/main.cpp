// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// plane_stress — CLI-configurable synthetic benchmark for LayerScene.
//
// Drives a scene through a configurable workload (layer count, format
// pool, tile size, churn mode and rate) and emits one CSV row per
// committed frame plus a final summary. The numbers feed Phase 4
// outreach: blog posts, READMEs, and the project's "what does the
// allocator actually do under load?" answer.
//
// Usage:
//
//   plane_stress [--device PATH]
//                [--layers N]                  number of scene layers (default 4)
//                [--formats argb,xrgb,abgr]    format pool (default argb,xrgb)
//                [--size WxH]                  layer tile size (default 256x256)
//                [--churn MODE]                none|move|alpha|reshade|toggle (default move)
//                [--churn-rate N]              one churn op every N frames (default 1)
//                [--duration S]                seconds to run (default 5)
//                [--csv PATH]                  CSV output path (default stdout)
//                [--quiet]                     suppress per-frame log; CSV only
//
// CSV columns (one row per commit):
//   frame, wall_us, commit_us, test_commits, assigned, composited,
//   unassigned, props, fbs, buckets
//
// The benchmark requires a real KMS device (libseat or root). Output
// is visible on the connected display while the benchmark runs.
//
// `--csv PATH` writes to whatever path the user supplies; intended for
// developer use, not setuid contexts. If you ever package this binary
// with elevated privileges, sanitise the path or drop the `--csv` flag.

#include "common/format_probe.hpp"
#include "common/open_output.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_handle.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

enum class ChurnMode : std::uint8_t {
  None,     // pure steady-state — no mutation between commits
  Move,     // shift one tile's dst_rect each churn tick
  Alpha,    // bump one tile's alpha each churn tick
  Reshade,  // dirty one tile's buffer (forces FB_ID re-emit)
  Toggle,   // remove + re-add one layer cyclically
};

struct Options {
  std::string device;
  std::size_t layers{4U};
  std::vector<std::uint32_t> formats{DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888};
  std::uint32_t tile_w{256U};
  std::uint32_t tile_h{256U};
  ChurnMode churn{ChurnMode::Move};
  std::uint32_t churn_rate{1U};
  double duration_s{5.0};
  std::string csv_path;  // empty = stdout
  bool quiet{false};
};

const char* churn_name(ChurnMode m) noexcept {
  switch (m) {
    case ChurnMode::None:
      return "none";
    case ChurnMode::Move:
      return "move";
    case ChurnMode::Alpha:
      return "alpha";
    case ChurnMode::Reshade:
      return "reshade";
    case ChurnMode::Toggle:
      return "toggle";
  }
  return "?";
}

std::optional<ChurnMode> parse_churn(std::string_view s) noexcept {
  if (s == "none") {
    return ChurnMode::None;
  }
  if (s == "move") {
    return ChurnMode::Move;
  }
  if (s == "alpha") {
    return ChurnMode::Alpha;
  }
  if (s == "reshade") {
    return ChurnMode::Reshade;
  }
  if (s == "toggle") {
    return ChurnMode::Toggle;
  }
  return std::nullopt;
}

std::optional<std::uint32_t> parse_format(std::string_view s) noexcept {
  if (s == "argb" || s == "ARGB8888") {
    return DRM_FORMAT_ARGB8888;
  }
  if (s == "xrgb" || s == "XRGB8888") {
    return DRM_FORMAT_XRGB8888;
  }
  if (s == "abgr" || s == "ABGR8888") {
    return DRM_FORMAT_ABGR8888;
  }
  if (s == "xbgr" || s == "XBGR8888") {
    return DRM_FORMAT_XBGR8888;
  }
  return std::nullopt;
}

const char* format_name(std::uint32_t fmt) noexcept {
  switch (fmt) {
    case DRM_FORMAT_ARGB8888:
      return "ARGB";
    case DRM_FORMAT_XRGB8888:
      return "XRGB";
    case DRM_FORMAT_ABGR8888:
      return "ABGR";
    case DRM_FORMAT_XBGR8888:
      return "XBGR";
    default:
      return "????";
  }
}

bool parse_size(std::string_view s, std::uint32_t& w, std::uint32_t& h) noexcept {
  const auto x = s.find('x');
  if (x == std::string_view::npos || x == 0U || x + 1U >= s.size()) {
    return false;
  }
  const auto wstr = std::string(s.substr(0, x));
  const auto hstr = std::string(s.substr(x + 1));
  try {
    w = static_cast<std::uint32_t>(std::stoul(wstr));
    h = static_cast<std::uint32_t>(std::stoul(hstr));
  } catch (...) {
    return false;
  }
  return w > 0 && h > 0;
}

// std::stoul / std::stod throw on garbage input; wrap so the caller
// can decline rather than abort.
std::optional<std::uint32_t> parse_u32(std::string_view s) noexcept {
  try {
    return static_cast<std::uint32_t>(std::stoul(std::string(s)));
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<double> parse_double(std::string_view s) noexcept {
  try {
    return std::stod(std::string(s));
  } catch (...) {
    return std::nullopt;
  }
}

std::vector<std::uint32_t> parse_format_list(std::string_view s) {
  std::vector<std::uint32_t> out;
  std::size_t start = 0;
  while (start <= s.size()) {
    const auto end = s.find(',', start);
    const auto tok =
        s.substr(start, end == std::string_view::npos ? s.size() - start : end - start);
    if (auto f = parse_format(tok)) {
      out.push_back(*f);
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1U;
  }
  return out;
}

// Argv-style flag parser. Stops at the first positional argv we don't
// recognise so the caller can still pass [device] through to
// open_and_pick_output.
bool parse_args(int argc, char** argv, Options& opt, int& consumed) {
  consumed = 1;  // skip program name
  for (int i = 1; i < argc; ++i) {
    const std::string_view a{argv[i]};
    auto take = [&](std::string_view& dst) -> bool {
      if (i + 1 >= argc) {
        return false;
      }
      dst = argv[++i];
      consumed = i + 1;
      return true;
    };
    if (a == "--device") {
      std::string_view v;
      if (!take(v)) {
        return false;
      }
      opt.device = std::string(v);
    } else if (a == "--layers") {
      std::string_view v;
      if (!take(v)) {
        return false;
      }
      const auto n = parse_u32(v);
      if (!n || *n == 0U) {
        drm::println(stderr, "--layers: expected positive integer, got '{}'", v);
        return false;
      }
      opt.layers = *n;
    } else if (a == "--formats") {
      std::string_view v;
      if (!take(v)) {
        return false;
      }
      auto list = parse_format_list(v);
      if (list.empty()) {
        drm::println(stderr, "--formats: no valid format token in '{}'", v);
        return false;
      }
      opt.formats = std::move(list);
    } else if (a == "--size") {
      std::string_view v;
      if (!take(v)) {
        return false;
      }
      if (!parse_size(v, opt.tile_w, opt.tile_h)) {
        drm::println(stderr, "--size: expected WxH, got '{}'", v);
        return false;
      }
    } else if (a == "--churn") {
      std::string_view v;
      if (!take(v)) {
        return false;
      }
      auto m = parse_churn(v);
      if (!m) {
        drm::println(stderr, "--churn: expected one of none|move|alpha|reshade|toggle, got '{}'",
                     v);
        return false;
      }
      opt.churn = *m;
    } else if (a == "--churn-rate") {
      std::string_view v;
      if (!take(v)) {
        return false;
      }
      const auto n = parse_u32(v);
      if (!n) {
        drm::println(stderr, "--churn-rate: expected non-negative integer, got '{}'", v);
        return false;
      }
      opt.churn_rate = std::max<std::uint32_t>(1U, *n);
    } else if (a == "--duration") {
      std::string_view v;
      if (!take(v)) {
        return false;
      }
      const auto x = parse_double(v);
      if (!x || *x <= 0.0) {
        drm::println(stderr, "--duration: expected positive number, got '{}'", v);
        return false;
      }
      opt.duration_s = *x;
    } else if (a == "--csv") {
      std::string_view v;
      if (!take(v)) {
        return false;
      }
      opt.csv_path = std::string(v);
    } else if (a == "--quiet") {
      opt.quiet = true;
      consumed = i + 1;
    } else if (a == "--help" || a == "-h") {
      return false;
    } else {
      // Unknown flag — leave it for open_and_pick_output (a positional
      // device path). Keep `consumed` at the first unconsumed slot so
      // the helper sees this argv onward.
      break;
    }
  }
  return true;
}

void print_usage() {
  drm::println(stderr,
               "Usage: plane_stress [--layers N] [--formats argb,xrgb] [--size WxH]\n"
               "                    [--churn none|move|alpha|reshade|toggle]\n"
               "                    [--churn-rate N] [--duration S] [--csv PATH]\n"
               "                    [--quiet] [--device PATH | /dev/dri/cardN]");
}

// One layer in the scene.
struct Tile {
  drm::scene::DumbBufferSource* src{nullptr};
  drm::scene::LayerHandle handle{};
  std::uint32_t fmt{0};
  std::int32_t x{0};
  std::int32_t y{0};
  std::uint16_t alpha{0xFFFFU};
  std::uint32_t shade{0xFF202020U};
};

// 32-bpp solid fill. Used for the initial paint and Reshade churn.
void paint_solid(drm::BufferMapping& map, std::uint32_t argb) noexcept {
  const auto pixels = map.pixels();
  const auto stride = map.stride();
  const std::uint32_t w = map.width();
  const std::uint32_t h = map.height();
  if (w == 0 || h == 0 || stride < w * 4U) {
    return;
  }
  for (std::uint32_t y = 0; y < h; ++y) {
    auto* row =
        reinterpret_cast<std::uint32_t*>(pixels.data() + (static_cast<std::size_t>(y) * stride));
    std::fill_n(row, w, argb);
  }
}

drm::expected<Tile, std::error_code> build_tile(drm::Device& dev, drm::scene::LayerScene& scene,
                                                std::uint32_t idx, std::uint32_t fmt,
                                                std::uint32_t tile_w, std::uint32_t tile_h,
                                                std::int32_t x, std::int32_t y) {
  auto src = drm::scene::DumbBufferSource::create(dev, tile_w, tile_h, fmt);
  if (!src) {
    return drm::unexpected<std::error_code>(src.error());
  }
  auto* src_ptr = src->get();
  drm::scene::LayerDesc desc;
  desc.source = std::move(*src);
  desc.display.src_rect = drm::scene::Rect{0, 0, tile_w, tile_h};
  desc.display.dst_rect = drm::scene::Rect{x, y, tile_w, tile_h};
  // Spread tiles across zpos starting at 3 (clears amdgpu's PRIMARY=2
  // pin); the allocator is free to re-shuffle if planes don't have zpos.
  desc.display.zpos = 3 + static_cast<int>(idx);
  desc.content_type = drm::planes::ContentType::Generic;
  auto h = scene.add_layer(std::move(desc));
  if (!h) {
    return drm::unexpected<std::error_code>(h.error());
  }
  // Distinct shade per tile so the screen output is visually verifiable.
  constexpr std::array<std::uint32_t, 8> palette{
      0xFFE57373U, 0xFF81C784U, 0xFF64B5F6U, 0xFFFFD54FU,
      0xFFBA68C8U, 0xFF4DB6ACU, 0xFFFF8A65U, 0xFF7986CBU,
  };
  Tile t;
  t.src = src_ptr;
  t.handle = *h;
  t.fmt = fmt;
  t.x = x;
  t.y = y;
  t.shade = palette.at(idx % palette.size());
  if (auto m = src_ptr->map(drm::MapAccess::Write); m) {
    paint_solid(*m, t.shade);
  }
  return t;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  int consumed = 1;
  if (!parse_args(argc, argv, opt, consumed)) {
    print_usage();
    return EXIT_FAILURE;
  }

  // Forward whatever's left of argv to the helper so the user can
  // still pass /dev/dri/cardN positionally.
  std::vector<char*> tail;
  tail.reserve(static_cast<std::size_t>(argc - consumed) + 1);
  tail.push_back(argv[0]);
  for (int i = consumed; i < argc; ++i) {
    tail.push_back(argv[i]);
  }
  if (!opt.device.empty()) {
    tail.push_back(opt.device.data());
  }
  auto output = drm::examples::open_and_pick_output(static_cast<int>(tail.size()), tail.data());
  if (!output) {
    return EXIT_FAILURE;
  }
  auto& dev = output->device;
  const drmModeModeInfo mode = output->mode;
  const std::uint32_t fb_w = mode.hdisplay;
  const std::uint32_t fb_h = mode.vdisplay;
  drm::println(stderr, "plane_stress: {}x{}@{}Hz on connector {} / CRTC {}", fb_w, fb_h,
               mode.vrefresh, output->connector_id, output->crtc_id);

  // Surface capability gaps that affect the stress shape (alpha layers
  // routing through composition, missing zpos defeating layer ordering).
  drm::examples::warn_compat(drm::examples::probe_output(dev, output->crtc_id),
                             {.wants_alpha_overlays = true,
                              .wants_explicit_zpos = true,
                              .wants_overlay_count = opt.layers - 1U});

  const drm::scene::LayerScene::Config cfg{output->crtc_id, output->connector_id, mode};
  auto scene_r = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_r) {
    drm::println(stderr, "LayerScene::create: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_r);

  // Lay tiles out left-to-right starting at (0,0); rows wrap when the
  // next tile would extend off-screen. Tiles overlap the right edge by
  // up to one tile so churn=move never has to clamp into negative space.
  std::vector<Tile> tiles;
  tiles.reserve(opt.layers);
  std::int32_t x = 0;
  std::int32_t y = 0;
  for (std::size_t i = 0; i < opt.layers; ++i) {
    if (x + static_cast<std::int32_t>(opt.tile_w) > static_cast<std::int32_t>(fb_w)) {
      x = 0;
      y += static_cast<std::int32_t>(opt.tile_h);
    }
    if (y + static_cast<std::int32_t>(opt.tile_h) > static_cast<std::int32_t>(fb_h)) {
      drm::println(stderr,
                   "plane_stress: tile grid ({} layers @ {}x{}) overflows {}x{} screen — "
                   "reduce --layers or --size",
                   opt.layers, opt.tile_w, opt.tile_h, fb_w, fb_h);
      return EXIT_FAILURE;
    }
    const auto fmt = opt.formats.at(i % opt.formats.size());
    auto t =
        build_tile(dev, *scene, static_cast<std::uint32_t>(i), fmt, opt.tile_w, opt.tile_h, x, y);
    if (!t) {
      drm::println(stderr, "build_tile {}: {}", i, t.error().message());
      return EXIT_FAILURE;
    }
    tiles.push_back(*t);
    x += static_cast<std::int32_t>(opt.tile_w);
  }

  drm::PageFlip page_flip(dev);
  bool flip_pending = false;
  page_flip.set_handler([&](std::uint32_t /*c*/, std::uint64_t /*s*/,
                            std::uint64_t /*t*/) noexcept { flip_pending = false; });

  // CSV sink. ofstream when --csv was set; std::cout otherwise. Buffered
  // writes are fine — we explicitly flush before exit.
  std::ofstream csv_file;
  std::ostream* csv = &std::cout;
  if (!opt.csv_path.empty()) {
    csv_file.open(opt.csv_path);
    if (!csv_file) {
      drm::println(stderr, "plane_stress: cannot open {} for writing", opt.csv_path);
      return EXIT_FAILURE;
    }
    csv = &csv_file;
  }
  *csv << "frame,wall_us,commit_us,test_commits,assigned,composited,unassigned,props,fbs,buckets\n";

  drm::println(
      stderr, "plane_stress: layers={} formats=[{}{}{}{}] tile={}x{} churn={} rate={} duration={}s",
      opt.layers, format_name(opt.formats.at(0)), opt.formats.size() > 1 ? "," : "",
      opt.formats.size() > 1 ? format_name(opt.formats.at(1)) : "",
      opt.formats.size() > 2 ? "+more" : "", opt.tile_w, opt.tile_h, churn_name(opt.churn),
      opt.churn_rate, opt.duration_s);

  using clock = std::chrono::steady_clock;
  const auto t_start = clock::now();
  std::uint64_t frame_idx = 0;
  std::uint64_t cum_assigned = 0;
  std::uint64_t cum_composited = 0;
  std::uint64_t cum_test = 0;
  std::uint64_t cum_props = 0;
  std::uint64_t cum_commit_us = 0;

  // Apply one churn op against tile `idx`. The mutation is recorded
  // back into the Tile struct so subsequent churn ops compound.
  auto apply_churn = [&](std::uint64_t tick) {
    if (opt.churn == ChurnMode::None || tiles.empty()) {
      return;
    }
    const std::size_t idx = tick % tiles.size();
    auto& t = tiles[idx];
    auto* layer = scene->get_layer(t.handle);
    switch (opt.churn) {
      case ChurnMode::None:
        break;
      case ChurnMode::Move: {
        // Bounce horizontally by 16 px, wrapping at the screen edge so
        // we never clip dst_rect into negative space.
        constexpr std::int32_t step = 16;
        t.x += step;
        if (t.x + static_cast<std::int32_t>(opt.tile_w) > static_cast<std::int32_t>(fb_w)) {
          t.x = 0;
        }
        if (layer != nullptr) {
          layer->set_dst_rect(drm::scene::Rect{t.x, t.y, opt.tile_w, opt.tile_h});
        }
        break;
      }
      case ChurnMode::Alpha: {
        // Cycle alpha through 0xFFFF, 0xC000, 0x8000, 0x4000.
        constexpr std::array<std::uint16_t, 4> levels{0xFFFFU, 0xC000U, 0x8000U, 0x4000U};
        t.alpha = levels.at(tick % levels.size());
        if (layer != nullptr) {
          layer->set_alpha(t.alpha);
        }
        break;
      }
      case ChurnMode::Reshade: {
        // Bump the lower 24 bits of the fill colour each tick so the
        // CPU repaint dirties the FB and forces an FB_ID re-emit.
        t.shade = (t.shade & 0xFF000000U) | ((t.shade + 0x010205U) & 0x00FFFFFFU);
        if (auto m = t.src->map(drm::MapAccess::Write); m) {
          paint_solid(*m, t.shade);
        }
        break;
      }
      case ChurnMode::Toggle: {
        // Remove the layer and rebuild it. Exercises add_layer +
        // remove_layer hot paths against the warm-start cache.
        const auto fmt = t.fmt;
        const std::int32_t x = t.x;
        const std::int32_t y = t.y;
        scene->remove_layer(t.handle);
        auto rebuilt = build_tile(dev, *scene, static_cast<std::uint32_t>(idx), fmt, opt.tile_w,
                                  opt.tile_h, x, y);
        if (rebuilt) {
          t = *rebuilt;
        }
        break;
      }
    }
  };

  // First commit primes the scanout. Drain the page flip before the
  // measured loop so frame 0 onward is in steady state.
  if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    drm::println(stderr, "first commit: {}", r.error().message());
    return EXIT_FAILURE;
  }
  flip_pending = true;
  while (flip_pending) {
    if (auto r = page_flip.dispatch(-1); !r) {
      drm::println(stderr, "page_flip dispatch (warm-up): {}", r.error().message());
      return EXIT_FAILURE;
    }
  }

  while (true) {
    const auto now = clock::now();
    const auto wall_us =
        std::chrono::duration_cast<std::chrono::microseconds>(now - t_start).count();
    if (std::chrono::duration<double>(now - t_start).count() >= opt.duration_s) {
      break;
    }

    if (frame_idx % opt.churn_rate == 0U) {
      apply_churn(frame_idx);
    }

    const auto t_commit_start = clock::now();
    auto report = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip);
    if (!report) {
      drm::println(stderr, "commit (frame {}): {}", frame_idx, report.error().message());
      return EXIT_FAILURE;
    }
    const auto commit_us =
        std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t_commit_start)
            .count();
    flip_pending = true;

    *csv << frame_idx << ',' << wall_us << ',' << commit_us << ',' << report->test_commits_issued
         << ',' << report->layers_assigned << ',' << report->layers_composited << ','
         << report->layers_unassigned << ',' << report->properties_written << ','
         << report->fbs_attached << ',' << report->composition_buckets << '\n';

    cum_assigned += report->layers_assigned;
    cum_composited += report->layers_composited;
    cum_test += report->test_commits_issued;
    cum_props += report->properties_written;
    cum_commit_us += static_cast<std::uint64_t>(commit_us);

    if (!opt.quiet && (frame_idx < 5U || frame_idx % 60U == 0U)) {
      drm::println(stderr,
                   "frame {:>5} commit_us={:>5} test={} assigned={} composited={} props={} fbs={}",
                   frame_idx, commit_us, report->test_commits_issued, report->layers_assigned,
                   report->layers_composited, report->properties_written, report->fbs_attached);
    }

    while (flip_pending) {
      if (auto r = page_flip.dispatch(-1); !r) {
        drm::println(stderr, "page_flip dispatch: {}", r.error().message());
        return EXIT_FAILURE;
      }
    }
    ++frame_idx;
  }

  csv->flush();

  if (frame_idx == 0U) {
    drm::println(stderr, "plane_stress: no frames committed (--duration too short?)");
    return EXIT_FAILURE;
  }

  const double avg_assigned = static_cast<double>(cum_assigned) / static_cast<double>(frame_idx);
  const double avg_composited =
      static_cast<double>(cum_composited) / static_cast<double>(frame_idx);
  const double avg_test = static_cast<double>(cum_test) / static_cast<double>(frame_idx);
  const double avg_props = static_cast<double>(cum_props) / static_cast<double>(frame_idx);
  const double avg_commit_us = static_cast<double>(cum_commit_us) / static_cast<double>(frame_idx);

  drm::println(stderr,
               "plane_stress: {} frames, avg test={:.2f} assigned={:.2f} composited={:.2f} "
               "props={:.2f} commit_us={:.1f}",
               frame_idx, avg_test, avg_assigned, avg_composited, avg_props, avg_commit_us);

  return EXIT_SUCCESS;
}