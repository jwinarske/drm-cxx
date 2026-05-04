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
//                  [--presenter {scene|plane}] [--png OUT.png]
//                  [/dev/dri/cardN]
//
//   --seconds N       Hold the composition on screen for N seconds.
//                     Default 5.
//   --theme NAME      Built-in theme. Default "default" (glass-default,
//                     Tier 0). "lite" maps to glass-lite (Tier 1) and
//                     "minimal" to glass-minimal (Tier 2).
//   --presenter MODE  How the decoration reaches the overlay plane:
//                       "scene" (default) — drm::scene::LayerScene
//                          drives both bg and decoration through one
//                          atomic commit. Validates the legacy path
//                          and stays the smallest reproducer for
//                          renderer / surface bugs.
//                       "plane" — LayerScene draws the bg only, then
//                          drm::csd::OverlayReservation reserves an
//                          overlay and drm::csd::PlanePresenter arms
//                          it via a second atomic commit. Validates
//                          the production presenter path end-to-end
//                          on real hardware. Two flips total; the
//                          one-frame bg-only window between them is
//                          the smoke variant of what an MDI shell
//                          sees during initial armup.
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
//   3. The painted Surface's FB ID reaches an overlay plane. In the
//      default `--presenter=scene` mode this routes through
//      drm::scene::LayerScene's allocator, which also validates that
//      the (format, modifier, zpos) story matches what the
//      production presenter expects. In `--presenter=plane` mode it
//      routes through drm::csd::OverlayReservation +
//      drm::csd::PlanePresenter — the actual production path the MDI
//      demo will use.
//   4. Optional --png round-trip: drm::capture::snapshot reads back
//      the same composition that's currently on screen, encoded to
//      PNG. The file should visually match the on-screen rendering.

#include "../../common/open_output.hpp"
#include "csd/overlay_reservation.hpp"
#include "csd/presenter.hpp"
#include "csd/presenter_plane.hpp"
#include "csd/renderer.hpp"
#include "csd/shadow_cache.hpp"
#include "csd/surface.hpp"
#include "csd/theme.hpp"
#include "csd/window_state.hpp"
#include "gbm/device.hpp"
#include "planes/plane_registry.hpp"
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
#include <drm-cxx/modeset/atomic.hpp>
#include <drm-cxx/modeset/page_flip.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

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

enum class PresenterMode : std::uint8_t { Scene, Plane };

struct Args {
  std::uint32_t seconds{5};
  std::string theme_name{"default"};
  std::string png_path;
  PresenterMode presenter{PresenterMode::Scene};
};

// Locate the ordinal index of `crtc_id` within drmModeRes::crtcs[].
// PlaneRegistry::for_crtc and OverlayReservation::reserve both want
// the index, but most other drm-cxx APIs work with the KMS object id;
// the smoke needs both.
std::optional<std::uint32_t> crtc_index_for(int fd, std::uint32_t crtc_id) {
  auto* res = drmModeGetResources(fd);
  if (res == nullptr) {
    return std::nullopt;
  }
  std::optional<std::uint32_t> out;
  for (int i = 0; i < res->count_crtcs; ++i) {
    if (res->crtcs[i] == crtc_id) {
      out = static_cast<std::uint32_t>(i);
      break;
    }
  }
  drmModeFreeResources(res);
  return out;
}

// Highest zpos any PRIMARY plane on this CRTC reaches. Used as the
// floor for overlay stacking — base_zpos = primary_zpos_max + 1
// lands the decoration directly above the bg. Returns 0 when no
// primary advertises zpos (rare but possible on stripped-down
// drivers); the plane path falls back to "leave zpos alone" on
// those.
std::uint64_t primary_zpos_max(const drm::planes::PlaneRegistry& registry,
                               std::uint32_t crtc_index) {
  std::uint64_t out = 0;
  for (const auto* cap : registry.for_crtc(crtc_index)) {
    if (cap == nullptr || cap->type != drm::planes::DRMPlaneType::PRIMARY) {
      continue;
    }
    if (cap->zpos_max.has_value()) {
      out = std::max(out, *cap->zpos_max);
    }
  }
  return out;
}

// Parse and strip our flags from argv so the leftover positional (the
// device path) lands at argv[1] for select_device. Accepts both the
// space-separated (`--flag value`) and equals (`--flag=value`) forms;
// the equals form is what most users reach for first.
Args parse_args(int& argc, char* argv[]) {
  Args a;
  auto strip = [&](int i, int n) {
    for (int j = i; j + n < argc; ++j) {
      argv[j] = argv[j + n];
    }
    argc -= n;
  };

  // Returns the value for `--name`/`--name=value` and the number of
  // argv slots to strip (1 for `=`, 2 for space-separated). When the
  // flag matches but no value is available, returns nullopt + 0 so
  // the caller can fall through to the next branch.
  auto match = [&](std::string_view arg, std::string_view name,
                   int i) -> std::pair<std::optional<std::string_view>, int> {
    if (arg == name && i + 1 < argc) {
      return {std::string_view{argv[i + 1]}, 2};
    }
    if (arg.size() > name.size() + 1 && arg.substr(0, name.size()) == name &&
        arg[name.size()] == '=') {
      return {arg.substr(name.size() + 1), 1};
    }
    return {std::nullopt, 0};
  };

  for (int i = 1; i < argc;) {
    const std::string_view arg = argv[i];
    if (auto [v, n] = match(arg, "--seconds", i); v) {
      a.seconds = static_cast<std::uint32_t>(std::strtoul(std::string(*v).c_str(), nullptr, 10));
      strip(i, n);
    } else if (auto [v2, n2] = match(arg, "--theme", i); v2) {
      a.theme_name = std::string(*v2);
      strip(i, n2);
    } else if (auto [v3, n3] = match(arg, "--png", i); v3) {
      a.png_path = std::string(*v3);
      strip(i, n3);
    } else if (auto [v4, n4] = match(arg, "--presenter", i); v4) {
      a.presenter = (*v4 == "plane") ? PresenterMode::Plane : PresenterMode::Scene;
      strip(i, n4);
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
  //
  // Try GBM first so the smoke exercises the production allocator path
  // on hardware where GBM is available (any modern desktop / mobile
  // GPU). If GBM construction fails — vgem, virgl headless, distros
  // without a scanout-capable mesa — Surface::create silently falls
  // back to the dumb path, so this stays portable.
  std::optional<drm::gbm::GbmDevice> gbm;
  if (auto g = drm::gbm::GbmDevice::create(dev.fd()); g) {
    gbm.emplace(std::move(*g));
  } else {
    drm::println("csd_smoke: GbmDevice::create failed ({}); using dumb-only path",
                 g.error().message());
  }

  drm::csd::SurfaceConfig deco_cfg;
  deco_cfg.width = 800;
  deco_cfg.height = 200;
  auto surface_res = drm::csd::Surface::create(dev, gbm ? &*gbm : nullptr, deco_cfg);
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

  // ── Scene assembly ───────────────────────────────────────────
  // Bg goes through LayerScene in both modes (it owns the modeset
  // path). The deco follows LayerScene in scene mode and a separate
  // PlanePresenter commit in plane mode.
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

  // We need to keep the painted decoration buffer alive for the life
  // of the program. In scene mode the SurfaceLayerSource adopts it
  // via a unique_ptr the LayerDesc owns; in plane mode we hold the
  // Surface directly so PlanePresenter can read its fb_id().
  std::unique_ptr<SurfaceLayerSource> deco_layer_src;
  drm::csd::Surface deco_surface_local;

  if (args.presenter == PresenterMode::Scene) {
    deco_layer_src = std::make_unique<SurfaceLayerSource>(std::move(surface));
    drm::scene::LayerDesc deco_desc;
    deco_desc.source = std::move(deco_layer_src);
    deco_desc.display.src_rect = drm::scene::Rect{0, 0, deco_w, deco_h};
    deco_desc.display.dst_rect = drm::scene::Rect{deco_x, deco_y, deco_w, deco_h};
    // Force the decoration above the bg — amdgpu pins the primary at
    // zpos=2, so 3 lands on the first overlay above it.
    deco_desc.display.zpos = 3;
    if (auto r = scene->add_layer(std::move(deco_desc)); !r) {
      drm::println(stderr, "add_layer (deco): {}", r.error().message());
      return EXIT_FAILURE;
    }
  } else {
    deco_surface_local = std::move(surface);
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

  // ── Optional second commit: deco via PlanePresenter ──────────
  if (args.presenter == PresenterMode::Plane) {
    auto registry_res = drm::planes::PlaneRegistry::enumerate(dev);
    if (!registry_res) {
      drm::println(stderr, "PlaneRegistry::enumerate: {}", registry_res.error().message());
      return EXIT_FAILURE;
    }
    const auto registry = std::move(*registry_res);

    const auto crtc_idx = crtc_index_for(dev.fd(), output->crtc_id);
    if (!crtc_idx) {
      drm::println(stderr, "crtc_index_for({}): not found", output->crtc_id);
      return EXIT_FAILURE;
    }

    auto reservation_res = drm::csd::OverlayReservation::create(registry);
    if (!reservation_res) {
      drm::println(stderr, "OverlayReservation::create: {}", reservation_res.error().message());
      return EXIT_FAILURE;
    }
    auto reservation = std::move(*reservation_res);

    // One overlay for one decoration. min_zpos=0 because amdgpu (and
    // most modern drivers) report mutable zpos on overlays — the
    // PlanePresenter's base_zpos write is what actually places the
    // overlay above primary, not the reservation filter. Hardware
    // that reports zpos_min == zpos_max (immutable, fixed-stack
    // overlays) would need a real min_zpos here.
    auto reserved_res = reservation.reserve(*crtc_idx, DRM_FORMAT_ARGB8888, /*count=*/1);
    if (!reserved_res) {
      drm::println(stderr, "OverlayReservation::reserve: {} (no compatible overlay free)",
                   reserved_res.error().message());
      return EXIT_FAILURE;
    }
    const auto reserved = std::move(*reserved_res);
    drm::println("csd_smoke: reserved overlay {} for decoration", reserved[0]);

    const std::uint64_t base_zpos = primary_zpos_max(registry, *crtc_idx) + 1U;
    auto presenter_res = drm::csd::PlanePresenter::create(
        dev, registry, output->crtc_id,
        drm::span<const std::uint32_t>(reserved.data(), reserved.size()), base_zpos);
    if (!presenter_res) {
      drm::println(stderr, "PlanePresenter::create: {}", presenter_res.error().message());
      return EXIT_FAILURE;
    }
    const auto presenter = std::move(*presenter_res);
    drm::println("csd_smoke: PlanePresenter armed (base_zpos={})", base_zpos);

    drm::AtomicRequest req(dev);
    if (!req.valid()) {
      drm::println(stderr, "AtomicRequest::ctor failed");
      return EXIT_FAILURE;
    }

    const drm::csd::SurfaceRef ref{&deco_surface_local, deco_x, deco_y};
    const drm::span<const drm::csd::SurfaceRef> surfaces(&ref, 1);
    if (auto r = presenter->apply(surfaces, req); !r) {
      drm::println(stderr, "PlanePresenter::apply: {}", r.error().message());
      return EXIT_FAILURE;
    }
    if (auto r = req.test(); !r) {
      drm::println(stderr, "AtomicRequest::test (plane presenter): {}", r.error().message());
      return EXIT_FAILURE;
    }
    flipped = false;
    if (auto r = req.commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
      drm::println(stderr, "AtomicRequest::commit (plane presenter): {}", r.error().message());
      return EXIT_FAILURE;
    }
    while (!flipped) {
      if (auto r = page_flip.dispatch(-1); !r) {
        drm::println(stderr, "page_flip.dispatch (plane presenter): {}", r.error().message());
        return EXIT_FAILURE;
      }
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
