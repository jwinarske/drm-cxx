// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// hdr_demo — three-layer LayerScene example exercising the HDR
// signaling, CRTC color pipeline, and CPU tone-map fallback the
// library exposes.
//
// Layers:
//
//   * Background (zpos=3) — P010 buffer with a horizontal PQ-encoded
//     gradient running from black to ~1000 cd/m². With `--mode pq`
//     the scene declares HDR PQ on the connector via the LayerScene
//     auto-derive; with `--mode hlg` the gradient's source_eotf is
//     changed to HLG (the gradient itself stays PQ-encoded so the
//     visual is intentionally wrong — this is the "for visual
//     comparison" path the design calls out); with `--mode sdr` the
//     gradient runs as plain BT.709 SDR.
//   * SDR overlay (zpos=4) — ARGB8888 corner badge with a colored
//     square. Always BT.709 so the demo always shows what mixing
//     SDR with HDR looks like.
//   * HLG square (zpos=5) — P010 buffer with HLG-encoded full
//     luminance. `source_eotf=Bt2100Hlg` documents the layer's
//     intent; on most hardware (no per-plane CMS) the kernel
//     scans both as PQ.
//
// CLI:
//
//   hdr_demo [--mode {pq|hlg|sdr}] [--target-nits N]
//            [--no-hw-pipeline] [/dev/dri/cardN]
//
// The demo commits once with the configured layers + HDR
// signaling, holds for `--hold-seconds N` (default 5) so the
// caller can observe the output, then tears down cleanly.

#include "../../common/open_output.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/display/connector_capabilities.hpp>
#include <drm-cxx/display/crtc_capabilities.hpp>
#include <drm-cxx/display/crtc_color_pipeline.hpp>
#include <drm-cxx/display/hdr_metadata.hpp>
#include <drm-cxx/display/tone_mapper.hpp>
#include <drm-cxx/scene/display_params.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/scene/output_signaling.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int k_default_hold_seconds = 5;

enum class Mode : std::uint8_t { Pq, Hlg, Sdr };

struct Args {
  Mode mode{Mode::Pq};
  bool no_hw_pipeline{false};
  bool tone_map{false};
  bool dry_run{false};
  std::optional<float> target_nits;
  int hold_seconds{k_default_hold_seconds};
};

[[nodiscard]] Args parse_args(int& argc, char**& argv) {
  Args a;
  // Walk argv consuming our flags; leave the device-path argument
  // (if any) in place for `open_and_pick_output` to find.
  int write = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--mode" && (i + 1) < argc) {
      const std::string_view v{argv[++i]};
      if (v == "pq") {
        a.mode = Mode::Pq;
      } else if (v == "hlg") {
        a.mode = Mode::Hlg;
      } else if (v == "sdr") {
        a.mode = Mode::Sdr;
      } else {
        drm::println(stderr, "hdr_demo: unknown --mode {} (expected pq|hlg|sdr)", v);
      }
    } else if (arg == "--target-nits" && (i + 1) < argc) {
      a.target_nits = static_cast<float>(std::atof(argv[++i]));
    } else if (arg == "--hold-seconds" && (i + 1) < argc) {
      a.hold_seconds = std::atoi(argv[++i]);
    } else if (arg == "--no-hw-pipeline") {
      a.no_hw_pipeline = true;
    } else if (arg == "--tone-map") {
      a.tone_map = true;
    } else if (arg == "--dry-run") {
      a.dry_run = true;
    } else {
      argv[write++] = argv[i];
    }
  }
  argc = write;
  return a;
}

const char* mode_name(Mode m) noexcept {
  switch (m) {
    case Mode::Pq:
      return "pq";
    case Mode::Hlg:
      return "hlg";
    case Mode::Sdr:
      return "sdr";
  }
  return "?";
}

const char* tier_name(bool no_hw, bool has_crtc_pipeline) noexcept {
  if (no_hw) {
    return "cpu-fallback (forced via --no-hw-pipeline)";
  }
  if (has_crtc_pipeline) {
    return "crtc-pipeline";
  }
  return "no-hw-pipeline-available (kernel-default scanout)";
}

// Read an AMD_PLANE_*_LUT_SIZE from the first plane advertising it (uniform
// across a device's planes); 0 when the driver has no AMD plane color pipeline.
std::uint64_t plane_lut_size(int fd, const char* name) {
  std::uint64_t sz = 0;
  drmModePlaneResPtr pr = drmModeGetPlaneResources(fd);
  if (pr == nullptr) {
    return sz;
  }
  for (std::uint32_t i = 0; i < pr->count_planes && sz == 0; ++i) {
    drmModeObjectProperties* props =
        drmModeObjectGetProperties(fd, pr->planes[i], DRM_MODE_OBJECT_PLANE);
    if (props != nullptr) {
      for (std::uint32_t j = 0; j < props->count_props && sz == 0; ++j) {
        drmModePropertyPtr p = drmModeGetProperty(fd, props->props[j]);
        if (p != nullptr) {
          if (std::strcmp(p->name, name) == 0) {
            sz = props->prop_values[j];
          }
          drmModeFreeProperty(p);
        }
      }
      drmModeFreeObjectProperties(props);
    }
  }
  drmModeFreePlaneResources(pr);
  return sz;
}

// Pack an 8-bit ARGB pixel into the 16-bit-per-channel u64 ToneMapper expects
// (x*257 maps 0..255 -> 0..65535), apply the mapper, and unpack back to ARGB.
// Mirrors src/scene/composite_canvas.cpp's apply_tone_mapper_argb. The software
// tone-map fallback.
std::uint32_t sw_tonemap_argb(std::uint32_t argb, const drm::display::ToneMapper& tm) {
  const auto a = static_cast<std::uint16_t>(((argb >> 24U) & 0xFFU) * 257U);
  const auto r = static_cast<std::uint16_t>(((argb >> 16U) & 0xFFU) * 257U);
  const auto g = static_cast<std::uint16_t>(((argb >> 8U) & 0xFFU) * 257U);
  const auto b = static_cast<std::uint16_t>((argb & 0xFFU) * 257U);
  const std::uint64_t packed =
      static_cast<std::uint64_t>(r) | (static_cast<std::uint64_t>(g) << 16U) |
      (static_cast<std::uint64_t>(b) << 32U) | (static_cast<std::uint64_t>(a) << 48U);
  const std::uint64_t o = tm(packed);
  const auto u8 = [](std::uint64_t c) { return static_cast<std::uint32_t>((c & 0xFFFFU) / 257U); };
  return (static_cast<std::uint32_t>((argb >> 24U) & 0xFFU) << 24U) | (u8(o) << 16U) |
         (u8(o >> 16U) << 8U) | u8(o >> 32U);
}

// Bake a ToneMapper into an amdgpu AMD_PLANE_LUT3D cube (dim^3 entries,
// blue-major then green then red) by sampling the mapper at each grid point.
// With Identity surrounding stages, the cube carries the full encoded->encoded
// transform the hardware applies at scanout. The hardware tone-map.
std::vector<drm::scene::ColorLutEntry> bake_tonemap_cube(const drm::display::ToneMapper& tm,
                                                         std::uint64_t dim) {
  std::vector<drm::scene::ColorLutEntry> v;
  v.reserve(dim * dim * dim);
  const auto q = [dim](std::uint64_t i) {
    return dim > 1 ? static_cast<std::uint16_t>(i * 65535ULL / (dim - 1)) : std::uint16_t{0};
  };
  for (std::uint64_t b = 0; b < dim; ++b) {
    for (std::uint64_t g = 0; g < dim; ++g) {
      for (std::uint64_t r = 0; r < dim; ++r) {
        const std::uint64_t packed =
            static_cast<std::uint64_t>(q(r)) | (static_cast<std::uint64_t>(q(g)) << 16U) |
            (static_cast<std::uint64_t>(q(b)) << 32U) | (std::uint64_t{0xFFFFU} << 48U);
        const std::uint64_t o = tm(packed);
        v.push_back({static_cast<std::uint16_t>(o & 0xFFFFU),
                     static_cast<std::uint16_t>((o >> 16U) & 0xFFFFU),
                     static_cast<std::uint16_t>((o >> 32U) & 0xFFFFU)});
      }
    }
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) try {
  auto args = parse_args(argc, argv);

  auto out = drm::examples::open_and_pick_output(argc, argv);
  if (!out) {
    return EXIT_FAILURE;
  }
  auto& dev = out->device;
  const auto w = static_cast<std::uint32_t>(out->mode.hdisplay);
  const auto h = static_cast<std::uint32_t>(out->mode.vdisplay);

  drm::println(stderr, "hdr_demo — mode={} {}x{}", mode_name(args.mode), w, h);

  // connector capability probe.
  const auto conn_caps_r = drm::display::probe_connector_capabilities(dev, out->connector_id);
  if (conn_caps_r) {
    const auto& c = *conn_caps_r;
    drm::println(stderr,
                 "  connector caps: max_bpc={}..{} hdr_blob={} colorspace={} can_signal_hdr={}",
                 c.max_bpc_min.value_or(0), c.max_bpc_max.value_or(0), c.has_hdr_output_metadata,
                 c.has_colorspace, c.can_signal_hdr());
  }

  // CRTC color pipeline probe.
  const auto crtc_caps_r = drm::display::probe_crtc_capabilities(dev, out->crtc_id);
  bool has_crtc_pipeline = false;
  if (crtc_caps_r) {
    const auto& c = *crtc_caps_r;
    has_crtc_pipeline = c.has_full_pipeline();
    drm::println(stderr, "  crtc caps: degamma={} ctm={} gamma={} (sizes degamma={} gamma={})",
                 c.has_degamma_lut, c.has_ctm, c.has_gamma_lut, c.degamma_lut_size,
                 c.gamma_lut_size);
  }
  drm::println(stderr, "  active tier: {}", tier_name(args.no_hw_pipeline, has_crtc_pipeline));

  // Build the LayerScene.
  const drm::scene::LayerScene::Config cfg{out->crtc_id, out->connector_id, out->mode};
  auto scene_r = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_r) {
    drm::println(stderr, "LayerScene::create: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto& scene = **scene_r;

  // set up CRTC pipeline if available and not disabled.
  std::optional<drm::display::CrtcColorPipeline> pipeline;
  if (has_crtc_pipeline && !args.no_hw_pipeline) {
    auto p = drm::display::CrtcColorPipeline::create(dev, out->crtc_id);
    if (p && p->set_identity()) {
      pipeline = std::move(*p);
      drm::println(stderr, "  CRTC pipeline: identity LUT/CTM/LUT armed");
    }
  }

  // ── Layer 1: SDR-encoded "gradient" background ────────────────
  // The design calls for P010 + PQ-encoded content here, but amdgpu
  // RDNA doesn't accept P010 on the primary scanout plane on every
  // generation — the demo runs on the broadest set of hardware if
  // the pixel content stays ARGB8888 and the HDR signaling rides
  // on the connector's HDR_OUTPUT_METADATA blob plus the
  // per-layer DisplayParams tagging. For an
  // RDNA3+ / Battlemage demo the layer's drm_fourcc could change
  // to P010 with a real PQ ramp; the rest of the wiring is
  // identical.
  auto bg_src_r = drm::scene::DumbBufferSource::create(dev, w, h, DRM_FORMAT_ARGB8888);
  if (!bg_src_r) {
    drm::println(stderr, "background (ARGB8888 {}x{}): {}", w, h, bg_src_r.error().message());
    return EXIT_FAILURE;
  }
  // HDR tone-map (--tone-map): map the PQ/HLG content toward the display.
  // Hardware-first — if the plane exposes the AMD color pipeline, bake the
  // ToneMapper into its 3D-LUT and the driver applies it at scanout (the layer
  // keeps the source ramp). Otherwise fall back to the software ToneMapper run
  // over the pixels (CPU). Both use the same ToneMapper, so the result matches
  // across the fleet (Deck = hardware; host amdgpu + VOP2 = software).
  const bool want_tonemap = args.tone_map && args.mode != Mode::Sdr;
  const std::uint64_t cube_dim =
      want_tonemap ? plane_lut_size(dev.fd(), "AMD_PLANE_LUT3D_SIZE") : 0;
  const bool hw_tonemap = want_tonemap && cube_dim > 0 && !args.no_hw_pipeline;
  const float tm_nits = args.target_nits.value_or(100.0F);
  std::optional<drm::display::ToneMapper> tm;
  if (want_tonemap) {
    tm = args.mode == Mode::Hlg ? drm::display::ToneMapper::hlg_to_bt709(tm_nits)
                                : drm::display::ToneMapper::bt2020_pq_to_bt709(tm_nits);
  }

  // Paint a test pattern: saturated color bars (top two-thirds) that exercise the
  // BT.2020 -> BT.709 gamut conversion and the full 3D-LUT cube — greys are
  // gamut-invariant, so only colored input drives the matrix — plus a grey ramp
  // (bottom third) for the luminance tone-curve. On the software path tone-map
  // each pixel as we write it; on hardware leave it for the 3D-LUT to transform.
  // Comparing the hardware vs software output also validates the cube ordering:
  // a wrong LUT3D traversal would swap the bars' colors on the hardware path.
  const bool sw_tonemap = want_tonemap && !hw_tonemap;
  if (auto m = (*bg_src_r)->map(drm::MapAccess::Write)) {
    auto& mapping = *m;
    if (auto* base = mapping.pixels().data(); base != nullptr) {
      const auto stride = mapping.stride();
      constexpr std::array<std::uint32_t, 8> bars{
          0xFFFFFFFFU, 0xFFFFFF00U, 0xFF00FFFFU, 0xFF00FF00U,  // white yellow cyan green
          0xFFFF00FFU, 0xFFFF0000U, 0xFF0000FFU, 0xFF000000U,  // magenta red blue black
      };
      const std::uint32_t bars_h = h * 2U / 3U;  // color bars above, grey ramp below
      for (std::uint32_t y = 0; y < h; ++y) {
        auto* px = reinterpret_cast<std::uint32_t*>(base + (static_cast<std::size_t>(y) * stride));
        for (std::uint32_t x = 0; x < w; ++x) {
          std::uint32_t argb = 0xFF000000U;
          if (y < bars_h) {
            argb = bars.at((x * bars.size()) / (w > 0 ? w : 1));
          } else {
            const auto v = static_cast<std::uint8_t>((x * 0xFFU) / (w > 1 ? (w - 1) : 1));
            argb = (0xFFU << 24U) | (static_cast<std::uint32_t>(v) << 16U) |
                   (static_cast<std::uint32_t>(v) << 8U) | static_cast<std::uint32_t>(v);
          }
          if (sw_tonemap) {
            argb = sw_tonemap_argb(argb, *tm);
          }
          px[x] = argb;
        }
      }
    }
  }

  drm::scene::LayerDesc bg_desc;
  bg_desc.source = std::move(*bg_src_r);
  bg_desc.display.zpos = 3;
  bg_desc.display.color_primaries = drm::scene::ColorPrimaries::Bt2020;
  if (args.mode == Mode::Pq) {
    bg_desc.display.source_eotf = drm::display::TransferFunction::SmpteSt2084Pq;
  } else if (args.mode == Mode::Hlg) {
    bg_desc.display.source_eotf = drm::display::TransferFunction::Bt2100Hlg;
  }
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, w, h};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, w, h};
  if (hw_tonemap) {
    // Bake the ToneMapper into the plane's 3D-LUT; the surrounding stages pass
    // through (Identity) so the LUT carries the whole encoded->encoded transform
    // amdgpu applies at scanout. Composes with the HDR_OUTPUT_METADATA above.
    using PTF = drm::scene::PlaneTransferFunction;
    auto& ac = bg_desc.display.amd_color;
    ac.degamma_tf = PTF::Identity;
    ac.shaper_tf = PTF::Identity;
    ac.blend_tf = PTF::Identity;
    ac.lut3d = bake_tonemap_cube(*tm, cube_dim);
    scene.set_output_transfer_function(drm::scene::LayerScene::OutputTransferFunction::Identity);
  }
  if (want_tonemap) {
    drm::println(stderr, "  tone-map: {} (target {} nits)",
                 hw_tonemap ? "HARDWARE (AMD plane 3D-LUT)" : "SOFTWARE (ToneMapper CPU fallback)",
                 tm_nits);
  }
  if (auto rh = scene.add_layer(std::move(bg_desc)); !rh) {
    drm::println(stderr, "add_layer(bg): {}", rh.error().message());
    return EXIT_FAILURE;
  }

  // ── Layer 2: SDR overlay corner badge ──────────────────────────
  std::uint32_t badge_color = 0xFF0000FFU;  // PQ → blue
  if (args.mode == Mode::Hlg) {
    badge_color = 0xFF00FF00U;  // HLG → green
  } else if (args.mode == Mode::Sdr) {
    badge_color = 0xFFFF0000U;  // SDR → red
  }
  constexpr std::uint32_t k_badge_w = 192;
  constexpr std::uint32_t k_badge_h = 64;
  auto overlay_src_r =
      drm::scene::DumbBufferSource::create(dev, k_badge_w, k_badge_h, DRM_FORMAT_ARGB8888);
  if (!overlay_src_r) {
    drm::println(stderr, "SDR overlay: {}", overlay_src_r.error().message());
    return EXIT_FAILURE;
  }
  if (auto m = (*overlay_src_r)->map(drm::MapAccess::Write)) {
    auto& mapping = *m;
    if (auto* base = mapping.pixels().data(); base != nullptr) {
      auto* px = reinterpret_cast<std::uint32_t*>(base);
      const std::uint32_t stride_px = mapping.stride() / 4U;
      for (std::uint32_t y = 0; y < k_badge_h; ++y) {
        auto* row = px + (static_cast<std::size_t>(y) * stride_px);
        for (std::uint32_t x = 0; x < k_badge_w; ++x) {
          row[x] = badge_color;
        }
      }
    }
  }
  drm::scene::LayerDesc overlay_desc;
  overlay_desc.source = std::move(*overlay_src_r);
  overlay_desc.display.zpos = 4;
  overlay_desc.display.color_primaries = drm::scene::ColorPrimaries::Bt709;
  overlay_desc.display.src_rect = drm::scene::Rect{0, 0, k_badge_w, k_badge_h};
  overlay_desc.display.dst_rect =
      drm::scene::Rect{static_cast<std::int32_t>(w - k_badge_w - 16), 16, k_badge_w, k_badge_h};
  if (auto rh = scene.add_layer(std::move(overlay_desc)); !rh) {
    drm::println(stderr, "add_layer(overlay): {}", rh.error().message());
  }

  // ── Layer 3: HLG square ────────────────────────────────────────
  // ARGB8888 placeholder painted bright white to approximate the
  // HLG full-luminance look. As with the background, real HLG-
  // encoded P010 content needs a generation that accepts P010 on
  // the scanout plane.
  constexpr std::uint32_t k_square_w = 256;
  constexpr std::uint32_t k_square_h = 256;
  auto hlg_src_r =
      drm::scene::DumbBufferSource::create(dev, k_square_w, k_square_h, DRM_FORMAT_ARGB8888);
  if (hlg_src_r) {
    if (auto m = (*hlg_src_r)->map(drm::MapAccess::Write)) {
      auto& mapping = *m;
      if (auto* base = mapping.pixels().data(); base != nullptr) {
        auto* px = reinterpret_cast<std::uint32_t*>(base);
        const auto stride_px = mapping.stride() / 4U;
        for (std::uint32_t y = 0; y < k_square_h; ++y) {
          auto* row = px + (static_cast<std::size_t>(y) * stride_px);
          for (std::uint32_t x = 0; x < k_square_w; ++x) {
            row[x] = 0xFFFFFFFFU;
          }
        }
      }
    }
    drm::scene::LayerDesc hlg_desc;
    hlg_desc.source = std::move(*hlg_src_r);
    hlg_desc.display.zpos = 5;
    hlg_desc.display.color_primaries = drm::scene::ColorPrimaries::Bt2020;
    hlg_desc.display.source_eotf = drm::display::TransferFunction::Bt2100Hlg;
    hlg_desc.display.src_rect = drm::scene::Rect{0, 0, k_square_w, k_square_h};
    const auto cy = static_cast<std::int32_t>((h - k_square_h) / 2U);
    hlg_desc.display.dst_rect = drm::scene::Rect{static_cast<std::int32_t>((w - k_square_w) / 2U),
                                                 cy, k_square_w, k_square_h};
    if (auto rh = scene.add_layer(std::move(hlg_desc)); !rh) {
      drm::println(stderr, "add_layer(hlg_square): {}", rh.error().message());
    }
  } else {
    drm::println(stderr, "HLG square ({}x{}): {}", k_square_w, k_square_h,
                 hlg_src_r.error().message());
  }

  // Manual HDR metadata override: auto-derive populates
  // an HdrSourceMetadata from the layer EOTFs but leaves luminance
  // fields at 0 (the auto-derive doesn't know what max_cll the
  // mastering setup intended). Fill them in for non-SDR modes.
  if (args.mode != Mode::Sdr) {
    drm::display::HdrSourceMetadata md;
    md.eotf = (args.mode == Mode::Pq) ? drm::display::TransferFunction::SmpteSt2084Pq
                                      : drm::display::TransferFunction::Bt2100Hlg;
    md.display_primaries =
        drm::scene::color_primaries_to_colorimetry(drm::scene::ColorPrimaries::Bt2020);
    const auto target = static_cast<std::uint16_t>(args.target_nits.value_or(1000.0F));
    md.max_display_mastering_luminance = target;
    md.min_display_mastering_luminance = 50;  // 0.005 cd/m² (50 * 0.0001)
    md.max_content_light_level = target;
    md.max_frame_average_light_level =
        static_cast<std::uint16_t>(static_cast<float>(target) * 0.4F);
    scene.set_output_metadata(md);
    drm::println(stderr, "  HDR metadata: eotf={} mastering_max={} maxCLL={} maxFALL={}",
                 (md.eotf == drm::display::TransferFunction::SmpteSt2084Pq) ? "PQ" : "HLG",
                 md.max_display_mastering_luminance, md.max_content_light_level,
                 md.max_frame_average_light_level);
  }

  if (args.dry_run) {
    drm::println(stderr, "hdr_demo: --dry-run, skipping commit");
    return EXIT_SUCCESS;
  }

  // Test commit + real commit. amdgpu generations vary in how they
  // accept simultaneous Colorspace + HDR_OUTPUT_METADATA + multi-
  // layer-plane-assignment changes; if the test commit comes back
  // EINVAL, fall back to dry-run output rather than failing — the
  // demo still printed the configuration it would have committed,
  // which is the documentation half of its job.
  if (auto r = scene.test(); !r) {
    drm::println(stderr,
                 "TEST commit refused: {} (kernel rejected this layer/HDR combination "
                 "on this hardware; demo output above is the configured shape)",
                 r.error().message());
    return EXIT_SUCCESS;
  }
  if (auto r = scene.commit(); !r) {
    drm::println(stderr,
                 "commit refused: {} (kernel rejected the real commit; the TEST commit "
                 "passed but live scanout was declined)",
                 r.error().message());
    return EXIT_SUCCESS;
  }
  drm::println(stderr, "hdr_demo: scene committed; holding for {} seconds", args.hold_seconds);

  std::this_thread::sleep_for(std::chrono::seconds(args.hold_seconds));
  drm::println(stderr, "hdr_demo: tearing down");
  return EXIT_SUCCESS;
} catch (...) {
  return EXIT_FAILURE;
}
