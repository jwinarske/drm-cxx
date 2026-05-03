// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// capture_demo — libinput-driven hardware validation harness for
// drm::capture. Mirrors cursor_rotate's shape: take a DRM device,
// enable atomic, bind libinput for keys, run an event loop, and on
// a keypress snapshot the current CRTC composition to a PNG.
//
// Usage: capture_demo [--out DIR] [--crtc ID] [/dev/dri/cardN]
//
//   --out DIR     Directory for captured PNGs (default: current dir).
//   --crtc ID     Capture a specific CRTC instead of auto-picking the
//                 first connected one. Useful on multi-head boxes when
//                 the interesting content isn't on CRTC 0.
//   /dev/dri/cardN  DRM device; select_device prompts if omitted.
//
// Key bindings in the event loop:
//   C, SPACE      Snapshot the active CRTC → capture_<ts>.png in --out.
//   R             Re-report CRTC state + count of bound planes.
//   ESC, Q        Quit.
//
// What this validates end-to-end on real hardware:
//
//   1. Atomic cap → drmModeObjectGetProperties exposes CRTC_X/Y/W/H
//      on every plane. snapshot() fails fast if it can't read those
//      for any plane on the target CRTC.
//   2. drmModeGetFB2 + drmPrimeHandleToFD + mmap(PROT_READ) round-trip
//      for each bound plane. Planes with non-linear modifiers or
//      non-ARGB/XRGB formats log a warning and get skipped — on most
//      systems the primary plane is linear ARGB8888 and the output PNG
//      matches the screen pixel-for-pixel.
//   3. Blend2D composite of every bound plane in zpos order to a
//      BL_FORMAT_PRGB32 output, then BLImageCodec PNG encode.
//
// Preconditions:
//   - Run from a TTY (Ctrl-Alt-F3) or another context where no
//     compositor holds the DRM master. On a running Wayland/X session,
//     Device::enable_atomic() will fail with EACCES.
//   - `input` group membership (or root) so libinput can open
//     /dev/input/event*. The example also runs under a seat session
//     via libseat when available, which gives the same access without
//     root.

#include "../../common/select_device.hpp"
#include "../../common/vt_switch.hpp"
#include "capture/png.hpp"
#include "capture/snapshot.hpp"
#include "core/device.hpp"
#include "core/resources.hpp"
#include "drm-cxx/detail/format.hpp"
#include "input/seat.hpp"
#include "session/seat.hpp"

#include <xf86drmMode.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <linux/input-event-codes.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <system_error>
#include <utility>
#include <variant>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_quit = 0;

void signal_handler(int /*sig*/) {
  g_quit = 1;
}

struct ActiveCrtc {
  std::uint32_t crtc_id{0};
  std::uint32_t mode_w{0};
  std::uint32_t mode_h{0};
};

// First connected connector with an active mode, or a specific crtc id
// if the caller pinned one with --crtc.
std::optional<ActiveCrtc> pick_crtc(int fd, std::uint32_t wanted_crtc) {
  const auto res = drm::get_resources(fd);
  if (!res) {
    return std::nullopt;
  }
  for (int i = 0; i < res->count_connectors; ++i) {
    auto conn = drm::get_connector(fd, res->connectors[i]);
    if (!conn || conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0 ||
        conn->encoder_id == 0) {
      continue;
    }
    auto enc = drm::get_encoder(fd, conn->encoder_id);
    if (!enc || enc->crtc_id == 0) {
      continue;
    }
    if (wanted_crtc != 0 && enc->crtc_id != wanted_crtc) {
      continue;
    }
    auto crtc = drm::get_crtc(fd, enc->crtc_id);
    if (!crtc || crtc->mode_valid == 0) {
      continue;
    }
    return ActiveCrtc{enc->crtc_id, crtc->mode.hdisplay, crtc->mode.vdisplay};
  }
  return std::nullopt;
}

// Count planes whose crtc_id matches ours and whose fb_id is non-zero.
// Informational — lets the REPORT key show whether there's anything to
// capture before the user triggers a snapshot.
std::uint32_t count_bound_planes(int fd, std::uint32_t crtc_id) {
  std::uint32_t bound = 0;
  auto* pres = drmModeGetPlaneResources(fd);
  if (pres == nullptr) {
    return 0;
  }
  for (std::uint32_t i = 0; i < pres->count_planes; ++i) {
    auto* pl = drmModeGetPlane(fd, pres->planes[i]);
    if (pl != nullptr && pl->crtc_id == crtc_id && pl->fb_id != 0) {
      ++bound;
    }
    if (pl != nullptr) {
      drmModeFreePlane(pl);
    }
  }
  drmModeFreePlaneResources(pres);
  return bound;
}

// "capture_YYYYMMDD_HHMMSS_NNN.png" where NNN is a monotonically
// incrementing tie-breaker so back-to-back captures don't clobber.
std::string make_capture_path(const std::filesystem::path& out_dir, std::uint32_t counter) {
  const auto now = std::chrono::system_clock::now();
  const auto t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&t, &tm);
  std::array<char, 32> stamp{};
  std::strftime(stamp.data(), stamp.size(), "%Y%m%d_%H%M%S", &tm);
  const auto filename = drm::format("capture_{}_{:03d}.png", stamp.data(), counter);
  return (out_dir / filename).string();
}

bool parse_uint(const char* s, int max_val, int& out) {
  if (s == nullptr || *s == '\0') {
    return false;
  }
  const std::string_view sv(s);
  int v = 0;
  const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
  if (ec != std::errc{} || ptr != sv.data() + sv.size() || v < 0 || v > max_val) {
    return false;
  }
  out = v;
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  // ---------------------------------------------------------------------------
  // CLI parse. --out and --crtc strip from argv before handing to select_device.
  // ---------------------------------------------------------------------------
  const char* cli_out = nullptr;
  int cli_crtc = 0;

  auto strip = [&](int i, int n) {
    for (int j = i; j + n < argc; ++j) {
      argv[j] = argv[j + n];
    }
    argc -= n;
  };

  for (int i = 1; i < argc;) {
    const bool is_out = std::strcmp(argv[i], "--out") == 0;
    const bool is_crtc = std::strcmp(argv[i], "--crtc") == 0;
    if (is_out || is_crtc) {
      if (i + 1 >= argc) {
        drm::println(stderr, "{}: missing value", argv[i]);
        return EXIT_FAILURE;
      }
      if (is_out) {
        cli_out = argv[i + 1];
      } else if (!parse_uint(argv[i + 1], 0x7fffffff, cli_crtc)) {
        drm::println(stderr, "--crtc: invalid value '{}'", argv[i + 1]);
        return EXIT_FAILURE;
      }
      strip(i, 2);
      continue;
    }
    ++i;
  }

  const std::filesystem::path out_dir =
      (cli_out != nullptr) ? std::filesystem::path(cli_out) : std::filesystem::current_path();
  std::error_code mkdir_ec;
  std::filesystem::create_directories(out_dir, mkdir_ec);
  if (mkdir_ec) {
    drm::println(stderr, "Failed to create output dir '{}': {}", out_dir.string(),
                 mkdir_ec.message());
    return EXIT_FAILURE;
  }

  const auto path = drm::examples::select_device(argc, argv);
  if (!path) {
    return EXIT_FAILURE;
  }

  // ---------------------------------------------------------------------------
  // Seat session + Device. Mirrors mouse_cursor — libseat gives us
  // both the DRM fd and the input-device opener without root when a
  // session is available.
  // ---------------------------------------------------------------------------
  auto seat = drm::session::Seat::open();

  const auto seat_dev = seat ? seat->take_device(*path) : std::nullopt;
  auto dev_holder = [&]() -> std::optional<drm::Device> {
    if (seat_dev) {
      return drm::Device::from_fd(seat_dev->fd);
    }
    auto r = drm::Device::open(*path);
    if (!r) {
      return std::nullopt;
    }
    return std::move(*r);
  }();
  if (!dev_holder) {
    drm::println(stderr, "Failed to open {}", *path);
    return EXIT_FAILURE;
  }
  auto& dev = *dev_holder;

  if (auto r = dev.enable_universal_planes(); !r) {
    drm::println(stderr, "Failed to enable universal planes: {}", r.error().message());
    return EXIT_FAILURE;
  }
  // snapshot() reads atomic-only CRTC_X/Y/W/H plane properties; without
  // this cap the kernel hides them and every plane silently fails the
  // geometry read.
  if (auto r = dev.enable_atomic(); !r) {
    drm::println(stderr,
                 "Failed to enable atomic modesetting: {} — run from a TTY "
                 "(no compositor holding DRM master)",
                 r.error().message());
    return EXIT_FAILURE;
  }

  // ---------------------------------------------------------------------------
  // CRTC discovery.
  // ---------------------------------------------------------------------------
  const auto target = pick_crtc(dev.fd(), static_cast<std::uint32_t>(cli_crtc));
  if (!target) {
    drm::println(stderr, "No connected CRTC with an active mode{}",
                 cli_crtc != 0 ? " matching --crtc" : " found");
    return EXIT_FAILURE;
  }
  drm::println("Capturing CRTC {} ({}x{}) → {}", target->crtc_id, target->mode_w, target->mode_h,
               out_dir.string());
  drm::println("Bound planes on this CRTC: {}", count_bound_planes(dev.fd(), target->crtc_id));

  // ---------------------------------------------------------------------------
  // Input seat. Keyboard-only — we don't care about pointer events.
  // ---------------------------------------------------------------------------
  drm::input::InputDeviceOpener input_opener;
  if (seat) {
    input_opener = seat->input_opener();
  }
  auto input_seat_result = drm::input::Seat::open({}, std::move(input_opener));
  if (!input_seat_result) {
    drm::println(stderr, "Failed to open input seat (need root or input group): {}",
                 input_seat_result.error().message());
    return EXIT_FAILURE;
  }
  auto& input_seat = *input_seat_result;

  std::uint32_t counter = 0;

  auto do_capture = [&]() {
    auto img = drm::capture::snapshot(dev, target->crtc_id);
    if (!img) {
      drm::println(stderr, "snapshot failed: {}", img.error().message());
      return;
    }
    const auto out = make_capture_path(out_dir, counter++);
    if (auto w = drm::capture::write_png(*img, out); !w) {
      drm::println(stderr, "write_png({}) failed: {}", out, w.error().message());
      return;
    }
    drm::println("wrote {} ({}x{})", out, img->width(), img->height());
  };

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
      g_quit = 1;
      return;
    }
    if (!ke->pressed) {
      return;
    }
    switch (ke->key) {
      case KEY_C:
      case KEY_SPACE:
        do_capture();
        break;
      case KEY_R:
        drm::println("CRTC {}: {}x{}, bound planes: {}", target->crtc_id, target->mode_w,
                     target->mode_h, count_bound_planes(dev.fd(), target->crtc_id));
        break;
      default:
        break;
    }
  });

  // ---------------------------------------------------------------------------
  // Seat pause/resume. The new fd from libseat needs UNIVERSAL_PLANES
  // and ATOMIC re-enabled before the next snapshot — those caps are
  // per-fd kernel state and don't survive the swap. Defer the rebuild
  // out of the libseat callback into the main loop so the listener
  // stays short.
  // ---------------------------------------------------------------------------
  int pending_resume_fd = -1;
  if (seat) {
    seat->set_pause_callback([&]() { (void)input_seat.suspend(); });
    seat->set_resume_callback([&](std::string_view /*path*/, int new_fd) {
      pending_resume_fd = new_fd;
      (void)input_seat.resume();
    });
  }

  drm::println("Press C or SPACE to capture, R for a state dump, Escape or Q to quit.");

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  // ---------------------------------------------------------------------------
  // Main loop. poll the input fd (and seat fd when available) and
  // dispatch on each wake.
  // ---------------------------------------------------------------------------
  std::array<pollfd, 2> pfds{};
  pfds[0].fd = input_seat.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = seat ? seat->poll_fd() : -1;
  pfds[1].events = POLLIN;

  while (g_quit == 0) {
    if (const int ret = poll(pfds.data(), pfds.size(), 100); ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      drm::println(stderr, "poll: {}", std::system_category().message(errno));
      break;
    }
    if ((pfds[0].revents & POLLIN) != 0) {
      if (auto r = input_seat.dispatch(); !r) {
        drm::println(stderr, "input dispatch failed: {}", r.error().message());
        break;
      }
    }
    if ((pfds[1].revents & POLLIN) != 0 && seat) {
      seat->dispatch();
    }

    if (pending_resume_fd != -1) {
      const int new_fd = pending_resume_fd;
      pending_resume_fd = -1;
      // dev is a reference into dev_holder, so reseating the optional
      // updates the device the rest of the loop reads through.
      dev_holder = drm::Device::from_fd(new_fd);
      if (auto r = dev.enable_universal_planes(); !r) {
        drm::println(stderr, "resume: enable_universal_planes failed: {}", r.error().message());
        break;
      }
      if (auto r = dev.enable_atomic(); !r) {
        drm::println(stderr, "resume: enable_atomic failed: {}", r.error().message());
        break;
      }
    }
  }

  drm::println("capture_demo: exiting");
  return EXIT_SUCCESS;
}
