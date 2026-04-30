// SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// camera — zero-copy libcamera -> KMS scanout viewfinder.
//
// Plan: docs/cam_example_plan.md (originally drafted as `camcli`,
// renamed to `camera`). The end-state is a single-binary CLI tool that
// negotiates a format both libcamera and a chosen DRM plane accept,
// imports libcamera's DMA-BUFs as a per-frame LayerScene buffer
// source, and drives a steady-state commit loop. `--probe` is the
// diagnostic mode: it walks both sides of the pipeline and prints the
// (camera, plane, fourcc, size, modifier) tuple a streaming run would
// pick, without acquiring streaming resources.
//
// libcamera is a hard build dependency of this example only — a
// missing libcamera fails the example target rather than the whole
// drm-cxx build.
//
// Usage:
//   camera --probe [/dev/dri/cardN]
//
// Output:
//   1. connector / CRTC / mode summary
//   2. per-plane table of the IN_FORMATS contents (or the bare format
//      list when the driver doesn't expose IN_FORMATS)
//   3. libcamera-enumerated cameras with id, model, location, and the
//      validated Viewfinder StreamConfiguration plus full StreamFormats
//      matrix
//   4. the negotiated target — the format / size / plane / modifier
//      tuple a streaming run would commit to.

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
#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/controls.h>
#include <libcamera/geometry.h>
#include <libcamera/pixel_format.h>
#include <libcamera/property_ids.h>
#include <libcamera/stream.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Probe data threaded through the plane/camera walks and consumed by
// negotiate() at the end of the probe. Both halves are flat: one
// DisplayFmt per (plane, fourcc) and one CaptureFmt per
// (camera, fourcc), with their respective modifier / size lists
// attached. The negotiator iterates fourccs by preference, finds the
// first pair that intersects, and picks a size + modifier from the
// intersection.
struct DisplayFmt {
  std::uint32_t plane_id;
  bool is_overlay;
  std::uint32_t fourcc;
  std::vector<std::uint64_t> modifiers;  // empty when IN_FORMATS unsupported
};

struct CaptureSize {
  std::uint32_t width;
  std::uint32_t height;
};

struct CaptureFmt {
  std::size_t camera_index;
  std::string camera_id;
  std::uint32_t fourcc;
  std::vector<CaptureSize> sizes;
};

struct NegotiatedTarget {
  std::size_t camera_index;
  std::string camera_id;
  std::uint32_t plane_id;
  std::uint32_t fourcc;
  CaptureSize size;
  std::uint64_t modifier;
};

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

void print_plane(const drm::planes::PlaneCapabilities& p, std::vector<DisplayFmt>& out) {
  drm::println("  plane id={} type={} zpos=[{}..{}] scaling={} rotation={} blend={} alpha_prop={}",
               p.id, plane_type_label(p.type),
               p.zpos_min ? drm::format("{}", *p.zpos_min) : std::string{"-"},
               p.zpos_max ? drm::format("{}", *p.zpos_max) : std::string{"-"},
               p.supports_scaling ? "yes" : "no", p.supports_rotation ? "yes" : "no",
               p.has_pixel_blend_mode ? "yes" : "no", p.has_per_plane_alpha ? "yes" : "no");

  // Cursor planes accept only ARGB8888 at fixed small sizes — they are
  // never candidates for camera scanout, so don't emit DisplayFmt
  // entries for them.
  const bool collect = p.type != drm::planes::DRMPlaneType::CURSOR;
  const bool is_overlay = p.type == drm::planes::DRMPlaneType::OVERLAY;

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
      if (collect) {
        out.push_back(DisplayFmt{p.id, is_overlay, cur, mods});
      }
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
      if (collect) {
        // Empty modifier list signals "implicit LINEAR" to the
        // negotiator.
        out.push_back(DisplayFmt{p.id, is_overlay, fmt, {}});
      }
    }
  }
}

const char* camera_location_label(std::int32_t loc) noexcept {
  switch (loc) {
    case libcamera::properties::CameraLocationFront:
      return "Front";
    case libcamera::properties::CameraLocationBack:
      return "Back";
    case libcamera::properties::CameraLocationExternal:
      return "External";
    default:
      return "Unknown";
  }
}

const char* config_status_label(libcamera::CameraConfiguration::Status s) noexcept {
  switch (s) {
    case libcamera::CameraConfiguration::Valid:
      return "Valid";
    case libcamera::CameraConfiguration::Adjusted:
      return "Adjusted";
    case libcamera::CameraConfiguration::Invalid:
      return "Invalid";
  }
  return "Unknown";
}

// Acquire the camera, generate a Viewfinder-role configuration, dump
// the validated default plus the full StreamFormats matrix
// (pixelformat × sizes) the pipeline can produce, append every
// (camera_index, fourcc, sizes) tuple to `out`, then release.
//
// acquire() may fail with -EBUSY when another process owns the
// camera; that's expected on systems with a running viewer/portal,
// not fatal to the probe — we log and move on.
void print_camera_streams(libcamera::Camera& cam, std::size_t cam_index,
                          std::vector<CaptureFmt>& out) {
  if (const int rc = cam.acquire(); rc < 0) {
    drm::println(stderr, "      acquire: {} (skipping streams)", std::strerror(-rc));
    return;
  }

  auto config = cam.generateConfiguration({libcamera::StreamRole::Viewfinder});
  if (!config || config->empty()) {
    drm::println(stderr, "      generateConfiguration(Viewfinder): empty");
    cam.release();
    return;
  }

  const auto status = config->validate();
  drm::println("      streams (Viewfinder, validate={}):", config_status_label(status));
  for (unsigned int i = 0; i < config->size(); ++i) {
    const auto& sc = config->at(i);
    drm::println("        [{}] default {} {} ({:#x}) frameSize={} bufferCount={}", i,
                 sc.size.toString(), sc.pixelFormat.toString(), sc.pixelFormat.fourcc(),
                 sc.frameSize, sc.bufferCount);
    const auto& fmts = sc.formats();
    const auto pixfmts = fmts.pixelformats();
    drm::println("            supported: {} pixel format(s)", pixfmts.size());
    for (const auto& pf : pixfmts) {
      const auto sizes = fmts.sizes(pf);
      drm::print("              {} ({:#x}):", pf.toString(), pf.fourcc());
      std::vector<CaptureSize> collected;
      collected.reserve(sizes.size());
      for (const auto& sz : sizes) {
        drm::print(" {}", sz.toString());
        collected.push_back(CaptureSize{sz.width, sz.height});
      }
      drm::println("");
      out.push_back(CaptureFmt{cam_index, cam.id(), pf.fourcc(), std::move(collected)});
    }
  }

  cam.release();
}

// Bring up libcamera's CameraManager, list every visible camera with
// the properties relevant to picking one (id, model, location) plus
// the Viewfinder StreamConfigurations it can produce, append capture
// formats to `out`, then tear it down.
int list_cameras(std::vector<CaptureFmt>& out) {
  libcamera::CameraManager cm;
  if (const int rc = cm.start(); rc < 0) {
    drm::println(stderr, "CameraManager::start: {}", std::strerror(-rc));
    return EXIT_FAILURE;
  }

  // Scope the shared_ptrs so they drop before CameraManager::stop().
  // stop() warns "Removing media device ... while still in use" when
  // any Camera shared_ptr outlives it.
  {
    const std::vector<std::shared_ptr<libcamera::Camera>> cameras = cm.cameras();
    drm::println("Cameras: {} visible to libcamera", cameras.size());
    for (std::size_t i = 0; i < cameras.size(); ++i) {
      const std::shared_ptr<libcamera::Camera>& cam = cameras.at(i);
      const libcamera::ControlList& props = cam->properties();
      const auto model = props.get(libcamera::properties::Model);
      const auto location = props.get(libcamera::properties::Location);
      drm::println("  [{}] id={}", i, cam->id());
      drm::println("      model={}  location={}", model ? *model : std::string{"<unknown>"},
                   location ? camera_location_label(*location) : "<unknown>");
      print_camera_streams(*cam, i, out);
    }
  }

  cm.stop();
  return EXIT_SUCCESS;
}

// Pure logic: pick a (camera, plane, fourcc, size, modifier) tuple
// from the probe data using a fixed preference order:
//
//   1. NV12 — half the bandwidth of RGB and the universal libcamera
//               viewfinder default; both UVC and most ISP pipelines
//               produce it.
//   2. YUV420 — the planar fallback when NV12 isn't on the menu.
//   3. XRGB8888 / ARGB8888 — last resort, 4× the bandwidth, but the
//               only formats some Raspberry-Pi-class plane stacks
//               accept directly.
//
// For the chosen fourcc:
//   - prefer an OVERLAY plane over a PRIMARY plane (overlays don't
//     fight the desktop background and let us scan out raw camera
//     pixels without compositing);
//   - pick the largest capture size that fits within the display
//     mode, falling back to the smallest available when every camera
//     size exceeds the display;
//   - prefer LINEAR over any vendor modifier the plane offers, since
//     libcamera's pipelines emit linear by default and forcing a
//     tiled modifier requires producer cooperation we don't have yet.
//     An empty modifier list (driver doesn't expose IN_FORMATS)
//     collapses to LINEAR by convention.
std::optional<NegotiatedTarget> negotiate(const std::vector<DisplayFmt>& display,
                                          const std::vector<CaptureFmt>& capture,
                                          std::uint32_t mode_w, std::uint32_t mode_h) {
  static constexpr std::array<std::uint32_t, 4> preference = {
      DRM_FORMAT_NV12,
      DRM_FORMAT_YUV420,
      DRM_FORMAT_XRGB8888,
      DRM_FORMAT_ARGB8888,
  };

  auto pick_size = [&](const std::vector<CaptureSize>& sizes) -> std::optional<CaptureSize> {
    if (sizes.empty()) {
      return std::nullopt;
    }
    const CaptureSize* best = nullptr;
    for (const auto& sz : sizes) {
      if (sz.width <= mode_w && sz.height <= mode_h) {
        if (best == nullptr || (static_cast<std::uint64_t>(sz.width) * sz.height) >
                                   (static_cast<std::uint64_t>(best->width) * best->height)) {
          best = &sz;
        }
      }
    }
    if (best != nullptr) {
      return *best;
    }
    // Every camera size exceeds the display mode — fall back to the
    // smallest so the consumer can still scale down to fit.
    best = &sizes.front();
    for (const auto& sz : sizes) {
      if ((static_cast<std::uint64_t>(sz.width) * sz.height) <
          (static_cast<std::uint64_t>(best->width) * best->height)) {
        best = &sz;
      }
    }
    return *best;
  };

  auto pick_modifier = [](const std::vector<std::uint64_t>& modifiers) -> std::uint64_t {
    if (modifiers.empty()) {
      return DRM_FORMAT_MOD_LINEAR;
    }
    for (const auto m : modifiers) {
      if (m == DRM_FORMAT_MOD_LINEAR) {
        return DRM_FORMAT_MOD_LINEAR;
      }
    }
    return modifiers.front();
  };

  for (const auto fourcc : preference) {
    const CaptureFmt* cap = nullptr;
    for (const auto& cf : capture) {
      if (cf.fourcc == fourcc && !cf.sizes.empty()) {
        cap = &cf;
        break;
      }
    }
    if (cap == nullptr) {
      continue;
    }

    const DisplayFmt* disp = nullptr;
    for (const auto& df : display) {
      if (df.fourcc == fourcc && df.is_overlay) {
        disp = &df;
        break;
      }
    }
    if (disp == nullptr) {
      for (const auto& df : display) {
        if (df.fourcc == fourcc) {
          disp = &df;
          break;
        }
      }
    }
    if (disp == nullptr) {
      continue;
    }

    const auto picked = pick_size(cap->sizes);
    if (!picked) {
      continue;
    }

    return NegotiatedTarget{cap->camera_index, cap->camera_id,
                            disp->plane_id,    fourcc,
                            *picked,           pick_modifier(disp->modifiers)};
  }
  return std::nullopt;
}

std::string modifier_label(std::uint64_t modifier) {
  if (modifier == DRM_FORMAT_MOD_LINEAR) {
    return "LINEAR";
  }
  if (modifier == DRM_FORMAT_MOD_INVALID) {
    return "INVALID";
  }
  return drm::format("{:#x}", modifier);
}

void print_negotiation(const std::optional<NegotiatedTarget>& target) {
  drm::println("");
  if (!target) {
    drm::println("No common format between any visible camera and any scanout plane.");
    return;
  }
  const auto chars = fourcc_to_chars(target->fourcc);
  drm::println("Negotiated target:");
  drm::println("  camera [{}] id={}", target->camera_index, target->camera_id);
  drm::println("  plane id={}", target->plane_id);
  drm::println("  format {} ({:#x}) {}x{} modifier={}", std::string_view(chars.data(), 4),
               target->fourcc, target->size.width, target->size.height,
               modifier_label(target->modifier));
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
  std::vector<DisplayFmt> display_formats;
  for (const auto* p : reg->for_crtc(*idx)) {
    print_plane(*p, display_formats);
  }

  drm::println("");
  std::vector<CaptureFmt> capture_formats;
  if (const int rc = list_cameras(capture_formats); rc != EXIT_SUCCESS) {
    return rc;
  }

  print_negotiation(negotiate(display_formats, capture_formats, mode.hdisplay, mode.vdisplay));
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