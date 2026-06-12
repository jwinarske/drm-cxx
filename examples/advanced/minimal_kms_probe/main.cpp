// SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// examples/advanced/minimal_kms_probe/main.cpp
//
// Capability + allocator-behaviour probe for minimal display controllers —
// written for the i.MX93 EVK's LCDIFv3 (`CONFIG_DRM_IMX_LCDIFV3`), but useful
// on any single-plane / GPU-less KMS device (TI tilcdc, i.MX8MM LCDIF, ...).
//
// It answers two questions, with no permanent state change to the card:
//
//   Phase 1 (structural, no commit): what does the PlaneRegistry + DriverProfile
//   actually report? A minimal controller should show exactly ONE PRIMARY plane, ZERO overlay,
//   ZERO cursor, no fb modifiers (LINEAR-only), and an RGB-only fourcc list. It
//   also flags the latent trap where DRM_CAP_CURSOR_* reports a 64x64 size even
//   though no CURSOR plane exists — HW cursor must be gated on the registry, not
//   the cap.
//
//   Phase 2 (behavioural, TEST_ONLY commit): drive a LayerScene with N
//   overlapping full-screen layers and read the CommitReport. On a one-plane
//   controller the allocator can hardware-assign at most one layer; the rest
//   must land in the CPU composition fallback (Composited), NOT be dropped
//   (Unassigned). The per-layer placement table shows exactly which path each
//   layer took.
//
// Build (on a host with meson): added to examples/ as target `minimal_kms_probe`.
// Build (on the EVK, which ships only gcc/g++ + libdrm, no meson):
//   see the compile note at the bottom of this file.
//
// Run:   ./minimal_kms_probe [/dev/dri/card0] [num_layers=3]

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/display/driver_profile.hpp>
#include <drm-cxx/planes/plane_registry.hpp>
#include <drm-cxx/present/scanout_format.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace {

const char* plane_type_name(drm::planes::DRMPlaneType t) {
  switch (t) {
    case drm::planes::DRMPlaneType::PRIMARY:
      return "PRIMARY";
    case drm::planes::DRMPlaneType::OVERLAY:
      return "OVERLAY";
    case drm::planes::DRMPlaneType::CURSOR:
      return "CURSOR";
  }
  return "?";
}

std::string fourcc_str(std::uint32_t f) {
  const char c[5] = {char(f), char(f >> 8), char(f >> 16), char(f >> 24), 0};
  return c;
}

const char* placement_name(drm::scene::LayerPlacement p) {
  switch (p) {
    case drm::scene::LayerPlacement::AssignedToPlane:
      return "AssignedToPlane";
    case drm::scene::LayerPlacement::Composited:
      return "Composited";
    case drm::scene::LayerPlacement::Unassigned:
      return "Unassigned (DROPPED)";
  }
  return "?";
}

// Minimal output pickup via libdrm: first CONNECTED connector, its current/first
// encoder's CRTC, and the connector's preferred (or first) mode. Kept inline so
// the probe is a single self-contained TU that links only drm-cxx + libdrm.
struct PickedOutput {
  std::uint32_t crtc_id{0};
  std::uint32_t connector_id{0};
  drmModeModeInfo mode{};
  bool ok{false};
};

PickedOutput pick_output(int fd) {
  PickedOutput out;
  drmModeRes* res = drmModeGetResources(fd);
  if (res == nullptr) {
    std::perror("drmModeGetResources");
    return out;
  }
  for (int i = 0; i < res->count_connectors && !out.ok; ++i) {
    drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[i]);
    if (conn == nullptr) {
      continue;
    }
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0 && conn->encoder_id != 0) {
      drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoder_id);
      if (enc != nullptr && enc->crtc_id != 0) {
        out.connector_id = conn->connector_id;
        out.crtc_id = enc->crtc_id;
        // Prefer the PREFERRED mode, else mode 0.
        out.mode = conn->modes[0];
        for (int m = 0; m < conn->count_modes; ++m) {
          if ((conn->modes[m].type & DRM_MODE_TYPE_PREFERRED) != 0U) {
            out.mode = conn->modes[m];
            break;
          }
        }
        out.ok = true;
      }
      if (enc != nullptr) {
        drmModeFreeEncoder(enc);
      }
    }
    drmModeFreeConnector(conn);
  }
  drmModeFreeResources(res);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = "/dev/dri/card0";
  int num_layers = 3;
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] == '/') {
      path = argv[i];  // device node
    } else if (const int n = std::atoi(argv[i]); n > 0) {
      num_layers = n;  // layer count
    }
    // Other flags (e.g. --no-seat) are ignored: this probe opens the node
    // directly and takes DRM master as the first opener — no libseat needed.
  }

  auto dev_r = drm::Device::open(path);
  if (!dev_r) {
    std::fprintf(stderr, "open %s: %s\n", path, dev_r.error().message().c_str());
    return 1;
  }
  drm::Device& dev = *dev_r;
  if (auto r = dev.enable_universal_planes(); !r) {
    std::fprintf(stderr, "enable_universal_planes: %s\n", r.error().message().c_str());
    return 1;
  }
  if (auto r = dev.enable_atomic(); !r) {
    std::fprintf(stderr, "enable_atomic: %s\n", r.error().message().c_str());
    return 1;
  }

  // ── Phase 1: structural ────────────────────────────────────────────────
  std::printf("=== Phase 1: DriverProfile + PlaneRegistry (%s) ===\n", path);

  auto prof_r = drm::display::DriverProfile::probe(dev);
  if (!prof_r) {
    std::fprintf(stderr, "DriverProfile::probe: %s\n", prof_r.error().message().c_str());
    return 1;
  }
  const auto& p = *prof_r;
  std::printf("driver name           : %s\n", p.name.c_str());
  std::printf("addfb2_modifiers      : %s   (minimal controllers are often LINEAR-only)\n",
              p.addfb2_modifiers ? "true" : "false");
  std::printf("async_page_flip       : %s\n", p.async_page_flip ? "true" : "false");
  std::printf("prime import/export   : %s / %s\n", p.prime_import ? "yes" : "no",
              p.prime_export ? "yes" : "no");
  std::printf("fb_damage_clips       : %s\n", p.fb_damage_clips ? "true" : "false");
  std::printf("vrr_capable           : %s\n", p.vrr_capable ? "true" : "false");
  std::printf("DRM_CAP_CURSOR_*      : %llu x %llu\n",
              static_cast<unsigned long long>(p.cursor_width),
              static_cast<unsigned long long>(p.cursor_height));

  auto reg_r = drm::planes::PlaneRegistry::enumerate(dev);
  if (!reg_r) {
    std::fprintf(stderr, "PlaneRegistry::enumerate: %s\n", reg_r.error().message().c_str());
    return 1;
  }
  const auto& reg = *reg_r;

  int n_primary = 0;
  int n_overlay = 0;
  int n_cursor = 0;
  std::printf("\nplanes: %zu\n", reg.all().size());
  for (const auto& pc : reg.all()) {
    switch (pc.type) {
      case drm::planes::DRMPlaneType::PRIMARY:
        ++n_primary;
        break;
      case drm::planes::DRMPlaneType::OVERLAY:
        ++n_overlay;
        break;
      case drm::planes::DRMPlaneType::CURSOR:
        ++n_cursor;
        break;
    }
    std::printf("  plane %u  type=%-7s  crtcs=0x%x  modifiers=%s  scaling=%s  zpos=[%s..%s]\n",
                pc.id, plane_type_name(pc.type), pc.possible_crtcs,
                pc.has_format_modifiers ? "yes" : "NO", pc.supports_scaling ? "yes" : "no",
                pc.zpos_min ? std::to_string(*pc.zpos_min).c_str() : "-",
                pc.zpos_max ? std::to_string(*pc.zpos_max).c_str() : "-");
    std::printf("           formats:");
    for (const std::uint32_t f : pc.formats) {
      std::printf(" %s", fourcc_str(f).c_str());
    }
    std::printf("\n");
  }

  std::printf("\nplane-type counts: PRIMARY=%d OVERLAY=%d CURSOR=%d\n", n_primary, n_overlay,
              n_cursor);

  // Expectation checks for a minimal single-plane controller (informational — never fatal).
  if (n_primary == 1 && n_overlay == 0 && n_cursor == 0) {
    std::printf("  [OK] minimal single-plane shape: single PRIMARY, no overlay, no cursor.\n");
  } else {
    std::printf("  [..] not the bare single-primary shape (fine for richer controllers).\n");
  }
  // The latent-bug check: cap reports a cursor size but there is no CURSOR plane.
  if (n_cursor == 0 && (p.cursor_width != 0 || p.cursor_height != 0)) {
    std::printf(
        "  [WARN] DRM_CAP_CURSOR_* reports %llux%llu but registry has NO CURSOR plane.\n"
        "         HW-cursor use MUST be gated on the registry, not the cap, or the cursor\n"
        "         path will try to arm a plane that does not exist.\n",
        static_cast<unsigned long long>(p.cursor_width),
        static_cast<unsigned long long>(p.cursor_height));
  }

  // ── Phase 2: allocator behaviour via LayerScene (TEST_ONLY) ────────────
  std::printf("\n=== Phase 2: allocator fallback (LayerScene TEST, %d overlapping layers) ===\n",
              num_layers);

  const PickedOutput po = pick_output(dev.fd());
  if (!po.ok) {
    std::fprintf(stderr,
                 "No connected connector/CRTC/mode found — skipping Phase 2.\n"
                 "(Phase 1 above is the structural truth and needs no output.)\n");
    return 0;
  }
  std::printf("output: connector %u, crtc %u, mode %dx%d@%d\n", po.connector_id, po.crtc_id,
              po.mode.hdisplay, po.mode.vdisplay, po.mode.vrefresh);

  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = po.crtc_id;
  cfg.connector_id = po.connector_id;
  cfg.mode = po.mode;
  auto scene_r = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_r) {
    std::fprintf(stderr, "LayerScene::create: %s\n", scene_r.error().message().c_str());
    return 1;
  }
  auto& scene = *scene_r;

  // Negotiate a format the primary plane scans out — tilcdc & other minimal
  // controllers reject ARGB8888 at AddFB2, which would fail Phase 2 at buffer
  // creation rather than exercising the allocator. Prefer 32-bpp, fall to RGB565.
  const std::array<std::uint32_t, 3> phase2_prefs{DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888,
                                                  DRM_FORMAT_RGB565};
  std::uint32_t fourcc = drm::present::negotiate_scanout_format(dev, po.crtc_id, phase2_prefs);
  if (fourcc == 0) {
    fourcc = DRM_FORMAT_ARGB8888;
  }
  std::printf("layer format          : %.4s\n", reinterpret_cast<const char*>(&fourcc));

  // Add N overlapping full-screen layers, each backed by a dumb buffer (so the
  // composition fallback has CPU-readable pixels to pull from). Full overlap
  // guarantees the allocator can't trivially spread them across planes.
  for (int i = 0; i < num_layers; ++i) {
    auto src_r =
        drm::scene::DumbBufferSource::create(dev, po.mode.hdisplay, po.mode.vdisplay, fourcc);
    if (!src_r) {
      std::fprintf(stderr, "DumbBufferSource %d: %s\n", i, src_r.error().message().c_str());
      return 1;
    }
    drm::scene::LayerDesc desc;
    desc.source = std::move(*src_r);
    desc.display.dst_rect = {0, 0, static_cast<std::uint32_t>(po.mode.hdisplay),
                             static_cast<std::uint32_t>(po.mode.vdisplay)};
    desc.display.zpos = i;
    if (auto h = scene->add_layer(std::move(desc)); !h) {
      std::fprintf(stderr, "add_layer %d: %s\n", i, h.error().message().c_str());
      return 1;
    }
  }

  auto rep_r = scene->test();
  if (!rep_r) {
    std::fprintf(stderr, "scene->test(): %s\n", rep_r.error().message().c_str());
    std::fprintf(stderr, "(A TEST failure here is itself a finding — note the errno.)\n");
    return 1;
  }
  const auto& rep = *rep_r;

  std::printf("\nCommitReport (TEST_ONLY):\n");
  std::printf("  layers_total      : %zu\n", rep.layers_total);
  std::printf("  layers_assigned   : %zu   (hardware planes)\n", rep.layers_assigned);
  std::printf("  layers_composited : %zu   (CPU composition fallback)\n", rep.layers_composited);
  std::printf("  layers_unassigned : %zu   (DROPPED — should be 0)\n", rep.layers_unassigned);
  std::printf("  composition_buckets: %zu\n", rep.composition_buckets);
  std::printf("  test_commits      : %zu\n", rep.test_commits_issued);
  std::printf("  per-layer placements:\n");
  for (const auto& e : rep.placements) {
    std::printf("    handle gen/idx -> %-22s plane_id=%u\n", placement_name(e.placement),
                e.plane_id);
  }

  // Behavioural verdict for a single-plane controller.
  std::printf("\nverdict: ");
  if (rep.layers_unassigned > 0) {
    std::printf("FAIL — %zu layer(s) dropped; composition fallback did not rescue them.\n",
                rep.layers_unassigned);
  } else if (rep.layers_assigned <= 1 && rep.layers_composited >= 1) {
    std::printf(
        "OK — single plane hosted %zu layer(s) directly, %zu rescued via CPU composition; "
        "nothing dropped.\n",
        rep.layers_assigned, rep.layers_composited);
  } else {
    std::printf("OK — %zu assigned, %zu composited (this device has >1 usable plane).\n",
                rep.layers_assigned, rep.layers_composited);
  }
  return 0;
}

// ── Compiling directly on the i.MX93 EVK (no meson/ninja on the BSP image) ──
//
// The NXP BSP ships gcc/g++ + libdrm runtime but no -dev tooling. Carry the
// drm-cxx tree (or its headers + prebuilt libdrm-cxx.so) onto the board, then:
//
//   g++ -std=c++17 -I<drm-cxx>/src main.cpp \
//       -L<drm-cxx-build> -ldrm-cxx $(pkg-config --cflags --libs libdrm) \
//       -o minimal_kms_probe
//
// (the -I points at src/ because public headers resolve as <drm-cxx/...> through
// the build-tree symlink; on an installed SDK use -I<sysroot>/usr/include.)
