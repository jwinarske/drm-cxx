// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// allocator_torture — adversarial test battery for the LayerScene
// allocator. Six cases that each construct a scene shape designed to
// expose a specific allocator behaviour, run the scene for as many
// frames as the case needs, and print PASS / FAIL / SKIP plus
// concrete metrics. The numbers are what blog posts and READMEs
// quote ("warm-start steady state holds at 1 test_commit / frame
// across 120 frames of slow drift on 4 layers").
//
// Cases:
//
//   1. N+1 problem — scene with one more layer than the CRTC has
//      planes. Greedy fails the last layer; bipartite + composition
//      fallback places everything.  PASS = 0 unassigned and at least
//      one composited.
//
//   2. format cascade — every layer asks for a different fourcc
//      (ARGB / XRGB / ABGR / XBGR). Tests format-aware bipartite
//      matching.  PASS = 0 unassigned across `--frames`.
//
//   3. scaler monopoly — one layer needs scaling, the rest are 1:1.
//      Forces the allocator to land the scaler-required layer on a
//      scaling-capable plane. PASS = 0 unassigned across `--frames`.
//      SKIP = no plane on this CRTC advertises scaling.
//
//   4. rapid churn — N layers, dst_rect mutates every frame. Tests
//      that the warm-start cache holds under per-frame churn.
//      PASS = avg test_commits ≤ `--rapid-threshold` (default 1.5).
//
//   5. slow drift — N layers, one moves 4 px / frame for `--frames`.
//      Tests that small dst_rect deltas keep the warm assignment
//      valid. PASS = test_commits == 1 every post-warm frame.
//
//   6. burst-then-calm — N layers; rebuild every layer (toggle), then
//      hold steady. Tests recovery time. PASS = test_commits == 1
//      within `--burst-recovery` frames (default 4) of the last
//      mutation.
//
// Cases that need a specific plane budget (1) or scaling-capable
// plane (3) skip with an explanatory message when the hardware
// doesn't support them; this is intentional — the harness should
// keep running and emit a useful summary on whatever's available.
//
// Usage:
//   allocator_torture [--device PATH] [--frames N] [--rapid-threshold X]
//                     [--burst-recovery N]
//
// Defaults: --frames 60, --rapid-threshold 1.5, --burst-recovery 4.

#include "common/format_probe.hpp"
#include "common/open_output.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/resources.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/planes/plane_registry.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/display_params.hpp>
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
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

struct Options {
  std::string device;
  std::uint32_t frames{60U};
  double rapid_threshold{1.5};
  std::uint32_t burst_recovery{4U};
};

// std::stoul / std::stod throw on bad input but happily accept partial
// parses ("5abc" → 5), values beyond the requested width (2^32 → 0
// after cast), and non-finite doubles ("NaN", "inf"). Reject all of
// those — otherwise `--rapid-threshold NaN` would silently pass the
// `< 0.0` validator (NaN compares false to anything) and corrupt the
// pass/fail decision.
std::optional<std::uint32_t> parse_u32(std::string_view s) noexcept {
  if (s.empty() || s.front() == '-') {
    return std::nullopt;
  }
  try {
    std::size_t pos = 0;
    const auto v = std::stoul(std::string(s), &pos);
    if (pos != s.size() || v > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(v);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<double> parse_double(std::string_view s) noexcept {
  if (s.empty()) {
    return std::nullopt;
  }
  try {
    std::size_t pos = 0;
    const auto v = std::stod(std::string(s), &pos);
    if (pos != s.size() || !std::isfinite(v)) {
      return std::nullopt;
    }
    return v;
  } catch (...) {
    return std::nullopt;
  }
}

bool parse_args(int argc, char** argv, Options& opt, int& consumed) {
  consumed = 1;
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
    } else if (a == "--frames") {
      std::string_view v;
      if (!take(v)) {
        return false;
      }
      const auto n = parse_u32(v);
      if (!n || *n == 0U) {
        drm::println(stderr, "--frames: expected positive integer, got '{}'", v);
        return false;
      }
      opt.frames = *n;
    } else if (a == "--rapid-threshold") {
      std::string_view v;
      if (!take(v)) {
        return false;
      }
      const auto x = parse_double(v);
      if (!x || *x < 0.0) {
        drm::println(stderr, "--rapid-threshold: expected non-negative number, got '{}'", v);
        return false;
      }
      opt.rapid_threshold = *x;
    } else if (a == "--burst-recovery") {
      std::string_view v;
      if (!take(v)) {
        return false;
      }
      const auto n = parse_u32(v);
      if (!n) {
        drm::println(stderr, "--burst-recovery: expected non-negative integer, got '{}'", v);
        return false;
      }
      opt.burst_recovery = *n;
    } else if (a == "--help" || a == "-h") {
      return false;
    } else {
      break;
    }
  }
  return true;
}

void print_usage() {
  drm::println(stderr,
               "Usage: allocator_torture [--device PATH] [--frames N]\n"
               "                         [--rapid-threshold X] [--burst-recovery N]");
}

// PASS / FAIL / SKIP outcome from a single torture case.
enum class Outcome : std::uint8_t { Pass, Fail, Skip };

const char* outcome_name(Outcome o) noexcept {
  switch (o) {
    case Outcome::Pass:
      return "PASS";
    case Outcome::Fail:
      return "FAIL";
    case Outcome::Skip:
      return "SKIP";
  }
  return "?";
}

struct CaseResult {
  std::string name;
  Outcome outcome;
  std::string detail;  // metric line or skip reason
};

// ── Helpers shared across cases ──────────────────────────────────────────

// Probe the active CRTC's plane budget. Returns nullopt on failure;
// callers treat that as "skip every case that needs it" rather than
// re-flagging it as a failure.
struct PlaneBudget {
  std::size_t total{0};        // PRIMARY + OVERLAY
  std::size_t with_scaler{0};  // subset that advertise scaling
};

std::optional<PlaneBudget> probe_planes(const drm::Device& dev, std::uint32_t crtc_id) {
  const auto res = drm::get_resources(dev.fd());
  if (!res) {
    return std::nullopt;
  }
  std::optional<std::uint32_t> idx;
  for (int i = 0; i < res->count_crtcs; ++i) {
    if (res->crtcs[i] == crtc_id) {
      idx = static_cast<std::uint32_t>(i);
      break;
    }
  }
  if (!idx) {
    return std::nullopt;
  }
  auto reg = drm::planes::PlaneRegistry::enumerate(dev);
  if (!reg) {
    return std::nullopt;
  }
  PlaneBudget out;
  for (const auto* p : reg->for_crtc(*idx)) {
    if (p->type == drm::planes::DRMPlaneType::CURSOR) {
      continue;
    }
    ++out.total;
    if (p->supports_scaling) {
      ++out.with_scaler;
    }
  }
  return out;
}

// Solid 32-bpp fill. Used to give every layer some non-noise pixels
// before the first commit; later cases may not paint at all.
void paint_solid(drm::BufferMapping& map, std::uint32_t argb) noexcept {
  const auto pixels = map.pixels();
  const auto stride = map.stride();
  const auto w = map.width();
  const auto h = map.height();
  if (w == 0 || h == 0 || stride < w * 4U) {
    return;
  }
  for (std::uint32_t y = 0; y < h; ++y) {
    auto* row =
        reinterpret_cast<std::uint32_t*>(pixels.data() + (static_cast<std::size_t>(y) * stride));
    for (std::uint32_t x = 0; x < w; ++x) {
      row[x] = argb;
    }
  }
}

struct TileSpec {
  std::uint32_t fmt{DRM_FORMAT_ARGB8888};
  std::uint32_t src_w{0};
  std::uint32_t src_h{0};
  drm::scene::Rect dst{};
  std::optional<int> zpos;
  std::uint32_t shade{0xFFE0E0E0U};
};

// Add one layer to the scene from a TileSpec. Returns the handle on
// success. Layer's buffer is solid-painted before return so the first
// commit shows visible pixels.
drm::expected<drm::scene::LayerHandle, std::error_code> add_tile(drm::Device& dev,
                                                                 drm::scene::LayerScene& scene,
                                                                 const TileSpec& s) {
  auto src = drm::scene::DumbBufferSource::create(dev, s.src_w, s.src_h, s.fmt);
  if (!src) {
    return drm::unexpected<std::error_code>(src.error());
  }
  if (auto m = src->get()->map(drm::MapAccess::Write); m) {
    paint_solid(*m, s.shade);
  }
  drm::scene::LayerDesc desc;
  desc.source = std::move(*src);
  desc.display.src_rect = drm::scene::Rect{0, 0, s.src_w, s.src_h};
  desc.display.dst_rect = s.dst;
  desc.display.zpos = s.zpos;
  return scene.add_layer(std::move(desc));
}

// Helper layout: tile a row of `n` `tile_w`-wide tiles starting at y,
// returning per-tile dst_rects. Falls back to a 2-row layout if the
// row would overflow `fb_w`.
std::vector<drm::scene::Rect> tile_row(std::size_t n, std::uint32_t tile_w, std::uint32_t tile_h,
                                       std::uint32_t fb_w, std::int32_t y_start) {
  std::vector<drm::scene::Rect> out;
  out.reserve(n);
  std::int32_t x = 0;
  std::int32_t y = y_start;
  for (std::size_t i = 0; i < n; ++i) {
    if (x + static_cast<std::int32_t>(tile_w) > static_cast<std::int32_t>(fb_w)) {
      x = 0;
      y += static_cast<std::int32_t>(tile_h);
    }
    out.push_back({x, y, tile_w, tile_h});
    x += static_cast<std::int32_t>(tile_w);
  }
  return out;
}

// Issue a single commit and drain its page flip; return the report.
drm::expected<drm::scene::CommitReport, std::error_code> commit_one(drm::scene::LayerScene& scene,
                                                                    drm::PageFlip& page_flip) {
  auto rep = scene.commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip);
  if (!rep) {
    return drm::unexpected<std::error_code>(rep.error());
  }
  bool pending = true;
  page_flip.set_handler([&](std::uint32_t /*c*/, std::uint64_t /*s*/,
                            std::uint64_t /*t*/) noexcept { pending = false; });
  while (pending) {
    if (auto r = page_flip.dispatch(-1); !r) {
      return drm::unexpected<std::error_code>(r.error());
    }
  }
  return *rep;
}

// Wipe every layer the scene currently holds. Keeps the scene alive
// so the caller can register a fresh layer set against the same CRTC.
void wipe_scene(drm::scene::LayerScene& scene, std::vector<drm::scene::LayerHandle>& handles) {
  for (auto h : handles) {
    scene.remove_layer(h);
  }
  handles.clear();
}

// ── Cases ────────────────────────────────────────────────────────────────

// A scene with N+1 layers on a CRTC that has only N usable planes.
// PASS when every layer reaches scanout (assigned + composited == N+1
// AND unassigned == 0) and at least one is composited (so the
// composition fallback was actually exercised).
CaseResult case_n_plus_one(drm::Device& dev, drm::scene::LayerScene& scene, drm::PageFlip& pf,
                           std::uint32_t fb_w, std::uint32_t fb_h, const PlaneBudget& budget) {
  CaseResult r{"N+1 problem", Outcome::Pass, {}};
  if (budget.total < 2U) {
    r.outcome = Outcome::Skip;
    r.detail = drm::format("need ≥2 PRIMARY+OVERLAY planes; have {}", budget.total);
    return r;
  }
  const std::size_t target = budget.total + 1U;
  const std::uint32_t tile_w = std::max<std::uint32_t>(64U, fb_w / 8U);
  const std::uint32_t tile_h = std::max<std::uint32_t>(64U, fb_h / 8U);
  const auto rects = tile_row(target, tile_w, tile_h, fb_w, 0);

  std::vector<drm::scene::LayerHandle> handles;
  for (std::size_t i = 0; i < target; ++i) {
    TileSpec s;
    s.src_w = tile_w;
    s.src_h = tile_h;
    s.dst = rects.at(i);
    s.zpos = 3 + static_cast<int>(i);
    s.shade = 0xFFFF0000U | static_cast<std::uint32_t>((i * 0x303030U) & 0xFFFFFFU);
    auto h = add_tile(dev, scene, s);
    if (!h) {
      r.outcome = Outcome::Fail;
      r.detail = drm::format("add_tile {}: {}", i, h.error().message());
      wipe_scene(scene, handles);
      return r;
    }
    handles.push_back(*h);
  }
  auto rep = commit_one(scene, pf);
  if (!rep) {
    r.outcome = Outcome::Fail;
    r.detail = drm::format("commit: {}", rep.error().message());
    wipe_scene(scene, handles);
    return r;
  }
  const bool all_landed =
      rep->layers_unassigned == 0U && (rep->layers_assigned + rep->layers_composited) == target;
  const bool composed = rep->layers_composited >= 1U;
  r.outcome = (all_landed && composed) ? Outcome::Pass : Outcome::Fail;
  r.detail =
      drm::format("planes={} target={} assigned={} composited={} unassigned={}", budget.total,
                  target, rep->layers_assigned, rep->layers_composited, rep->layers_unassigned);
  wipe_scene(scene, handles);
  return r;
}

// Format-cascade case: build a tile per format in a small format pool
// (ARGB / XRGB / ABGR / XBGR). Trims to the plane budget so a 2-plane
// CRTC isn't forced into N+1 territory (that case is exercised
// separately above).
CaseResult case_format_cascade(drm::Device& dev, drm::scene::LayerScene& scene, drm::PageFlip& pf,
                               std::uint32_t fb_w, std::uint32_t fb_h, const PlaneBudget& budget) {
  CaseResult r{"format cascade", Outcome::Pass, {}};
  constexpr std::array<std::uint32_t, 4> pool{DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
                                              DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888};
  if (budget.total == 0) {
    r.outcome = Outcome::Skip;
    r.detail = "no usable planes";
    return r;
  }
  const std::size_t target = std::min<std::size_t>(pool.size(), budget.total);
  const std::uint32_t tile_w = std::max<std::uint32_t>(64U, fb_w / 8U);
  const std::uint32_t tile_h = std::max<std::uint32_t>(64U, fb_h / 8U);
  const auto rects = tile_row(target, tile_w, tile_h, fb_w, static_cast<std::int32_t>(tile_h));

  std::vector<drm::scene::LayerHandle> handles;
  for (std::size_t i = 0; i < target; ++i) {
    TileSpec s;
    s.fmt = pool.at(i);
    s.src_w = tile_w;
    s.src_h = tile_h;
    s.dst = rects.at(i);
    s.zpos = 3 + static_cast<int>(i);
    s.shade = 0xFF00FF00U;
    auto h = add_tile(dev, scene, s);
    if (!h) {
      // Some drivers reject DUMB allocations of less-common 32-bpp
      // channel orderings. Skip the case rather than mark it failed
      // — the allocator never had a chance to misbehave.
      r.outcome = Outcome::Skip;
      r.detail = drm::format("DumbBufferSource fmt {} unsupported: {}", i, h.error().message());
      wipe_scene(scene, handles);
      return r;
    }
    handles.push_back(*h);
  }
  auto rep = commit_one(scene, pf);
  if (!rep) {
    r.outcome = Outcome::Fail;
    r.detail = drm::format("commit: {}", rep.error().message());
    wipe_scene(scene, handles);
    return r;
  }
  r.outcome = (rep->layers_unassigned == 0U) ? Outcome::Pass : Outcome::Fail;
  r.detail = drm::format("formats={} assigned={} composited={} unassigned={}", target,
                         rep->layers_assigned, rep->layers_composited, rep->layers_unassigned);
  wipe_scene(scene, handles);
  return r;
}

// One scaler-required layer + (budget-1) 1:1 layers. The allocator
// must steer the scaler-required layer to a scaler-capable plane.
// Skipped on hardware without a scaling-capable plane.
CaseResult case_scaler_monopoly(drm::Device& dev, drm::scene::LayerScene& scene, drm::PageFlip& pf,
                                std::uint32_t fb_w, std::uint32_t fb_h, const PlaneBudget& budget) {
  CaseResult r{"scaler monopoly", Outcome::Pass, {}};
  if (budget.with_scaler == 0) {
    r.outcome = Outcome::Skip;
    r.detail = "no scaler-capable plane on this CRTC";
    return r;
  }
  if (budget.total < 2U) {
    r.outcome = Outcome::Skip;
    r.detail = drm::format("need ≥2 PRIMARY+OVERLAY planes; have {}", budget.total);
    return r;
  }
  const std::size_t target = std::min<std::size_t>(budget.total, 4U);
  const std::uint32_t tile_w = std::max<std::uint32_t>(64U, fb_w / 8U);
  const std::uint32_t tile_h = std::max<std::uint32_t>(64U, fb_h / 8U);
  const auto rects = tile_row(target, tile_w, tile_h, fb_w, 2 * static_cast<std::int32_t>(tile_h));

  std::vector<drm::scene::LayerHandle> handles;
  for (std::size_t i = 0; i < target; ++i) {
    TileSpec s;
    s.fmt = DRM_FORMAT_ARGB8888;
    s.dst = rects.at(i);
    s.zpos = 3 + static_cast<int>(i);
    if (i == 0) {
      // Scaler-required: source half the dst dimensions on each axis,
      // forcing a 2× upscale that only a scaling-capable plane can
      // satisfy.
      s.src_w = tile_w / 2U;
      s.src_h = tile_h / 2U;
      s.shade = 0xFFFF8800U;  // orange to make it visually obvious
    } else {
      s.src_w = tile_w;
      s.src_h = tile_h;
      s.shade = 0xFF8888FFU;
    }
    auto h = add_tile(dev, scene, s);
    if (!h) {
      r.outcome = Outcome::Fail;
      r.detail = drm::format("add_tile {}: {}", i, h.error().message());
      wipe_scene(scene, handles);
      return r;
    }
    handles.push_back(*h);
  }
  auto rep = commit_one(scene, pf);
  if (!rep) {
    r.outcome = Outcome::Fail;
    r.detail = drm::format("commit: {}", rep.error().message());
    wipe_scene(scene, handles);
    return r;
  }
  // PASS: scaler layer reached scanout (assigned or composited; either
  // way the allocator did not drop it). FAIL: layers_unassigned > 0.
  r.outcome = (rep->layers_unassigned == 0U) ? Outcome::Pass : Outcome::Fail;
  r.detail = drm::format("scalers={}/{} assigned={} composited={} unassigned={}",
                         budget.with_scaler, budget.total, rep->layers_assigned,
                         rep->layers_composited, rep->layers_unassigned);
  wipe_scene(scene, handles);
  return r;
}

// Build N tiles, mutate one tile's dst_rect every frame for `frames`,
// average test_commits across the run. PASS when avg ≤ threshold.
CaseResult case_rapid_churn(drm::Device& dev, drm::scene::LayerScene& scene, drm::PageFlip& pf,
                            std::uint32_t fb_w, std::uint32_t fb_h, std::uint32_t frames,
                            double threshold) {
  CaseResult r{"rapid churn", Outcome::Pass, {}};
  constexpr std::size_t k_layers = 4U;
  const std::uint32_t tile_w = std::max<std::uint32_t>(64U, fb_w / 8U);
  const std::uint32_t tile_h = std::max<std::uint32_t>(64U, fb_h / 8U);
  if (tile_w >= fb_w || tile_h >= fb_h) {
    r.outcome = Outcome::Skip;
    r.detail = drm::format("screen {}x{} too small for {}x{} tiles", fb_w, fb_h, tile_w, tile_h);
    return r;
  }
  const auto rects =
      tile_row(k_layers, tile_w, tile_h, fb_w, 3 * static_cast<std::int32_t>(tile_h));

  std::vector<drm::scene::LayerHandle> handles;
  for (std::size_t i = 0; i < k_layers; ++i) {
    TileSpec s;
    s.src_w = tile_w;
    s.src_h = tile_h;
    s.dst = rects.at(i);
    s.zpos = 3 + static_cast<int>(i);
    s.shade = 0xFF00FFFFU;
    auto h = add_tile(dev, scene, s);
    if (!h) {
      r.outcome = Outcome::Fail;
      r.detail = drm::format("add_tile {}: {}", i, h.error().message());
      wipe_scene(scene, handles);
      return r;
    }
    handles.push_back(*h);
  }

  std::uint64_t cum_test = 0U;
  std::uint32_t observed = 0U;
  for (std::uint32_t f = 0; f < frames; ++f) {
    // Bounce the i-th tile horizontally by 16 px.
    const std::size_t idx = f % k_layers;
    auto* layer = scene.get_layer(handles.at(idx));
    if (layer != nullptr) {
      auto rect = rects.at(idx);
      rect.x = (rect.x + 16) % static_cast<std::int32_t>(fb_w - tile_w);
      layer->set_dst_rect(rect);
    }
    auto rep = commit_one(scene, pf);
    if (!rep) {
      r.outcome = Outcome::Fail;
      r.detail = drm::format("commit @ frame {}: {}", f, rep.error().message());
      wipe_scene(scene, handles);
      return r;
    }
    if (f > 0) {  // skip frame 0 (cold start) so the avg reflects warm path
      cum_test += rep->test_commits_issued;
      ++observed;
    }
  }
  const double avg =
      observed == 0 ? 0.0 : static_cast<double>(cum_test) / static_cast<double>(observed);
  r.outcome = (avg <= threshold) ? Outcome::Pass : Outcome::Fail;
  r.detail = drm::format("avg test_commits={:.2f} (threshold {:.2f}) over {} warm frames", avg,
                         threshold, observed);
  wipe_scene(scene, handles);
  return r;
}

// Slow drift: one layer slides 4 px / frame for `frames`. The warm
// assignment should remain valid every frame after frame 0.
CaseResult case_slow_drift(drm::Device& dev, drm::scene::LayerScene& scene, drm::PageFlip& pf,
                           std::uint32_t fb_w, std::uint32_t fb_h, std::uint32_t frames) {
  CaseResult r{"slow drift", Outcome::Pass, {}};
  constexpr std::size_t k_layers = 4U;
  const std::uint32_t tile_w = std::max<std::uint32_t>(64U, fb_w / 8U);
  const std::uint32_t tile_h = std::max<std::uint32_t>(64U, fb_h / 8U);
  if (tile_w >= fb_w || tile_h >= fb_h) {
    r.outcome = Outcome::Skip;
    r.detail = drm::format("screen {}x{} too small for {}x{} tiles", fb_w, fb_h, tile_w, tile_h);
    return r;
  }
  const auto rects =
      tile_row(k_layers, tile_w, tile_h, fb_w, 4 * static_cast<std::int32_t>(tile_h));

  std::vector<drm::scene::LayerHandle> handles;
  for (std::size_t i = 0; i < k_layers; ++i) {
    TileSpec s;
    s.src_w = tile_w;
    s.src_h = tile_h;
    s.dst = rects.at(i);
    s.zpos = 3 + static_cast<int>(i);
    s.shade = 0xFFFFFF00U;
    auto h = add_tile(dev, scene, s);
    if (!h) {
      r.outcome = Outcome::Fail;
      r.detail = drm::format("add_tile {}: {}", i, h.error().message());
      wipe_scene(scene, handles);
      return r;
    }
    handles.push_back(*h);
  }

  std::uint32_t over_one = 0U;  // count of warm frames where test_commits > 1
  std::uint32_t observed = 0U;
  auto rect0 = rects.at(0);
  for (std::uint32_t f = 0; f < frames; ++f) {
    auto* layer = scene.get_layer(handles.at(0));
    if (layer != nullptr) {
      rect0.x = (rect0.x + 4) % static_cast<std::int32_t>(fb_w - tile_w);
      layer->set_dst_rect(rect0);
    }
    auto rep = commit_one(scene, pf);
    if (!rep) {
      r.outcome = Outcome::Fail;
      r.detail = drm::format("commit @ frame {}: {}", f, rep.error().message());
      wipe_scene(scene, handles);
      return r;
    }
    if (f > 0) {
      ++observed;
      if (rep->test_commits_issued > 1U) {
        ++over_one;
      }
    }
  }
  r.outcome = (over_one == 0U) ? Outcome::Pass : Outcome::Fail;
  r.detail = drm::format("test_commits>1 in {}/{} warm frames", over_one, observed);
  wipe_scene(scene, handles);
  return r;
}

// Burst-then-calm: rebuild every layer once, then hold steady. Count
// how many post-burst frames it takes for test_commits to settle to 1.
CaseResult case_burst_then_calm(drm::Device& dev, drm::scene::LayerScene& scene, drm::PageFlip& pf,
                                std::uint32_t fb_w, std::uint32_t fb_h, std::uint32_t frames,
                                std::uint32_t recovery_budget) {
  CaseResult r{"burst-then-calm", Outcome::Pass, {}};
  constexpr std::size_t k_layers = 4U;
  const std::uint32_t tile_w = std::max<std::uint32_t>(64U, fb_w / 8U);
  const std::uint32_t tile_h = std::max<std::uint32_t>(64U, fb_h / 8U);
  const auto rects =
      tile_row(k_layers, tile_w, tile_h, fb_w, 5 * static_cast<std::int32_t>(tile_h));

  std::vector<drm::scene::LayerHandle> handles;
  auto build_set = [&](std::uint32_t shade) -> drm::expected<void, std::error_code> {
    for (std::size_t i = 0; i < k_layers; ++i) {
      TileSpec s;
      s.src_w = tile_w;
      s.src_h = tile_h;
      s.dst = rects.at(i);
      s.zpos = 3 + static_cast<int>(i);
      s.shade = shade;
      auto h = add_tile(dev, scene, s);
      if (!h) {
        return drm::unexpected<std::error_code>(h.error());
      }
      handles.push_back(*h);
    }
    return {};
  };
  if (auto b = build_set(0xFFAA00FFU); !b) {
    r.outcome = Outcome::Fail;
    r.detail = drm::format("initial build_set: {}", b.error().message());
    wipe_scene(scene, handles);
    return r;
  }

  // Warm up.
  for (std::uint32_t f = 0; f < 2; ++f) {
    auto rep = commit_one(scene, pf);
    if (!rep) {
      r.outcome = Outcome::Fail;
      r.detail = drm::format("warmup commit @ frame {}: {}", f, rep.error().message());
      wipe_scene(scene, handles);
      return r;
    }
  }

  // Burst: tear down + rebuild every layer.
  wipe_scene(scene, handles);
  if (auto b = build_set(0xFF00AAFFU); !b) {
    r.outcome = Outcome::Fail;
    r.detail = drm::format("burst build_set: {}", b.error().message());
    wipe_scene(scene, handles);
    return r;
  }

  // Track frames-to-warm. The first commit after the burst is cold by
  // construction; subsequent commits should warm up within
  // `recovery_budget` frames.
  std::uint32_t recovery_frames = 0U;
  bool warmed = false;
  for (std::uint32_t f = 0; f < frames; ++f) {
    auto rep = commit_one(scene, pf);
    if (!rep) {
      r.outcome = Outcome::Fail;
      r.detail = drm::format("post-burst commit @ frame {}: {}", f, rep.error().message());
      wipe_scene(scene, handles);
      return r;
    }
    if (rep->test_commits_issued <= 1U) {
      warmed = true;
      recovery_frames = f;
      break;
    }
  }
  if (!warmed) {
    r.outcome = Outcome::Fail;
    r.detail = drm::format("never warmed within {} frames", frames);
  } else if (recovery_frames > recovery_budget) {
    r.outcome = Outcome::Fail;
    r.detail = drm::format("warmed at frame {} (budget {})", recovery_frames, recovery_budget);
  } else {
    r.detail = drm::format("warmed at frame {} (budget {})", recovery_frames, recovery_budget);
  }
  wipe_scene(scene, handles);
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  int consumed = 1;
  if (!parse_args(argc, argv, opt, consumed)) {
    print_usage();
    return EXIT_FAILURE;
  }

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
  drm::println(stderr, "allocator_torture: {}x{}@{}Hz on connector {} / CRTC {}", fb_w, fb_h,
               mode.vrefresh, output->connector_id, output->crtc_id);

  const auto budget_opt = probe_planes(dev, output->crtc_id);
  PlaneBudget budget;
  if (budget_opt) {
    budget = *budget_opt;
  } else {
    drm::println(stderr, "allocator_torture: plane probe failed — case 1 and case 3 will skip");
  }
  drm::println(stderr, "plane budget: {} usable, {} with scaler", budget.total, budget.with_scaler);

  // Surface capability gaps that affect the torture shape (alpha
  // routing through composition, missing zpos defeating layer order).
  drm::examples::warn_compat(drm::examples::probe_output(dev, output->crtc_id),
                             {.wants_alpha_overlays = true, .wants_explicit_zpos = true});

  const drm::scene::LayerScene::Config cfg{output->crtc_id, output->connector_id, mode};
  auto scene_r = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_r) {
    drm::println(stderr, "LayerScene::create: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_r);
  drm::PageFlip page_flip(dev);

  std::vector<CaseResult> results;
  results.push_back(case_n_plus_one(dev, *scene, page_flip, fb_w, fb_h, budget));
  results.push_back(case_format_cascade(dev, *scene, page_flip, fb_w, fb_h, budget));
  results.push_back(case_scaler_monopoly(dev, *scene, page_flip, fb_w, fb_h, budget));
  results.push_back(
      case_rapid_churn(dev, *scene, page_flip, fb_w, fb_h, opt.frames, opt.rapid_threshold));
  results.push_back(case_slow_drift(dev, *scene, page_flip, fb_w, fb_h, opt.frames));
  results.push_back(
      case_burst_then_calm(dev, *scene, page_flip, fb_w, fb_h, opt.frames, opt.burst_recovery));

  // Print a uniform results table to stderr (CSV-clean for shell pipelines).
  drm::println(stderr, "");
  drm::println(stderr, "case                       result   detail");
  drm::println(stderr, "----                       ------   ------");
  std::uint32_t pass_count = 0U;
  std::uint32_t fail_count = 0U;
  std::uint32_t skip_count = 0U;
  for (const auto& r : results) {
    drm::println(stderr, "  {:<24}  {:<6}  {}", r.name, outcome_name(r.outcome), r.detail);
    switch (r.outcome) {
      case Outcome::Pass:
        ++pass_count;
        break;
      case Outcome::Fail:
        ++fail_count;
        break;
      case Outcome::Skip:
        ++skip_count;
        break;
    }
  }
  drm::println(stderr, "");
  drm::println(stderr, "summary: {} pass, {} fail, {} skip", pass_count, fail_count, skip_count);

  // Exit non-zero only on FAIL — SKIP is a benign hardware-doesn't-
  // expose-this signal, not a build/test failure.
  return fail_count == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
