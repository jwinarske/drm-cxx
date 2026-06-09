// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// v4l2_camera_demo — minimal demo of drm::scene::V4l2CameraSource.
//
// Walks /dev/video* for the first CAPTURE-only single-plane device
// advertising NV12 or YUYV, picks the first supported framesize,
// constructs a V4l2CameraSource, and scans it out fullscreen on the
// first connected display until Esc/Q/Ctrl-C or a fatal error.
//
// Compared to examples/scene/camera/ (libcamera + tiered fallback,
// fps overlay, status badge), this example is intentionally small:
// one source, one layer, one CRTC, no overlay. It exists to demonstrate
// that V4l2CameraSource is the right type when an application doesn't
// want libcamera and just needs raw V4L2 capture into the scene.
//
// Flags:
//   --device /dev/videoN — pick a specific V4L2 device. Default: probe.
//   --mode auto|dmabuf|mmap — buffer mode. Default: auto.
//   --width N / --height N — request specific dimensions. Default: the
//      first ENUM_FRAMESIZES entry the chosen device advertises.
//
// Session pause/resume is wired via libseat; Esc/Q/Ctrl-C quit through
// the shared libinput keyboard + VtChordTracker helper. EAGAIN from
// acquire() (camera hasn't produced a frame yet) is swallowed so the
// scene only commits once the first frame is ready.

#include "../../common/event_loop.hpp"
#include "../../common/open_output.hpp"
#include "../../common/session_pump.hpp"
#include "../../common/vt_switch.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/input/seat.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/planes/plane_registry.hpp>
#include <drm-cxx/scene/display_params.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/scene/v4l2_camera_source.hpp>
#include <drm-cxx/session/seat.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <linux/videodev2.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>

namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t k_pix_fmt_nv12 = 0x3231564EU;
constexpr std::uint32_t k_pix_fmt_yuyv = 0x56595559U;

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
  std::string device_path;
  drm::scene::V4l2CameraBufferMode mode{drm::scene::V4l2CameraBufferMode::Auto};
  std::uint32_t requested_width{0};
  std::uint32_t requested_height{0};
  bool show_help{false};
};

// Consumed flags are stripped from argv so the remainder (e.g. the
// DRM card path) is what `open_and_pick_output` sees in argv[1].
Args parse_args(int& argc, char**& argv) {
  Args out;
  int write = 1;
  for (int i = 1; i < argc; ++i) {
    std::string_view const a(argv[i]);
    if (a == "--help" || a == "-h") {
      out.show_help = true;
    } else if (a == "--device" && i + 1 < argc) {
      out.device_path = argv[++i];
    } else if (a == "--mode" && i + 1 < argc) {
      std::string_view const m(argv[++i]);
      if (m == "dmabuf") {
        out.mode = drm::scene::V4l2CameraBufferMode::DmaBufZeroCopy;
      } else if (m == "mmap") {
        out.mode = drm::scene::V4l2CameraBufferMode::MmapCopy;
      } else {
        out.mode = drm::scene::V4l2CameraBufferMode::Auto;
      }
    } else if (a == "--width" && i + 1 < argc) {
      out.requested_width = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    } else if (a == "--height" && i + 1 < argc) {
      out.requested_height = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    } else {
      argv[write++] = argv[i];
    }
  }
  argc = write;
  return out;
}

void print_help() {
  drm::println("v4l2_camera_demo — drm-cxx V4l2CameraSource demo");
  drm::println("");
  drm::println("Usage:");
  drm::println("  v4l2_camera_demo [/dev/dri/cardN] [--device /dev/videoN]");
  drm::println("                   [--mode auto|dmabuf|mmap] [--width N] [--height N]");
  drm::println("");
  drm::println("Probes /dev/video* for the first CAPTURE device advertising NV12 or YUYV");
  drm::println("if --device is not given (NV12 preferred). Quit with Esc, Q, or Ctrl-C.");
  drm::println("");
  drm::println("Driver note: amdgpu DC's plane format whitelist is NV12 + the XRGB family");
  drm::println("only — it does NOT advertise YUYV / UYVY / NV21 / YVYU on any plane. UVC");
  drm::println("YUYV-only webcams (Logitech C920 / C525) cannot reach scanout through");
  drm::println("V4l2CameraSource on amdgpu without an external YUYV→NV12 conversion.");
  drm::println("This demo refuses up front in that case; pick a camera that advertises");
  drm::println("NV12 (Logitech MX Brio does), or run on i915 / RPi / RK3588 where the");
  drm::println("display engine accepts YUYV directly.");
}

struct ProbedDevice {
  std::string path;
  std::uint32_t fourcc;
  std::uint32_t width;
  std::uint32_t height;
};

[[nodiscard]] std::optional<ProbedDevice> probe_device(const std::string& explicit_path,
                                                       std::uint32_t want_w,
                                                       std::uint32_t want_h) noexcept {
  auto try_path = [&](const std::string& p) -> std::optional<ProbedDevice> {
    int const fd = ::open(p.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
      return std::nullopt;
    }
    v4l2_capability cap{};
    if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
      ::close(fd);
      return std::nullopt;
    }
    std::uint32_t const caps =
        ((cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U) ? cap.device_caps : cap.capabilities;
    bool const is_capture = (caps & V4L2_CAP_VIDEO_CAPTURE) != 0U;
    bool const is_m2m = (caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)) != 0U;
    if (!is_capture || is_m2m || (caps & V4L2_CAP_STREAMING) == 0U) {
      ::close(fd);
      return std::nullopt;
    }
    // Prefer NV12 over YUYV: amdgpu DC's plane format whitelist is
    // NV12 + the XRGB family — no plane on the device advertises YUYV,
    // so on amdgpu the YUYV path can't reach scanout at all. NV12 is
    // universally accepted.
    std::optional<std::uint32_t> nv12_seen;
    std::optional<std::uint32_t> yuyv_seen;
    for (std::uint32_t i = 0; i < 64; ++i) {
      v4l2_fmtdesc desc{};
      desc.index = i;
      desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      if (::ioctl(fd, VIDIOC_ENUM_FMT, &desc) != 0) {
        break;
      }
      if (desc.pixelformat == k_pix_fmt_nv12 && !nv12_seen.has_value()) {
        nv12_seen = desc.pixelformat;
      } else if (desc.pixelformat == k_pix_fmt_yuyv && !yuyv_seen.has_value()) {
        yuyv_seen = desc.pixelformat;
      }
    }
    std::optional<std::uint32_t> chosen_fourcc = nv12_seen ? nv12_seen : yuyv_seen;
    if (!chosen_fourcc.has_value()) {
      ::close(fd);
      return std::nullopt;
    }
    std::uint32_t width = want_w;
    std::uint32_t height = want_h;
    if (width == 0 || height == 0) {
      v4l2_frmsizeenum fz{};
      fz.index = 0;
      fz.pixel_format = *chosen_fourcc;
      if (::ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fz) == 0) {
        if (fz.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
          width = fz.discrete.width;
          height = fz.discrete.height;
        } else {
          width = fz.stepwise.min_width;
          height = fz.stepwise.min_height;
        }
      }
    }
    ::close(fd);
    if (width == 0 || height == 0) {
      return std::nullopt;
    }
    return ProbedDevice{p, *chosen_fourcc, width, height};
  };

  if (!explicit_path.empty()) {
    return try_path(explicit_path);
  }
  std::error_code ec;
  for (auto const& entry : fs::directory_iterator("/dev", ec)) {
    auto const& p = entry.path();
    std::string const name = p.filename().string();
    if (name.rfind("video", 0) != 0) {
      continue;
    }
    if (auto r = try_path(p.string()); r.has_value()) {
      return r;
    }
  }
  return std::nullopt;
}

const char* fourcc_name(std::uint32_t f) noexcept {
  if (f == k_pix_fmt_nv12) {
    return "NV12";
  }
  if (f == k_pix_fmt_yuyv) {
    return "YUYV";
  }
  return "?";
}

[[nodiscard]] std::uint32_t v4l2_to_drm_fourcc(std::uint32_t v4l2_fourcc) noexcept {
  if (v4l2_fourcc == k_pix_fmt_nv12) {
    return DRM_FORMAT_NV12;
  }
  if (v4l2_fourcc == k_pix_fmt_yuyv) {
    return DRM_FORMAT_YUYV;
  }
  return 0;
}

// Returns true iff at least one plane on `dev` advertises `drm_fourcc`.
// The check uses the kernel's IN_FORMATS (or bare format list on legacy
// drivers) and is the same check the kernel applies inside drmModeAddFB2,
// so a positive answer here means V4l2CameraSource::create won't EINVAL
// at the FB-mint step on a format-mismatch basis. (It can still fail
// for other reasons — UVC vmalloc dma-buf rejection on amdgpu, etc.)
[[nodiscard]] bool any_plane_supports_format(const drm::Device& dev,
                                             std::uint32_t drm_fourcc) noexcept {
  auto reg = drm::planes::PlaneRegistry::enumerate(dev);
  if (!reg) {
    return false;
  }
  const auto planes = reg->all();
  return std::any_of(planes.begin(), planes.end(),
                     [drm_fourcc](const drm::planes::PlaneCapabilities& cap) {
                       return cap.supports_format(drm_fourcc);
                     });
}

// Center `cam_w x cam_h` inside the `fb_w x fb_h` framebuffer, scaling
// up as far as it fits without distorting the source aspect ratio. The
// camera fills whichever axis is the binding constraint; the other
// axis gets equal margins above/below or left/right. Mirrors the
// `fit_within` helper in examples/scene/camera/main.cpp.
[[nodiscard]] drm::scene::Rect fit_centered(std::uint32_t cam_w, std::uint32_t cam_h,
                                            std::uint32_t fb_w, std::uint32_t fb_h) noexcept {
  if (cam_w == 0 || cam_h == 0 || fb_w == 0 || fb_h == 0) {
    return {0, 0, fb_w, fb_h};
  }
  const std::uint64_t scaled_w_by_h = static_cast<std::uint64_t>(cam_w) * fb_h;
  const std::uint64_t scaled_h_by_w = static_cast<std::uint64_t>(cam_h) * fb_w;
  std::uint32_t dst_w = fb_w;
  std::uint32_t dst_h = fb_h;
  if (scaled_w_by_h > scaled_h_by_w) {
    dst_h = static_cast<std::uint32_t>(scaled_h_by_w / cam_w);
  } else {
    dst_w = static_cast<std::uint32_t>(scaled_w_by_h / cam_h);
  }
  const auto x = static_cast<std::int32_t>((fb_w - dst_w) / 2);
  const auto y = static_cast<std::int32_t>((fb_h - dst_h) / 2);
  return {x, y, dst_w, dst_h};
}

const char* mode_name(drm::scene::V4l2CameraBufferMode m) noexcept {
  switch (m) {
    case drm::scene::V4l2CameraBufferMode::Auto:
      return "Auto";
    case drm::scene::V4l2CameraBufferMode::DmaBufZeroCopy:
      return "DmaBufZeroCopy";
    case drm::scene::V4l2CameraBufferMode::MmapCopy:
      return "MmapCopy";
  }
  return "?";
}

}  // namespace

int main(int argc, char* argv[]) {
  const Args args = parse_args(argc, argv);
  if (args.show_help) {
    print_help();
    return EXIT_SUCCESS;
  }
  install_signal_handler();

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

  auto probed = probe_device(args.device_path, args.requested_width, args.requested_height);
  if (!probed.has_value()) {
    drm::println(stderr, "No usable V4L2 CAPTURE device (NV12/YUYV) found.");
    return EXIT_FAILURE;
  }
  drm::println("Camera: {} {} {}x{}", probed->path, fourcc_name(probed->fourcc), probed->width,
               probed->height);

  // Up-front plane-format check: refuse cleanly when no plane on the
  // chosen DRM device advertises the camera's native format. Without
  // this, V4l2CameraSource::create() returns a bare "Invalid argument"
  // from deep inside drmModeAddFB2 — useless for diagnosing the actual
  // cause, which on amdgpu DC is the kernel's plane-format whitelist
  // (NV12 + XRGB-family only; no YUYV / UYVY / NV21).
  std::uint32_t const drm_fourcc = v4l2_to_drm_fourcc(probed->fourcc);
  if (drm_fourcc == 0 || !any_plane_supports_format(dev, drm_fourcc)) {
    drm::println(stderr, "No plane on this DRM device advertises {} — refusing to start.",
                 fourcc_name(probed->fourcc));
    drm::println(stderr,
                 "  Driver: amdgpu DC's plane whitelist is NV12 + XRGB only; YUYV / UYVY / NV21");
    drm::println(stderr,
                 "  are not scanout-capable. Try a camera that advertises NV12 (e.g. Logitech MX");
    drm::println(stderr,
                 "  Brio), or run on i915 / RPi / RK3588 where YUYV is supported by the planes.");
    return EXIT_FAILURE;
  }

  drm::scene::V4l2CameraConfig cam_cfg;
  cam_cfg.pixel_fourcc = probed->fourcc;
  cam_cfg.width = probed->width;
  cam_cfg.height = probed->height;
  cam_cfg.buffer_count = 4;
  cam_cfg.mode = args.mode;
  auto cam_r = drm::scene::V4l2CameraSource::create(dev, probed->path.c_str(), cam_cfg);
  if (!cam_r) {
    drm::println(stderr, "V4l2CameraSource::create: {} (mode={})", cam_r.error().message(),
                 mode_name(args.mode));
    return EXIT_FAILURE;
  }
  drm::println("V4l2CameraSource ready (active_mode={})", mode_name((*cam_r)->active_mode()));

  drm::scene::LayerScene::Config scene_cfg;
  scene_cfg.crtc_id = output.crtc_id;
  scene_cfg.connector_id = output.connector_id;
  scene_cfg.mode = output.mode;
  auto scene_r = drm::scene::LayerScene::create(dev, scene_cfg);
  if (!scene_r) {
    drm::println(stderr, "LayerScene::create: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_r);

  // Preserve the camera's aspect ratio rather than stretching it to
  // the full framebuffer. A 4:3 webcam on a 16:9 (or 7:6 here) display
  // gets pillar/letterbox bars; the alternative would stretch faces.
  drm::scene::Rect const dst = fit_centered(probed->width, probed->height, fb_w, fb_h);
  drm::println("Layer: src {}x{} → dst {}x{} at ({}, {}) [aspect-preserved]", probed->width,
               probed->height, dst.w, dst.h, dst.x, dst.y);
  drm::scene::LayerDesc desc_layer;
  desc_layer.source = std::move(*cam_r);
  desc_layer.display.src_rect = drm::scene::Rect{0, 0, probed->width, probed->height};
  desc_layer.display.dst_rect = dst;
  desc_layer.content_type = drm::planes::ContentType::Video;
  auto layer_r = scene->add_layer(std::move(desc_layer));
  if (!layer_r) {
    drm::println(stderr, "add_layer: {}", layer_r.error().message());
    return EXIT_FAILURE;
  }
  // The downcast is safe by construction — we just constructed a
  // V4l2CameraSource into this layer's source slot.
  auto* cam_source =
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
      static_cast<drm::scene::V4l2CameraSource*>(&scene->get_layer(*layer_r)->source());

  // libinput keyboard for Esc/Q/Ctrl-C.
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

  drm::examples::SessionPumpState session_state;
  if (seat) {
    drm::examples::wire_session_callbacks(*seat, *scene, session_state, &input_seat);
  }

  // Preroll: wait for the camera to produce its first frame before the
  // first commit. Without this the scene commits with no fb_id, the
  // kernel modesets PRIMARY blank, and no page-flip-event lands.
  {
    constexpr std::chrono::seconds k_preroll_timeout{5};
    const auto deadline = std::chrono::steady_clock::now() + k_preroll_timeout;
    bool got_first = false;
    pollfd pfds[2]{};
    pfds[0].fd = cam_source->fd();
    pfds[0].events = POLLIN;
    pfds[1].fd = input_seat.fd();
    pfds[1].events = POLLIN;
    while (!quit && !g_quit.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < deadline) {
      if (auto r = cam_source->drive(); !r) {
        drm::println(stderr, "preroll drive(): {}", r.error().message());
        return EXIT_FAILURE;
      }
      auto acq = cam_source->acquire();
      if (acq.has_value()) {
        cam_source->release(std::move(*acq));
        got_first = true;
        break;
      }
      if (acq.error() != std::make_error_code(std::errc::resource_unavailable_try_again)) {
        drm::println(stderr, "preroll acquire(): {}", acq.error().message());
        return EXIT_FAILURE;
      }
      if (::poll(pfds, 2, 10) > 0) {
        if ((pfds[1].revents & POLLIN) != 0) {
          (void)input_seat.dispatch();
        }
      }
    }
    if (!got_first) {
      drm::println(stderr, "preroll: camera produced no frame within {}s",
                   k_preroll_timeout.count());
      return EXIT_FAILURE;
    }
  }

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
    return EXIT_FAILURE;
  }
  session_state.flip_pending = true;

  drm::println("Streaming — Esc/Q/Ctrl-C to quit.");

  drm::examples::EventLoop loop;
  (void)loop.add_slot(input_seat.fd(), [&] { (void)input_seat.dispatch(); });
  int const drm_slot = loop.add_slot(dev.fd(), [&] { (void)page_flip.dispatch(0); });
  (void)loop.add_slot(seat ? seat->poll_fd() : -1, [&] {
    if (seat) {
      seat->dispatch();
    }
  });
  (void)loop.add_slot(cam_source->fd(), [&] {
    if (auto r = cam_source->drive(); !r) {
      drm::println(stderr, "drive(): {}", r.error().message());
    }
  });

  bool error_exit = false;
  while (!quit && !g_quit.load(std::memory_order_relaxed)) {
    // Default: block until a slot fires (camera frame, input event,
    // page-flip event, session change). Busy-spinning at timeout=0
    // would burn CPU between camera frames; with the slots wired
    // above, every interesting event already has an fd that wakes us.
    int timeout_ms = -1;
    if (session_state.flip_pending) {
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
      page_flip = make_page_flip();
      session_state.flip_pending = false;
    }

    if (session_state.flip_pending || session_state.paused) {
      continue;
    }

    auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, &page_flip);
    if (!r) {
      if (r.error() == std::errc::permission_denied) {
        session_state.paused = true;
        session_state.flip_pending = false;
        continue;
      }
      if (r.error() == std::errc::resource_unavailable_try_again) {
        session_state.flip_pending = false;
        continue;
      }
      drm::println(stderr, "commit: {}", r.error().message());
      error_exit = true;
      break;
    }
    // Only mark flip_pending when the commit actually attached a new
    // FB the kernel will scan out — otherwise no page-flip event will
    // arrive and the loop would deadlock waiting for it. A commit
    // where every source returned EAGAIN from acquire() returns
    // success but updates no plane state.
    if (r->fbs_attached > 0) {
      session_state.flip_pending = true;
    }
  }

  scene.reset();
  return error_exit ? EXIT_FAILURE : EXIT_SUCCESS;
}
