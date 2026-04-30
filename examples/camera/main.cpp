// SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// camera — zero-copy libcamera -> KMS scanout viewfinder.
//
// Plan: docs/cam_example_plan.md (originally drafted as `camcli`,
// renamed to `camera`). The end-state is a single-binary CLI tool that
// negotiates a format both libcamera and a chosen DRM plane accept,
// imports libcamera's DMA-BUFs as a per-frame LayerScene buffer source,
// and drives a steady-state commit loop. The `--probe` mode is the
// first slice: it opens the device, picks an output, and prints what
// the active CRTC's planes can scan out so the upcoming format-
// negotiation pass has something concrete to negotiate against.
//
// libcamera is intentionally not yet a build dependency. The probe is
// pure drm-cxx so it remains useful for diagnosing scanout-side
// problems (a wrong plane, a missing format) on hardware where the
// camera side hasn't been wired up yet.
//
// Usage:
//   camera --probe [/dev/dri/cardN]
//
// Output: connector / CRTC / mode summary, followed by a per-plane
// table of the IN_FORMATS contents (or the bare format list when the
// driver doesn't expose IN_FORMATS).

#include "../common/format_probe.hpp"
#include "../common/open_output.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/resources.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <drm_fourcc.h>
#include <xf86drmMode.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace {

// Render a fourcc as the four ASCII bytes drivers expose in IN_FORMATS,
// falling back to the hex value when any byte is non-printable. Avoids
// the C-style `%c%c%c%c` pun and keeps every output line the same width.
std::array<char, 5> fourcc_to_chars(std::uint32_t f) noexcept {
  std::array<char, 5> out{};
  for (int i = 0; i < 4; ++i) {
    const auto c = static_cast<unsigned char>((f >> (i * 8)) & 0xffU);
    out.at(static_cast<std::size_t>(i)) = (c >= 0x20 && c <= 0x7e) ? static_cast<char>(c) : '?';
  }
  return out;
}

const char* plane_type_label(drm::planes::DRMPlaneType t) noexcept {
  switch (t) {
    case drm::planes::DRMPlaneType::PRIMARY:
      return "PRIMARY";
    case drm::planes::DRMPlaneType::OVERLAY:
      return "OVERLAY";
    case drm::planes::DRMPlaneType::CURSOR:
      return "CURSOR ";
  }
  return "UNKNOWN";
}

// Locate the CRTC's 0-based index in the device's resource list.
// PlaneRegistry::for_crtc() and PlaneCapabilities::possible_crtcs both
// key on this position rather than the CRTC object id.
std::optional<std::uint32_t> crtc_index_of(const drm::Device& dev, std::uint32_t crtc_id) noexcept {
  const auto res = drm::get_resources(dev.fd());
  if (!res) {
    return std::nullopt;
  }
  for (int i = 0; i < res->count_crtcs; ++i) {
    if (res->crtcs[i] == crtc_id) {
      return static_cast<std::uint32_t>(i);
    }
  }
  return std::nullopt;
}

void print_plane(const drm::planes::PlaneCapabilities& p) {
  drm::println("  plane id={} type={} zpos=[{}..{}] scaling={} rotation={} blend={} alpha_prop={}",
               p.id, plane_type_label(p.type),
               p.zpos_min ? drm::format("{}", *p.zpos_min) : std::string{"-"},
               p.zpos_max ? drm::format("{}", *p.zpos_max) : std::string{"-"},
               p.supports_scaling ? "yes" : "no", p.supports_rotation ? "yes" : "no",
               p.has_pixel_blend_mode ? "yes" : "no", p.has_per_plane_alpha ? "yes" : "no");

  if (p.has_format_modifiers) {
    // IN_FORMATS path: group (format, modifier) pairs by format so each
    // line shows one fourcc and its full modifier list. Pairs arrive
    // sorted by format ascending, so a single linear pass groups them.
    std::uint32_t cur = 0;
    bool have_cur = false;
    std::vector<std::uint64_t> mods;
    auto emit = [&]() {
      if (!have_cur) {
        return;
      }
      const auto chars = fourcc_to_chars(cur);
      drm::print("    {} ({:#x})  modifiers:", std::string_view(chars.data(), 4), cur);
      for (const auto m : mods) {
        if (m == DRM_FORMAT_MOD_LINEAR) {
          drm::print(" LINEAR");
        } else if (m == DRM_FORMAT_MOD_INVALID) {
          drm::print(" INVALID");
        } else {
          drm::print(" {:#x}", m);
        }
      }
      drm::println("");
    };
    for (const auto& [fmt, mod] : p.format_modifiers) {
      if (!have_cur || fmt != cur) {
        emit();
        cur = fmt;
        have_cur = true;
        mods.clear();
      }
      mods.push_back(mod);
    }
    emit();
  } else {
    // Pre-IN_FORMATS legacy path: just the bare format list, no
    // modifier metadata. LINEAR / INVALID are the only viable
    // modifiers in this case.
    for (const auto fmt : p.formats) {
      const auto chars = fourcc_to_chars(fmt);
      drm::println("    {} ({:#x})  modifiers: <IN_FORMATS not exposed>",
                   std::string_view(chars.data(), 4), fmt);
    }
  }
}

void print_usage() {
  drm::println(stderr, "usage: camera --probe [/dev/dri/cardN]");
}

int run_probe(int argc, char* argv[]) {
  auto output = drm::examples::open_and_pick_output(argc, argv);
  if (!output) {
    return EXIT_FAILURE;
  }
  auto& dev = output->device;
  const drmModeModeInfo mode = output->mode;

  drm::println("Output: {}x{}@{}Hz on connector {} / CRTC {}", mode.hdisplay, mode.vdisplay,
               mode.vrefresh, output->connector_id, output->crtc_id);

  // High-level capability summary first, so the per-plane dump below has
  // context. The format probe already parses the same data we walk
  // below; the duplication is intentional — the summary is what a user
  // wants to skim, the per-plane dump is what a developer wants to
  // grep.
  const auto caps = drm::examples::probe_output(dev, output->crtc_id);
  drm::println(
      "Planes: {} total ({} primary, {} overlay, {} cursor); zpos={} alpha_blend_planes={}",
      caps.n_planes + caps.n_cursor, caps.n_primary, caps.n_overlay, caps.n_cursor,
      caps.any_plane_zpos ? "yes" : "no", caps.n_overlay_alpha_blend + caps.n_primary_alpha_blend);

  const auto idx = crtc_index_of(dev, output->crtc_id);
  if (!idx) {
    drm::println(stderr, "Could not resolve CRTC index for id {}", output->crtc_id);
    return EXIT_FAILURE;
  }
  auto reg = drm::planes::PlaneRegistry::enumerate(dev);
  if (!reg) {
    drm::println(stderr, "PlaneRegistry::enumerate: {}", reg.error().message());
    return EXIT_FAILURE;
  }

  drm::println("Plane formats (IN_FORMATS) reachable from CRTC {}:", output->crtc_id);
  for (const auto* p : reg->for_crtc(*idx)) {
    print_plane(*p);
  }
  return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char* argv[]) {
  // Pull `--probe` off argv if present so the device-path positional
  // arg lands at argv[1] for select_device(). Anything else is rejected
  // until concrete modes (--frames, --size, --format, --camera) are
  // wired in.
  bool want_probe = false;
  std::vector<char*> rest;
  rest.reserve(static_cast<std::size_t>(argc));
  rest.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--probe") {
      want_probe = true;
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      return EXIT_SUCCESS;
    } else if (!arg.empty() && arg.front() == '-') {
      drm::println(stderr, "Unknown option: {}", arg);
      print_usage();
      return EXIT_FAILURE;
    } else {
      rest.push_back(argv[i]);
    }
  }

  if (!want_probe) {
    drm::println(stderr, "camera: --probe is currently the only supported mode");
    print_usage();
    return EXIT_FAILURE;
  }

  return run_probe(static_cast<int>(rest.size()), rest.data());
}