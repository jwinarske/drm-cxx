// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// video_player — minimal demo of drm::scene::GstAppsinkSource. Plays
// one GStreamer pipeline's appsink output to the first connected
// display until end-of-stream, Esc/Q/Ctrl+C, or a fatal pipeline error.
//
// Pipeline selection (all forms must terminate in `appsink name=sink`):
//
//   * default — `videotestsrc is-live=true ! video/x-raw,format=BGRx
//                ! appsink name=sink` (works without any media file).
//   * `--file PATH` — convenience wrapper for
//     `filesrc location=PATH ! decodebin3 ! videoconvert ! videoscale
//      ! video/x-raw,format=BGRx ! appsink name=sink`. Uses GStreamer's
//     decodebin so any installed decoder set works (sw or hw).
//   * `--launch "<full pipeline string>"` — caller-supplied launch
//     string. Must end in `appsink name=sink` so the sink element can
//     be retrieved by name.
//
// Quit path is the libinput keyboard (Esc / Q / Ctrl+C). When libseat
// is in use it puts the TTY into KD_GRAPHICS, where the kernel
// suppresses Ctrl-C signal generation, so std::signal(SIGINT) alone is
// not a reliable exit path on a bare VT. SIGINT/SIGTERM are still wired
// as a fallback for runs without libseat (or where the keyboard isn't
// available).
//
// libseat session pause/resume on VT switch is handled: on pause the
// scene drops its imported FBs and the pipeline transitions to PAUSED;
// on resume the scene re-binds to the fresh DRM fd, the pipeline
// returns to PLAYING, and decoded samples are re-imported against the
// new fd. Ctrl+Alt+F<n> chords are forwarded to libseat via the shared
// VtChordTracker helper.
//
// Out of scope (intentional simplifications — see signage_player for
// orthogonal demonstrations):
//   * Hotplug-follow / dynamic mode change.
//
// Build with `-DDRM_CXX_GSTREAMER=ON` (CMake) or `-Dgstreamer=enabled`
// (Meson). The example is gated on the same flag, so a build without
// GStreamer simply doesn't build this binary.

#include "../../common/event_loop.hpp"
#include "../../common/open_output.hpp"
#include "../../common/session_pump.hpp"
#include "../../common/vt_switch.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/input/seat.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/scene/gst_appsink_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/session/seat.hpp>

#include <drm_mode.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <gst/gst.h>  // gst_init only
#include <gst/gstbin.h>
#include <gst/gstelement.h>
#include <gst/gstobject.h>
#include <gst/gstparse.h>
#include <memory>
#include <string>
#include <sys/poll.h>
#include <system_error>
#include <utility>
#include <variant>

namespace {

// SIGINT / SIGTERM toggle this flag to break the main loop. Marked
// volatile-equivalent via std::atomic; mutated from the signal handler
// only. Only effective on bare-fd runs without libseat — when libseat
// has the TTY in KD_GRAPHICS the kernel won't deliver Ctrl-C as SIGINT,
// so the libinput keyboard is the actual quit path on a real VT.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_quit{false};

extern "C" void on_sigint(int /*sig*/) {
  g_quit.store(true, std::memory_order_relaxed);
}

void install_signal_handler() {
  std::signal(SIGINT, on_sigint);
  std::signal(SIGTERM, on_sigint);
}

struct Args {
  std::string file_path;
  std::string custom_launch;
  bool show_help{false};
};

Args parse_args(int argc, char* argv[]) {
  Args out;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      out.show_help = true;
    } else if (a == "--file" && i + 1 < argc) {
      out.file_path = argv[++i];
    } else if (a == "--launch" && i + 1 < argc) {
      out.custom_launch = argv[++i];
    }
  }
  return out;
}

void print_help() {
  drm::println("video_player — drm-cxx GstAppsinkSource demo");
  drm::println("");
  drm::println("Usage:");
  drm::println("  video_player [/dev/dri/cardN] [--file PATH | --launch PIPELINE]");
  drm::println("");
  drm::println("Without --file or --launch, plays a videotestsrc pattern.");
  drm::println("Quit with Esc, Q, or Ctrl-C.");
}

std::string build_pipeline_string(const Args& args, std::uint32_t width, std::uint32_t height) {
  if (!args.custom_launch.empty()) {
    return args.custom_launch;
  }
  if (!args.file_path.empty()) {
    return drm::format(
        "filesrc location={} ! decodebin3 ! videoconvert ! videoscale ! "
        "video/x-raw,format=BGRx,width={},height={} ! appsink name=sink",
        args.file_path, width, height);
  }
  return drm::format(
      "videotestsrc is-live=true ! video/x-raw,format=BGRx,width={},height={} "
      "! appsink name=sink",
      width, height);
}

}  // namespace

int main(int argc, char* argv[]) try {
  const Args args = parse_args(argc, argv);
  if (args.show_help) {
    print_help();
    return EXIT_SUCCESS;
  }

  install_signal_handler();
  gst_init(&argc, &argv);

  auto output_opt = drm::examples::open_and_pick_output(argc, argv);
  if (!output_opt) {
    return EXIT_FAILURE;
  }
  auto& output = *output_opt;
  auto& dev = output.device;
  auto& seat = output.seat;
  const auto fb_w = output.mode.hdisplay;
  const auto fb_h = output.mode.vdisplay;
  drm::println("Display: {}x{}@{}Hz on connector={} crtc={}", fb_w, fb_h, output.mode.vrefresh,
               output.connector_id, output.crtc_id);

  // Build the GStreamer pipeline at the display's resolution. This
  // forces videoscale to do the scaling once on the producer side
  // rather than asking the KMS scaler (which may not exist).
  const std::string desc = build_pipeline_string(args, fb_w, fb_h);
  drm::println("Pipeline: {}", desc);
  // Pass nullptr for the GError out-param: GError lives in <glib.h>,
  // an umbrella the include-cleaner lint can't trace through; the
  // pipeline string is logged above so the diagnostic surface is
  // already adequate without GLib's structured error pickup.
  GstElement* pipeline = gst_parse_launch(desc.c_str(), nullptr);
  if (pipeline == nullptr) {
    drm::println(stderr, "gst_parse_launch failed for the pipeline above");
    return EXIT_FAILURE;
  }
  // RAII the pipeline ref so every error path on the way to the main
  // loop unwinds it without a manual unref at each return site.
  auto pipeline_holder =
      std::unique_ptr<GstElement, decltype(&gst_object_unref)>(pipeline, gst_object_unref);

  GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
  if (sink == nullptr) {
    drm::println(stderr, "Pipeline does not contain `appsink name=sink`");
    return EXIT_FAILURE;
  }
  // gst_bin_get_by_name returned a fresh ref; the source takes its own.
  auto sink_holder =
      std::unique_ptr<GstElement, decltype(&gst_object_unref)>(sink, gst_object_unref);

  // Build the scene with one full-screen layer fed by the GStreamer
  // appsink source.
  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = output.crtc_id;
  cfg.connector_id = output.connector_id;
  cfg.mode = output.mode;
  auto scene_r = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_r) {
    drm::println(stderr, "LayerScene::create: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_r);

  auto src_r = drm::scene::GstAppsinkSource::create(dev, sink_holder.get(), {});
  if (!src_r) {
    drm::println(stderr, "GstAppsinkSource::create: {}", src_r.error().message());
    return EXIT_FAILURE;
  }

  drm::scene::LayerDesc desc_layer;
  desc_layer.source = std::move(*src_r);
  desc_layer.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  desc_layer.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  desc_layer.content_type = drm::planes::ContentType::Video;
  auto layer_r = scene->add_layer(std::move(desc_layer));
  if (!layer_r) {
    drm::println(stderr, "add_layer: {}", layer_r.error().message());
    return EXIT_FAILURE;
  }
  // Hold a non-owning pointer to the source so we can drive() its bus.
  // LayerScene owns the LayerBufferSource via unique_ptr; the layer
  // accessor returns a reference to it, which we downcast back to the
  // concrete GstAppsinkSource type. The downcast is safe by
  // construction — we just constructed a GstAppsinkSource into this
  // layer's source slot one statement above.
  auto* video_source =
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
      static_cast<drm::scene::GstAppsinkSource*>(&scene->get_layer(*layer_r)->source());

  // libinput-backed keyboard. The libseat-managed TTY runs in
  // KD_GRAPHICS where Ctrl-C is no longer translated to SIGINT, so this
  // is the only reliable in-app quit path on a real VT. Routes through
  // the seat's privileged opener when libseat is available, so input
  // fds get the same revocable / resume-aware lifetime as the DRM fd.
  drm::input::InputDeviceOpener libinput_opener;
  if (seat) {
    libinput_opener = seat->input_opener();
  }
  auto input_seat_res = drm::input::Seat::open({}, std::move(libinput_opener));
  if (!input_seat_res) {
    drm::println(stderr, "drm::input::Seat::open: {} (need root or 'input' group membership)",
                 input_seat_res.error().message());
    return EXIT_FAILURE;
  }
  auto& input_seat = *input_seat_res;
  bool quit = false;
  drm::examples::VtChordTracker vt_chord;
  input_seat.set_event_handler([&](const drm::input::InputEvent& event) {
    const auto* ke = std::get_if<drm::input::KeyboardEvent>(&event);
    if (ke == nullptr) {
      return;
    }
    if (vt_chord.observe(*ke, seat ? &*seat : nullptr)) {
      return;
    }
    if (vt_chord.is_quit_key(*ke)) {
      quit = true;
    }
  });

  // Session pause/resume bookkeeping. wire_session_callbacks installs
  // the standard pause (drop scene FBs + suspend libinput + clear
  // session_state.flip_pending) and resume (capture new fd into
  // pending_resume_fd after the /dev/dri/ filter) handlers; the
  // gst pipeline pause hook below runs at the end of the pause cb,
  // before the kernel revokes the fd. PAUSED keeps the negotiated
  // caps + element state so the resume transition back to PLAYING
  // is fast. The fd swap + on_session_resumed land later in
  // apply_pending_resume.
  drm::examples::SessionPumpState session_state;
  if (seat) {
    drm::examples::wire_session_callbacks(*seat, *scene, session_state, &input_seat, [&] {
      gst_element_set_state(pipeline, GST_STATE_PAUSED);
    });
  }

  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    drm::println(stderr, "Failed to set pipeline to PLAYING");
    return EXIT_FAILURE;
  }

  // Wait for the first decoded sample before committing. Unlike
  // signage_player, this scene has no always-ready background layer:
  // the GStreamer-fed video is the *only* layer, so a commit issued
  // before the pipeline has produced anything would arm no plane at
  // all. The kernel modesets the CRTC but leaves PRIMARY blank, no
  // PAGE_FLIP_EVENT lands, the scanout still shows fbcon, and the
  // main loop hangs waiting for a flip that will never come. Pre-
  // pulling the first sample populates the source's cached fb_id so
  // the scene's own acquire() during commit sees a real frame. The
  // preroll polls the libinput fd so Esc/Q still quit while the
  // pipeline is still warming up.
  {
    constexpr std::chrono::seconds k_preroll_timeout{5};
    const auto deadline = std::chrono::steady_clock::now() + k_preroll_timeout;
    bool got_first = false;
    pollfd preroll_pfd{};
    preroll_pfd.fd = input_seat.fd();
    preroll_pfd.events = POLLIN;
    while (!quit && !g_quit.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < deadline) {
      if (auto r = video_source->drive(); !r) {
        drm::println(stderr, "preroll drive(): {}", r.error().message());
        gst_element_set_state(pipeline, GST_STATE_NULL);
        return EXIT_FAILURE;
      }
      auto acq = video_source->acquire();
      if (acq.has_value()) {
        video_source->release(std::move(*acq));
        got_first = true;
        break;
      }
      if (acq.error() != std::make_error_code(std::errc::resource_unavailable_try_again)) {
        drm::println(stderr, "preroll acquire(): {}", acq.error().message());
        gst_element_set_state(pipeline, GST_STATE_NULL);
        return EXIT_FAILURE;
      }
      // ~5 ms between attempts so we don't busy-spin while the
      // streaming thread completes its first decode. Pump the keyboard
      // on each tick so a press still terminates the wait.
      if (::poll(&preroll_pfd, 1, 5) > 0 && (preroll_pfd.revents & POLLIN) != 0) {
        (void)input_seat.dispatch();
      }
    }
    if (!got_first) {
      if (!quit && !g_quit.load(std::memory_order_relaxed)) {
        drm::println(stderr, "preroll: pipeline produced no sample within {}s",
                     k_preroll_timeout.count());
      }
      gst_element_set_state(pipeline, GST_STATE_NULL);
      return (quit || g_quit.load(std::memory_order_relaxed)) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
  }

  // Page-flip dispatch: the scene's commit path uses
  // DRM_MODE_PAGE_FLIP_EVENT, which fires once the kernel has scanned
  // out the committed FB. session_state.flip_pending gates the next
  // commit. The PageFlip object captures the device fd at construction;
  // on session resume it must be rebuilt against the new fd, hence the
  // factory lambda.
  auto make_page_flip = [&]() {
    drm::PageFlip pf(dev);
    pf.set_handler([&](std::uint32_t /*c*/, std::uint64_t /*s*/, std::uint64_t /*t*/) {
      session_state.flip_pending = false;
    });
    return pf;
  };
  drm::PageFlip page_flip = make_page_flip();

  if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    drm::println(stderr, "first commit: {}", r.error().message());
    gst_element_set_state(pipeline, GST_STATE_NULL);
    return EXIT_FAILURE;
  }
  session_state.flip_pending = true;

  drm::println("Playing — Esc/Q/Ctrl-C to quit.");

  drm::examples::EventLoop loop;
  (void)loop.add_slot(input_seat.fd(), [&] { (void)input_seat.dispatch(); });
  int const drm_slot = loop.add_slot(dev.fd(), [&] { (void)page_flip.dispatch(0); });
  (void)loop.add_slot(seat ? seat->poll_fd() : -1, [&] {
    if (seat) {
      seat->dispatch();
    }
  });

  // Becomes true on a genuine error (commit, drive, poll, resume-step
  // failure). Stays false for the clean exits (user quit, EOS, signal),
  // so the process returns the right code to systemd / shell scripts.
  bool error_exit = false;
  while (!quit && !g_quit.load(std::memory_order_relaxed)) {
    // Drive bus first so EOS / errors surface before the next commit
    // wastes a vblank. Skipped while paused — the pipeline is in
    // PAUSED, the appsink isn't producing, and the bus pop is a
    // no-op anyway.
    if (!session_state.paused) {
      if (auto r = video_source->drive(); !r) {
        const auto ec = r.error();
        if (ec == std::make_error_code(std::errc::no_message_available)) {
          drm::println("EOS — exiting.");
          break;
        }
        drm::println(stderr, "drive(): {}", ec.message());
        error_exit = true;
        break;
      }
    }

    // While paused, block indefinitely so the CPU is idle until libseat
    // wakes us with a resume. Otherwise, 16ms ≈ one 60Hz frame so we
    // don't busy-loop when no new sample is ready.
    int timeout_ms = 0;
    if (session_state.paused) {
      timeout_ms = -1;
    } else if (session_state.flip_pending) {
      timeout_ms = 16;
    }
    if (!loop.tick(timeout_ms)) {
      error_exit = true;
      break;
    }

    auto resumed = drm::examples::apply_pending_resume(session_state, dev, *scene);
    if (!resumed) {
      error_exit = true;
      break;
    }
    if (*resumed) {
      loop.set_fd(drm_slot, dev.fd());
      // PageFlip captured the dead fd at construction; rebuild against
      // the new device. Move-assign destroys the old PageFlip (its
      // userspace epfd closes; the dead drm fd was already handled by
      // Device's owns_fd contract above).
      page_flip = make_page_flip();
      gst_element_set_state(pipeline, GST_STATE_PLAYING);
      session_state.flip_pending = false;
    }

    if (session_state.flip_pending || session_state.paused) {
      continue;
    }

    if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, &page_flip);
        !r) {
      // EACCES is the timely "you've lost master" signal — libseat's
      // pause callback may not have arrived yet (drmIsMaster lags the
      // actual revocation). Treat it as a soft pause and wait for the
      // explicit resume to land in pending_resume_fd.
      if (r.error() == std::errc::permission_denied) {
        session_state.paused = true;
        session_state.flip_pending = false;
        continue;
      }
      // EAGAIN from the allocator's warm-start TEST_ONLY commit: the
      // previous frame's plane assignment no longer applies. Clear the
      // flip state and let the next iteration go through full search.
      // Not a soft-pause — there's no resume_cb coming to clear it.
      if (r.error() == std::errc::resource_unavailable_try_again) {
        session_state.flip_pending = false;
        continue;
      }
      drm::println(stderr, "commit: {}", r.error().message());
      error_exit = true;
      break;
    }
    session_state.flip_pending = true;
  }

  gst_element_set_state(pipeline, GST_STATE_NULL);
  // The scene (and its layers) must be destroyed before the pipeline:
  // GstAppsinkSource holds a ref on the appsink which lives in the
  // pipeline bin, and scene destruction unref's the appsink as part
  // of the source's dtor.
  scene.reset();
  return error_exit ? EXIT_FAILURE : EXIT_SUCCESS;
} catch (...) {
  return EXIT_FAILURE;
}
