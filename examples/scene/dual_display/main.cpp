// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// dual_display — minimal multi-CRTC SceneSet demo.
//
// Drives every connected output on one card with its own LayerScene,
// then lays one shared layer across all of them via SceneSet::add_layer.
// Every frame is one drmModeAtomicCommit covering N CRTCs, so the
// scanning bar in the mirrored tile lands on the same frame across
// every connected output.
//
// What it demonstrates:
//
//   * One full-screen background DumbBufferSource per output, each
//     filled with a different solid tint — per-output specialization.
//   * One shared DumbBufferSource carried across every scene via
//     SceneSet::add_layer, centered on each output, with a horizontal
//     scan bar that advances one frame at a time — mirrored content.
//   * Combined cross-CRTC atomic commits through SceneSet::commit.
//
// Hardware requirements:
//
//   * A card with at least two connected outputs. On a single-output
//     workstation, provision vkms with two virtual connectors first:
//
//       sudo scripts/vkms_dual.sh up
//       ./dual_display /dev/dri/cardN     # N = the vkms node
//
// Keys:
//
//   * Esc / q / Ctrl+C — quit
//   * Ctrl+Alt+F<n>    — VT switch (libseat-managed)

#include "../../common/event_loop.hpp"
#include "../../common/multi_crtc_probe.hpp"
#include "../../common/open_output.hpp"
#include "../../common/vt_switch.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/input/seat.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/scene/scene_set.hpp>
#include <drm-cxx/session/seat.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr std::uint32_t k_mirror_side = 320U;

// Per-output background tints so the two displays are visually
// distinct. More than four outputs cycle through this palette — the
// interesting comparison is the first two, so a repeat further down
// the list is harmless.
constexpr std::array<std::uint32_t, 4> k_bg_tints{
    0xFF1A237EU,  // deep indigo
    0xFF1B5E20U,  // deep green
    0xFF4A148CU,  // deep purple
    0xFF263238U,  // dark slate
};

void fill_solid_xrgb(drm::BufferMapping const& map, std::uint32_t color_xrgb) noexcept {
  const auto width = map.width();
  const auto height = map.height();
  const auto stride = map.stride();
  const auto pixels = map.pixels();
  if (width == 0U || height == 0U || stride < width * 4U) {
    return;
  }
  if (pixels.size() < static_cast<std::size_t>(height) * stride) {
    return;
  }
  for (std::uint32_t y = 0; y < height; ++y) {
    auto* row =
        reinterpret_cast<std::uint32_t*>(pixels.data() + (static_cast<std::size_t>(y) * stride));
    std::fill_n(row, width, color_xrgb);
  }
}

// Mirrored tile: white frame, semi-transparent cyan body so the
// per-output background tints show through, and one bright bar that
// advances every frame. The scan position is the only frame-to-frame
// change, which keeps the dirty surface small while making cross-CRTC
// synchronization visually unambiguous.
void paint_mirror_argb(drm::BufferMapping const& map, std::uint32_t scan_x) noexcept {
  const auto width = map.width();
  const auto height = map.height();
  const auto stride = map.stride();
  const auto pixels = map.pixels();
  if (width == 0U || height == 0U || stride < width * 4U) {
    return;
  }
  if (pixels.size() < static_cast<std::size_t>(height) * stride) {
    return;
  }

  constexpr std::uint32_t k_body = 0xCC00BCD4U;    // 80% cyan
  constexpr std::uint32_t k_border = 0xFFFFFFFFU;  // opaque white
  constexpr std::uint32_t k_bar = 0xFFFF5252U;     // opaque red
  constexpr std::uint32_t k_border_px = 4U;
  constexpr std::uint32_t k_bar_w = 6U;

  const std::uint32_t bar_x = scan_x % width;
  for (std::uint32_t y = 0; y < height; ++y) {
    auto* row =
        reinterpret_cast<std::uint32_t*>(pixels.data() + (static_cast<std::size_t>(y) * stride));
    const bool border_row = y < k_border_px || y + k_border_px >= height;
    for (std::uint32_t x = 0; x < width; ++x) {
      if (border_row || x < k_border_px || x + k_border_px >= width) {
        row[x] = k_border;
      } else if (x >= bar_x && x < bar_x + k_bar_w) {
        row[x] = k_bar;
      } else {
        row[x] = k_body;
      }
    }
  }
}

// Per-output bookkeeping that survives after the LayerScene unique_ptrs
// are handed off to SceneSet. The raw `bg` pointer stays valid because
// DumbBufferSource is heap-allocated inside the LayerScene and not moved
// when ownership transfers.
struct OutputView {
  drm::examples::multi_crtc::ConnectedOutput info;
  drm::scene::DumbBufferSource* bg{nullptr};
  drm::scene::LayerScene* scene{nullptr};
};

drm::expected<std::unique_ptr<drm::scene::LayerScene>, std::error_code> build_scene_with_bg(
    drm::Device& dev, const drm::examples::multi_crtc::ConnectedOutput& out,
    std::uint32_t tint_xrgb, drm::scene::DumbBufferSource** bg_out) {
  auto bg_src = drm::scene::DumbBufferSource::create(dev, out.mode.hdisplay, out.mode.vdisplay,
                                                     DRM_FORMAT_XRGB8888);
  if (!bg_src) {
    return drm::unexpected<std::error_code>(bg_src.error());
  }
  // Paint the background once. It never changes after this, so the
  // dirty-tracking path emits FB_ID exactly once per session — every
  // subsequent commit re-uses the cached property snapshot.
  if (auto m = (*bg_src)->map(drm::MapAccess::Write); m) {
    fill_solid_xrgb(*m, tint_xrgb);
  }
  *bg_out = bg_src->get();

  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = out.crtc_id;
  cfg.connector_id = out.connector_id;
  cfg.mode = out.mode;
  auto scene_r = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_r) {
    return drm::unexpected<std::error_code>(scene_r.error());
  }
  auto scene = std::move(*scene_r);

  drm::scene::LayerDesc bg_desc;
  bg_desc.source = std::move(*bg_src);
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, out.mode.hdisplay, out.mode.vdisplay};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, out.mode.hdisplay, out.mode.vdisplay};
  bg_desc.content_type = drm::planes::ContentType::Generic;
  auto h = scene->add_layer(std::move(bg_desc));
  if (!h) {
    return drm::unexpected<std::error_code>(h.error());
  }
  return scene;
}

}  // namespace

int main(int argc, char** argv) try {
  auto ctx = drm::examples::open_device(argc, argv);
  if (!ctx) {
    return EXIT_FAILURE;
  }
  auto& dev = ctx->device;
  auto& seat = ctx->seat;

  auto outputs = drm::examples::multi_crtc::enumerate_connected_outputs(dev);
  if (outputs.size() < 2) {
    drm::println(stderr,
                 "dual_display needs >=2 connected outputs (found {}). "
                 "Try `sudo scripts/vkms_dual.sh up` and pass the resulting vkms cardN.",
                 outputs.size());
    return EXIT_FAILURE;
  }
  drm::println("Driving {} connected output(s):", outputs.size());
  for (const auto& o : outputs) {
    drm::println("  {} @ CRTC {} mode {}x{}@{}Hz", o.connector_name, o.crtc_id, o.mode.hdisplay,
                 o.mode.vdisplay, o.mode.vrefresh);
  }

  // Up-front kernel feasibility probe. If the combined TEST is
  // rejected, SceneSet::commit will fail too — catch it here with a
  // useful diagnostic before any allocations land.
  const auto probe = drm::examples::multi_crtc::probe_combined_atomic(dev, outputs);
  if (probe.verdict == drm::examples::multi_crtc::CombinedAtomicVerdict::Rejected) {
    drm::println(stderr, "Combined cross-CRTC atomic TEST rejected: {}", probe.error.message());
    return EXIT_FAILURE;
  }

  std::vector<std::unique_ptr<drm::scene::LayerScene>> scenes_owner;
  std::vector<OutputView> views;
  scenes_owner.reserve(outputs.size());
  views.reserve(outputs.size());
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    drm::scene::DumbBufferSource* bg_ptr = nullptr;
    auto scene_r =
        build_scene_with_bg(dev, outputs[i], k_bg_tints.at(i % k_bg_tints.size()), &bg_ptr);
    if (!scene_r) {
      drm::println(stderr, "scene[{}] ({}) build failed: {}", i, outputs[i].connector_name,
                   scene_r.error().message());
      return EXIT_FAILURE;
    }
    views.push_back(OutputView{outputs[i], bg_ptr, scene_r->get()});
    scenes_owner.push_back(std::move(*scene_r));
  }

  auto set_r = drm::scene::SceneSet::create(dev, std::move(scenes_owner));
  if (!set_r) {
    drm::println(stderr, "SceneSet::create: {}", set_r.error().message());
    return EXIT_FAILURE;
  }
  auto& scene_set = **set_r;

  // Shared mirrored layer: one DumbBufferSource, one SceneSet::add_layer
  // call that fans it out to every child scene with a per-output dst rect.
  auto mirror_src_r =
      drm::scene::DumbBufferSource::create(dev, k_mirror_side, k_mirror_side, DRM_FORMAT_ARGB8888);
  if (!mirror_src_r) {
    drm::println(stderr, "mirror DumbBufferSource: {}", mirror_src_r.error().message());
    return EXIT_FAILURE;
  }
  auto* mirror_src = mirror_src_r->get();
  const std::shared_ptr<drm::scene::LayerBufferSource> mirror_shared(std::move(*mirror_src_r));

  drm::scene::SceneSetLayerSpec mirror_spec;
  mirror_spec.source = mirror_shared;
  mirror_spec.targets.reserve(views.size());
  for (std::size_t i = 0; i < views.size(); ++i) {
    const auto& o = views[i].info;
    const std::uint32_t cx =
        (o.mode.hdisplay > k_mirror_side) ? (o.mode.hdisplay - k_mirror_side) / 2U : 0U;
    const std::uint32_t cy =
        (o.mode.vdisplay > k_mirror_side) ? (o.mode.vdisplay - k_mirror_side) / 2U : 0U;
    drm::scene::DisplayParams disp;
    disp.src_rect = drm::scene::Rect{0, 0, k_mirror_side, k_mirror_side};
    disp.dst_rect = drm::scene::Rect{static_cast<std::int32_t>(cx), static_cast<std::int32_t>(cy),
                                     k_mirror_side, k_mirror_side};
    // Sit above the background regardless of driver. amdgpu pins
    // PRIMARY at zpos=2, so 5 stays clear of that.
    disp.zpos = 5;
    mirror_spec.targets.push_back({.scene_index = i, .display = disp, .force_composited = false});
  }
  auto mirror_handle_r = scene_set.add_layer(mirror_spec);
  if (!mirror_handle_r) {
    drm::println(stderr, "SceneSet::add_layer: {}", mirror_handle_r.error().message());
    return EXIT_FAILURE;
  }

  // Page-flips: every combined commit yields one event per CRTC. We
  // track an outstanding count so the next frame doesn't issue until
  // every child has flipped.
  drm::PageFlip page_flip(dev);
  std::size_t flips_outstanding = 0;
  page_flip.set_handler([&](std::uint32_t /*crtc*/, std::uint64_t /*seq*/, std::uint64_t /*ts*/) {
    if (flips_outstanding > 0) {
      --flips_outstanding;
    }
  });

  // libinput for the quit chord + libseat-driven VT-switch.
  drm::input::InputDeviceOpener libinput_opener;
  if (seat) {
    libinput_opener = seat->input_opener();
  }
  auto input_seat_r = drm::input::Seat::open({}, std::move(libinput_opener));
  if (!input_seat_r) {
    drm::println(stderr, "Failed to open input seat: {}", input_seat_r.error().message());
    return EXIT_FAILURE;
  }
  auto& input_seat = *input_seat_r;
  bool quit = false;
  drm::examples::VtChordTracker vt_chord;
  input_seat.set_event_handler([&](const drm::input::InputEvent& event) {
    if (const auto* ke = std::get_if<drm::input::KeyboardEvent>(&event)) {
      if (vt_chord.observe(*ke, seat ? &*seat : nullptr)) {
        return;
      }
      if (vt_chord.is_quit_key(*ke)) {
        quit = true;
      }
    }
  });

  // Session pause/resume across N scenes. SceneSet has no session API
  // of its own yet (Phase 8.2.3); fan the existing per-scene contract
  // out manually so a VT-switch round-trip leaves a usable demo.
  bool session_paused = false;
  int pending_resume_fd = -1;
  if (seat) {
    seat->set_pause_callback([&]() {
      session_paused = true;
      flips_outstanding = 0;  // queued flips won't fire after fd revoke
      for (auto& v : views) {
        v.scene->on_session_paused();
      }
      (void)input_seat.suspend();
    });
    seat->set_resume_callback([&](std::string_view path, int new_fd) {
      // libseat fires resume_cb once per managed device; only the DRM
      // node carries the fd we need to swap into drm::Device.
      if (path.substr(0, 9) != "/dev/dri/") {
        return;
      }
      pending_resume_fd = new_fd;
      session_paused = false;
      (void)input_seat.resume();
    });
  }

  auto repaint_bg = [&]() {
    for (std::size_t i = 0; i < views.size(); ++i) {
      if (auto m = views[i].bg->map(drm::MapAccess::Write); m) {
        fill_solid_xrgb(*m, k_bg_tints.at(i % k_bg_tints.size()));
      }
    }
  };

  std::uint32_t scan_x = 0;
  auto repaint_mirror = [&]() {
    if (auto m = mirror_src->map(drm::MapAccess::Write); m) {
      paint_mirror_argb(*m, scan_x);
    }
  };
  repaint_mirror();

  // First commit. The kernel implicitly adds ALLOW_MODESET on each
  // scene's first commit (every scene here is on its first), so
  // ALLOW_MODESET ends up on the combined commit too.
  if (auto r = scene_set.commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    drm::println(stderr, "first SceneSet::commit: {}", r.error().message());
    return EXIT_FAILURE;
  }
  flips_outstanding = scene_set.scene_count();

  drm::println("Running — Esc/q to quit.");

  drm::examples::EventLoop loop;
  (void)loop.add_slot(input_seat.fd(), [&] { (void)input_seat.dispatch(); });
  int const drm_slot = loop.add_slot(dev.fd(), [&] { (void)page_flip.dispatch(0); });
  (void)loop.add_slot(seat ? seat->poll_fd() : -1, [&] {
    if (seat) {
      seat->dispatch();
    }
  });

  while (!quit) {
    int timeout = 0;
    if (session_paused) {
      timeout = -1;
    } else if (flips_outstanding > 0) {
      timeout = 16;
    }
    if (!loop.tick(timeout)) {
      break;
    }

    if (pending_resume_fd >= 0) {
      const int new_fd = pending_resume_fd;
      pending_resume_fd = -1;
      dev = drm::Device::from_fd(new_fd);
      if (auto r = dev.enable_universal_planes(); !r) {
        drm::println(stderr, "resume: enable_universal_planes: {}", r.error().message());
        break;
      }
      if (auto r = dev.enable_atomic(); !r) {
        drm::println(stderr, "resume: enable_atomic: {}", r.error().message());
        break;
      }
      bool ok = true;
      for (auto& v : views) {
        if (auto r = v.scene->on_session_resumed(dev); !r) {
          drm::println(stderr, "resume: scene[{}] ({}) on_session_resumed: {}",
                       static_cast<std::size_t>(&v - views.data()), v.info.connector_name,
                       r.error().message());
          ok = false;
          break;
        }
      }
      if (!ok) {
        break;
      }
      // Backing buffers were unmapped during pause; re-paint everything
      // before the next commit so the first post-resume frame is correct.
      repaint_bg();
      repaint_mirror();
      loop.set_fd(drm_slot, dev.fd());
      page_flip = drm::PageFlip(dev);
      page_flip.set_handler([&](std::uint32_t, std::uint64_t, std::uint64_t) {
        if (flips_outstanding > 0) {
          --flips_outstanding;
        }
      });
    }

    if (session_paused || flips_outstanding > 0) {
      continue;
    }

    ++scan_x;
    repaint_mirror();

    const std::uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
    if (auto r = scene_set.commit(flags, &page_flip); !r) {
      if (r.error() == std::errc::permission_denied) {
        // libseat's pause callback usually fires before EACCES, but
        // drmIsMaster lags — treat EACCES as the timely pause signal.
        session_paused = true;
        flips_outstanding = 0;
        continue;
      }
      drm::println(stderr, "SceneSet::commit: {}", r.error().message());
      break;
    }
    flips_outstanding = scene_set.scene_count();
  }

  return EXIT_SUCCESS;
} catch (...) {
  return EXIT_FAILURE;
}