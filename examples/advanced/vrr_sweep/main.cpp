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
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sched.h>
#include <string_view>
#include <sys/mman.h>
#include <system_error>
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

// Pin pages + run at real-time priority so the harness's own pacing jitter
// (clock_nanosleep wakeup + commit scheduling) drops below the panel's VRR
// behavior, making a VRR-on vs control difference measurable. Needs CAP_SYS_NICE
// (run as root); best-effort, reports why it failed.
bool engage_realtime() {
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    drm::println(stderr, "vrr_sweep: mlockall failed: {} (run as root for --rt)",
                 std::strerror(errno));
    return false;
  }
  // NOLINTNEXTLINE(misc-include-cleaner) — sched_param / SCHED_FIFO via <sched.h>
  struct sched_param sp{};
  sp.sched_priority = sched_get_priority_max(SCHED_FIFO) / 2;
  if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
    drm::println(stderr, "vrr_sweep: sched_setscheduler(SCHED_FIFO) failed: {}",
                 std::strerror(errno));
    return false;
  }
  return true;
}

// Parse "WxH@R" (e.g. "1920x1080@120").
bool parse_mode_spec(std::string_view s, unsigned& w, unsigned& h, unsigned& r) {
  const auto xpos = s.find('x');
  const auto apos = s.find('@');
  if (xpos == std::string_view::npos || apos == std::string_view::npos || apos <= xpos) {
    return false;
  }
  const auto to_u = [](std::string_view sv, unsigned& v) {
    return std::from_chars(sv.data(), sv.data() + sv.size(), v).ec == std::errc{};
  };
  return to_u(s.substr(0, xpos), w) && to_u(s.substr(xpos + 1, apos - xpos - 1), h) &&
         to_u(s.substr(apos + 1), r);
}

// Override `mode` with the connector's matching WxH@R mode. Returns false (and
// lists what's available) when the spec is malformed or no mode matches.
bool apply_mode(int fd, std::uint32_t connector_id, std::string_view spec, drmModeModeInfo& mode) {
  unsigned w = 0;
  unsigned h = 0;
  unsigned r = 0;
  if (!parse_mode_spec(spec, w, h, r)) {
    drm::println(stderr, "vrr_sweep: bad --mode '{}' (expected WxH@R)", spec);
    return false;
  }
  drmModeConnectorPtr conn = drmModeGetConnectorCurrent(fd, connector_id);
  if (conn == nullptr) {
    return false;
  }
  bool found = false;
  for (int i = 0; i < conn->count_modes && !found; ++i) {
    const auto& m = conn->modes[i];
    if (m.hdisplay == w && m.vdisplay == h && m.vrefresh == r) {
      mode = m;
      found = true;
    }
  }
  if (!found) {
    drm::println(stderr, "vrr_sweep: mode {} not found; available:", spec);
    for (int i = 0; i < conn->count_modes; ++i) {
      const auto& m = conn->modes[i];
      drm::println(stderr, "  {}x{}@{}", m.hdisplay, m.vdisplay, m.vrefresh);
    }
  }
  drmModeFreeConnector(conn);
  return found;
}

// Map a --regamma name to the scene's OutputTransferFunction.
std::optional<drm::scene::LayerScene::OutputTransferFunction> parse_regamma(std::string_view s) {
  using TF = drm::scene::LayerScene::OutputTransferFunction;
  if (s == "default") {
    return TF::Default;
  }
  if (s == "identity") {
    return TF::Identity;
  }
  if (s == "srgb") {
    return TF::Srgb;
  }
  if (s == "bt709") {
    return TF::Bt709;
  }
  if (s == "pq") {
    return TF::Pq;
  }
  if (s == "gamma22") {
    return TF::Gamma22;
  }
  if (s == "gamma24") {
    return TF::Gamma24;
  }
  if (s == "gamma26") {
    return TF::Gamma26;
  }
  return std::nullopt;
}

// Read a CRTC property's current value by name (nullopt if the CRTC lacks it).
// Used to confirm AMD_CRTC_REGAMMA_TF is present + what the commit set it to.
std::optional<std::uint64_t> crtc_prop_value(int fd, std::uint32_t crtc_id, const char* name) {
  std::optional<std::uint64_t> out;
  drmModeObjectProperties* props = drmModeObjectGetProperties(fd, crtc_id, DRM_MODE_OBJECT_CRTC);
  if (props == nullptr) {
    return out;
  }
  for (std::uint32_t i = 0; i < props->count_props && !out.has_value(); ++i) {
    drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
    if (p != nullptr) {
      if (std::strcmp(p->name, name) == 0) {
        out = props->prop_values[i];
      }
      drmModeFreeProperty(p);
    }
  }
  drmModeFreeObjectProperties(props);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  bool want_vrr = false;
  bool want_rt = false;
  std::string_view mode_str;
  std::string_view regamma_str;
  {
    int write = 1;
    for (int i = 1; i < argc; ++i) {
      const std::string_view a{argv[i]};
      if (a == "--vrr") {
        want_vrr = true;
      } else if (a == "--rt") {
        want_rt = true;
      } else if (a == "--mode" && (i + 1) < argc) {
        mode_str = argv[++i];
      } else if (a == "--regamma" && (i + 1) < argc) {
        regamma_str = argv[++i];
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
  // --mode picks a non-preferred mode (e.g. a high-refresh one): VRR can only
  // track up to the mode's max, so a 4K30 preferred mode pins the sweep at 30.
  if (!mode_str.empty() && !apply_mode(dev.fd(), output->connector_id, mode_str, output->mode)) {
    return EXIT_FAILURE;
  }
  if (want_rt) {
    drm::println("vrr_sweep: RT pacing {}",
                 engage_realtime() ? "engaged (SCHED_FIFO + mlockall)" : "not engaged");
  }
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
  if (!regamma_str.empty()) {
    const auto tf = parse_regamma(regamma_str);
    if (!tf) {
      drm::println(stderr,
                   "vrr_sweep: bad --regamma '{}' (default/identity/srgb/bt709/pq/gamma22..26)",
                   regamma_str);
      return EXIT_FAILURE;
    }
    const auto before = crtc_prop_value(dev.fd(), output->crtc_id, "AMD_CRTC_REGAMMA_TF");
    drm::println("vrr_sweep: AMD_CRTC_REGAMMA_TF on this CRTC: {} -> requesting '{}'",
                 before ? "present" : "ABSENT (call will no-op)", regamma_str);
    scene->set_output_transfer_function(*tf);
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

  if (!regamma_str.empty()) {
    const auto after = crtc_prop_value(dev.fd(), output->crtc_id, "AMD_CRTC_REGAMMA_TF");
    if (after) {
      drm::println("vrr_sweep: AMD_CRTC_REGAMMA_TF after commit = {} (driver accepted the set)",
                   *after);
    } else {
      drm::println("vrr_sweep: AMD_CRTC_REGAMMA_TF absent on this CRTC (set was a no-op)");
    }
  }
  drm::println("vrr_sweep: done");
  return EXIT_SUCCESS;
}
