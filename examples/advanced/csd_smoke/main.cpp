// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// csd_smoke — minimal hardware-validation harness for drm::csd.
//
// Opens a DRM device, picks the first connected output, paints a
// horizontal gradient across the primary, then renders ONE glass
// decoration via drm::csd::Renderer into a drm::csd::Surface and
// scans it out on an overlay plane.
//
// Usage: csd_smoke [--seconds N] [--theme {default|lite|minimal}]
//                  [--png OUT.png] [/dev/dri/cardN]
//
//   --seconds N       Hold the composition on screen for N seconds.
//                     Default 5.
//   --theme NAME      Built-in theme. Default "default" (glass-default,
//                     Tier 0). "lite" maps to glass-lite (Tier 1) and
//                     "minimal" to glass-minimal (Tier 2).
//   --png OUT.png     After arming the composition, snapshot the CRTC
//                     via drm::capture and write a PNG. Useful for
//                     headless regression checks; the file lands at
//                     OUT.png in the cwd unless an absolute path is
//                     given. The example still holds for --seconds
//                     before exiting so the on-screen result remains
//                     observable.
//   /dev/dri/cardN    DRM device; select_device prompts if omitted.
//
// Preconditions:
//   - Run from a TTY (Ctrl-Alt-F3) or a libseat session — a Wayland /
//     X session holding DRM master will reject enable_atomic with
//     EACCES.
//   - DRM_CXX_HAS_BLEND2D=1 in the build (the gate that pulls in csd).
//
// What this validates end-to-end on real hardware:
//
//   1. csd::Surface::create returns a CPU-mappable, KMS-scanout-ready
//      ARGB8888 buffer. Both the GBM and dumb paths are exercised
//      depending on whether GBM is available; the chosen path is
//      logged.
//   2. csd::Renderer::draw paints the glass theme into the Surface's
//      mapping — shadow halo from the cache, panel gradient, specular,
//      noise, title text (when a system font is available), traffic-
//      light buttons, rim. Pixel correctness is asserted by the unit
//      tests; the smoke proves the Renderer doesn't crash on a real
//      device's allocator and that BLContext::end() flushes before
//      scanout.
//   3. The painted Surface's FB ID reaches an overlay plane via
//      drm::scene::LayerScene's allocator, which means the (format,
//      modifier, zpos) story is consistent with what the production
//      Plane presenter (Tier 0) will need.
//   4. Optional --png round-trip: drm::capture::snapshot reads back
//      the same composition that's currently on screen, encoded to
//      PNG. The file should visually match the on-screen rendering.

#include "../../common/open_output.hpp"
#include "csd/renderer.hpp"
#include "csd/shadow_cache.hpp"
#include "csd/surface.hpp"
#include "csd/theme.hpp"
#include "csd/window_state.hpp"
#include "scene/buffer_source.hpp"
#include "scene/dumb_buffer_source.hpp"
#include "scene/layer_desc.hpp"
#include "scene/layer_scene.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/capture/png.hpp>
#include <drm-cxx/capture/snapshot.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/modeset/page_flip.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace {

// ── Surface adapter ──────────────────────────────────────────────
//
// drm::csd::Surface is the production CSD backing buffer (GBM-or-dumb,
// CPU-mapped, FB ID exported). To feed its FB into LayerScene we wrap
// it in a tiny adapter that fulfills the LayerBufferSource contract.
// The adapter is intentionally file-local — production presenter code
// (Step 5 of the Blend2D plan) will own its own surface→plane wiring
// and won't go through LayerScene at all.
class SurfaceLayerSource : public drm::scene::LayerBufferSource {
 public:
  explicit SurfaceLayerSource(drm::csd::Surface surface) noexcept : surface_(std::move(surface)) {
    fmt_.drm_fourcc = surface_.format();
    fmt_.modifier = DRM_FORMAT_MOD_LINEAR;
    fmt_.width = surface_.width();
    fmt_.height = surface_.height();
  }

  drm::expected<drm::scene::AcquiredBuffer, std::error_code> acquire() override {
    drm::scene::AcquiredBuffer out;
    out.fb_id = surface_.fb_id();
    return out;
  }

  void release(drm::scene::AcquiredBuffer /*acquired*/) noexcept override {}

  [[nodiscard]] drm::scene::BindingModel binding_model() const noexcept override {
    return drm::scene::BindingModel::SceneSubmitsFbId;
  }

  [[nodiscard]] drm::scene::SourceFormat format() const noexcept override { return fmt_; }

  drm::expected<drm::BufferMapping, std::error_code> map(drm::MapAccess access) override {
    return surface_.paint(access);
  }

 private:
  drm::csd::Surface surface_;
  drm::scene::SourceFormat fmt_{};
};

// ── Helpers ──────────────────────────────────────────────────────

const drm::csd::Theme& pick_theme(std::string_view name) {
  if (name == "lite") {
    return drm::csd::glass_lite_theme();
  }
  if (name == "minimal") {
    return drm::csd::glass_minimal_theme();
  }
  return drm::csd::glass_default_theme();
}

// Paint a horizontal black-to-white gradient into a CPU-linear XRGB8888
// buffer. Same trick atomic_modeset uses — proves the buffer reaches
// scanout and the byte order is right.
void paint_gradient(drm::span<std::uint8_t> pixels, std::uint32_t stride, std::uint32_t w,
                    std::uint32_t h) {
  for (std::uint32_t y = 0; y < h; ++y) {
    auto* row = pixels.data() + (static_cast<std::size_t>(y) * stride);
    for (std::uint32_t x = 0; x < w; ++x) {
      const auto v = static_cast<std::uint8_t>((x * 255U) / (w == 0 ? 1 : (w - 1U)));
      row[(x * 4U) + 0U] = v;  // B
      row[(x * 4U) + 1U] = v;  // G
      row[(x * 4U) + 2U] = v;  // R
      row[(x * 4U) + 3U] = 0;  // X
    }
  }
}

struct Args {
  std::uint32_t seconds{5};
  std::string theme_name{"default"};
  std::string png_path;
};

// Parse and strip our flags from argv so the leftover positional (the
// device path) lands at argv[1] for select_device. Same shape as
// cursor_rotate's strip helper.
Args parse_args(int& argc, char* argv[]) {
  Args a;
  auto strip = [&](int i, int n) {
    for (int j = i; j + n < argc; ++j) {
      argv[j] = argv[j + n];
    }
    argc -= n;
  };
  for (int i = 1; i < argc;) {
    const std::string_view arg = argv[i];
    if (arg == "--seconds" && i + 1 < argc) {
      a.seconds = static_cast<std::uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
      strip(i, 2);
    } else if (arg == "--theme" && i + 1 < argc) {
      a.theme_name = argv[i + 1];
      strip(i, 2);
    } else if (arg == "--png" && i + 1 < argc) {
      a.png_path = argv[i + 1];
      strip(i, 2);
    } else {
      ++i;
    }
  }
  return a;
}

}  // namespace

int main(int argc, char* argv[]) {
  const Args args = parse_args(argc, argv);  // strips its own flags

  auto output = drm::examples::open_and_pick_output(argc, argv);
  if (!output) {
    return EXIT_FAILURE;
  }
  auto& dev = output->device;
  const drmModeModeInfo mode = output->mode;
  const std::uint32_t fb_w = mode.hdisplay;
  const std::uint32_t fb_h = mode.vdisplay;
  drm::println("csd_smoke: {}x{}@{}Hz on connector {} / CRTC {}", fb_w, fb_h, mode.vrefresh,
               output->connector_id, output->crtc_id);

  // ── Background gradient ──────────────────────────────────────
  auto bg_src_res = drm::scene::DumbBufferSource::create(dev, fb_w, fb_h, DRM_FORMAT_XRGB8888);
  if (!bg_src_res) {
    drm::println(stderr, "DumbBufferSource::create (bg): {}", bg_src_res.error().message());
    return EXIT_FAILURE;
  }
  auto bg_src = std::move(*bg_src_res);
  {
    auto map = bg_src->map(drm::MapAccess::Write);
    if (!map) {
      drm::println(stderr, "bg.map: {}", map.error().message());
      return EXIT_FAILURE;
    }
    paint_gradient(map->pixels(), map->stride(), fb_w, fb_h);
  }

  // ── Decoration via csd::Surface + Renderer ───────────────────
  // 800×200 is large enough to read the title text + see all three
  // buttons, small enough to fit comfortably above any FullHD output.
  drm::csd::SurfaceConfig deco_cfg;
  deco_cfg.width = 800;
  deco_cfg.height = 200;
  auto surface_res = drm::csd::Surface::create(dev, deco_cfg);
  if (!surface_res) {
    drm::println(stderr, "csd::Surface::create: {}", surface_res.error().message());
    return EXIT_FAILURE;
  }
  auto surface = std::move(*surface_res);
  drm::println("csd_smoke: Surface backing = {}",
               surface.backing() == drm::csd::SurfaceBacking::Gbm ? "GBM" : "dumb");

  drm::csd::ShadowCache shadows;
  drm::csd::Renderer renderer;
  drm::println("csd_smoke: renderer.has_font() = {}", renderer.has_font());

  drm::csd::WindowState state;
  state.title = "drm-cxx csd_smoke";
  state.focused = true;
  state.hover = drm::csd::HoverButton::None;

  const drm::csd::Theme& theme = pick_theme(args.theme_name);
  drm::println("csd_smoke: theme = {}", theme.name);
  {
    auto map = surface.paint(drm::MapAccess::ReadWrite);
    if (!map) {
      drm::println(stderr, "surface.paint: {}", map.error().message());
      return EXIT_FAILURE;
    }
    if (auto r = renderer.draw(theme, state, *map, shadows); !r) {
      drm::println(stderr, "renderer.draw: {}", r.error().message());
      return EXIT_FAILURE;
    }
  }

  auto deco_src = std::make_unique<SurfaceLayerSource>(std::move(surface));

  // ── Scene assembly ───────────────────────────────────────────
  drm::scene::LayerScene::Config scene_cfg;
  scene_cfg.crtc_id = output->crtc_id;
  scene_cfg.connector_id = output->connector_id;
  scene_cfg.mode = mode;
  auto scene_res = drm::scene::LayerScene::create(dev, scene_cfg);
  if (!scene_res) {
    drm::println(stderr, "LayerScene::create: {}", scene_res.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_res);

  drm::scene::LayerDesc bg_desc;
  bg_desc.source = std::move(bg_src);
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  if (auto r = scene->add_layer(std::move(bg_desc)); !r) {
    drm::println(stderr, "add_layer (bg): {}", r.error().message());
    return EXIT_FAILURE;
  }

  // Center the decoration horizontally, place it ~80 px from the top.
  const std::uint32_t deco_w = 800;
  const std::uint32_t deco_h = 200;
  const std::int32_t deco_x = fb_w > deco_w ? static_cast<std::int32_t>((fb_w - deco_w) / 2U) : 0;
  const std::int32_t deco_y = 80;
  drm::scene::LayerDesc deco_desc;
  deco_desc.source = std::move(deco_src);
  deco_desc.display.src_rect = drm::scene::Rect{0, 0, deco_w, deco_h};
  deco_desc.display.dst_rect = drm::scene::Rect{deco_x, deco_y, deco_w, deco_h};
  // Force the decoration above the bg — amdgpu pins the primary at
  // zpos=2, so 3 lands on the first overlay above it.
  deco_desc.display.zpos = 3;
  if (auto r = scene->add_layer(std::move(deco_desc)); !r) {
    drm::println(stderr, "add_layer (deco): {}", r.error().message());
    return EXIT_FAILURE;
  }

  // ── Commit + wait for first flip ─────────────────────────────
  drm::PageFlip page_flip(dev);
  bool flipped = false;
  page_flip.set_handler([&](std::uint32_t crtc, std::uint64_t seq, std::uint64_t /*ts*/) {
    drm::println("csd_smoke: page flip on CRTC {}: seq={}", crtc, seq);
    flipped = true;
  });

  if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    drm::println(stderr, "scene.commit: {}", r.error().message());
    return EXIT_FAILURE;
  }
  while (!flipped) {
    if (auto r = page_flip.dispatch(-1); !r) {
      drm::println(stderr, "page_flip.dispatch: {}", r.error().message());
      return EXIT_FAILURE;
    }
  }

  // ── Optional readback ────────────────────────────────────────
  if (!args.png_path.empty()) {
    auto snap = drm::capture::snapshot(dev, output->crtc_id);
    if (!snap) {
      drm::println(stderr, "capture::snapshot: {}", snap.error().message());
    } else if (auto r = drm::capture::write_png(*snap, args.png_path); !r) {
      drm::println(stderr, "capture::write_png ({}): {}", args.png_path, r.error().message());
    } else {
      drm::println("csd_smoke: wrote {}", args.png_path);
    }
  }

  drm::println("csd_smoke: holding for {}s", args.seconds);
  std::this_thread::sleep_for(std::chrono::seconds(args.seconds));
  return EXIT_SUCCESS;
}
