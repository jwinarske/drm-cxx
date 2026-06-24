// SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// v4l2_decode — feed an Annex-B H.264 file to drm::scene::V4l2DecoderSource and
// scan the decoded NV12 frames out on a KMS plane. A dongle-free, GStreamer-free
// repro of the stateful-V4L2 hardware-decode -> display path (e.g. the Raspberry
// Pi bcm2835-codec), and the validation rig for tiled-modifier (SAND/AFBC)
// import support: it prints the negotiated (fourcc, modifier) and frame rate.
//
// Usage:
//   v4l2_decode /dev/dri/cardN --file clip.h264 [--no-seat] [--codec /dev/videoN]
//               [--modifier linear|sand] [--size WxH]
//
//   The DRM card is the first argument. --size sizes the decoder only; the
//   display stays at the connector's preferred mode and the hardware plane
//   scales the decoded frame to fill it.
//   --codec     V4L2 decoder node; default = scan /dev/video* for an H.264 M2M
//               decoder (handles the Pi's /dev/video10+ numbering).
//   --modifier  CAPTURE pixel format: linear -> V4L2_PIX_FMT_NV12 (default),
//               sand -> V4L2_PIX_FMT_NV12_COL128 (needs drm-cxx tiled import).
//   --size WxH  Coded size hint (default = the display mode). Match the clip's
//               resolution to avoid a mid-stream SOURCE_CHANGE.
//
// MUST run on a free VT with DRM master. Quit with Ctrl-C; plays to EOF then
// holds the last frame.

#include "../../common/open_output.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/scene/v4l2_decoder_source.hpp>

#include <drm_mode.h>

#include <fcntl.h>
#include <ios>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <system_error>
#include <unistd.h>
#include <utility>

// Broadcom SAND column-tiled NV12 (the Pi codec's native CAPTURE format). A
// fairly recent uAPI addition; define it if the build host's headers predate it.
#ifndef V4L2_PIX_FMT_NV12_COL128
#define V4L2_PIX_FMT_NV12_COL128 v4l2_fourcc('N', 'C', '1', '2')
#endif

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_quit = 0;
void on_quit(int /*unused*/) {
  g_quit = 1;
}

// Scan /dev/video0..63 for a V4L2 M2M device whose OUTPUT advertises H.264.
// The Pi's decoder is /dev/video10 (nodes start at 10), which the 0..63 walk
// reaches; the first matching node is the stateful decoder.
std::string find_h264_decoder() {
  for (int i = 0; i < 64; ++i) {
    char path[32];
    std::snprintf(path, sizeof path, "/dev/video%d", i);
    int const fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    bool ok = false;
    v4l2_capability cap{};
    if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
      const std::uint32_t caps =
          (cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U ? cap.device_caps : cap.capabilities;
      const bool mplane = (caps & V4L2_CAP_VIDEO_M2M_MPLANE) != 0U;
      if (mplane || (caps & V4L2_CAP_VIDEO_M2M) != 0U) {
        for (std::uint32_t idx = 0; idx < 256U; ++idx) {
          v4l2_fmtdesc d{};
          d.index = idx;
          d.type = mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
          if (::ioctl(fd, VIDIOC_ENUM_FMT, &d) != 0) {
            break;
          }
          if (d.pixelformat == V4L2_PIX_FMT_H264) {
            ok = true;
            break;
          }
        }
      }
    }
    ::close(fd);
    if (ok) {
      return path;
    }
  }
  return {};
}

const char* arg_value(int argc, char** argv, const char* flag) {
  for (int i = 1; i + 1 < argc; ++i) {
    // NOLINTNEXTLINE(clang-analyzer-core.NonNullParamChecker) -- argv[1..argc) are non-null.
    if (std::strcmp(argv[i], flag) == 0) {
      return argv[i + 1];
    }
  }
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  // The DRM device is argv[1] (the drm-cxx example convention that
  // open_and_pick_output / select_device follow); the clip and everything else
  // are named flags so they never collide with that positional.
  const char* clip = arg_value(argc, argv, "--file");
  if (clip == nullptr) {
    std::fprintf(stderr,
                 "usage: v4l2_decode /dev/dri/cardN --file clip.h264 [--no-seat] "
                 "[--codec /dev/videoN] [--modifier linear|sand] [--size WxH]\n");
    return 2;
  }

  std::signal(SIGINT, on_quit);
  std::signal(SIGTERM, on_quit);

  // 1. DRM output (connected connector + CRTC + preferred mode).
  auto out = drm::examples::open_and_pick_output(argc, argv);
  if (!out) {
    std::fprintf(stderr, "no DRM output (run on a free VT; try --no-seat)\n");
    return 1;
  }
  drm::Device& dev = out->device;
  const std::uint32_t mode_w = out->mode.hdisplay;
  const std::uint32_t mode_h = out->mode.vdisplay;

  // 2. V4L2 H.264 decoder.
  const char* codec_arg = arg_value(argc, argv, "--codec");
  const std::string codec = codec_arg != nullptr ? codec_arg : find_h264_decoder();
  if (codec.empty()) {
    std::fprintf(stderr, "no H.264 V4L2 decoder found (try --codec /dev/videoN)\n");
    return 1;
  }
  const char* mod = arg_value(argc, argv, "--modifier");
  const bool want_sand = mod != nullptr && std::strcmp(mod, "sand") == 0;

  std::uint32_t coded_w = mode_w;
  std::uint32_t coded_h = mode_h;
  if (const char* size = arg_value(argc, argv, "--size"); size != nullptr) {
    unsigned w = 0;
    unsigned h = 0;
    // NOLINTNEXTLINE(cert-err34-c)
    if (std::sscanf(size, "%ux%u", &w, &h) == 2 && w != 0 && h != 0) {
      coded_w = w;
      coded_h = h;
    }
  }

  drm::scene::V4l2DecoderConfig cfg;
  cfg.codec_fourcc = V4L2_PIX_FMT_H264;
  cfg.capture_fourcc = want_sand ? V4L2_PIX_FMT_NV12_COL128 : V4L2_PIX_FMT_NV12;
  cfg.coded_width = coded_w;
  cfg.coded_height = coded_h;
  cfg.output_buffer_count = 4;
  cfg.capture_buffer_count = 6;
  cfg.output_buffer_size = static_cast<std::size_t>(coded_w) * coded_h;

  auto src_r = drm::scene::V4l2DecoderSource::create(dev, codec.c_str(), cfg);
  if (!src_r) {
    std::fprintf(stderr, "V4l2DecoderSource::create(%s): %s\n", codec.c_str(),
                 src_r.error().message().c_str());
    return 1;
  }

  // 3. Scene + a single full-screen video layer. Move the source into the
  //    layer, then borrow it back (downcast) to drive the decode side.
  drm::scene::LayerScene::Config scfg;
  scfg.crtc_id = out->crtc_id;
  scfg.connector_id = out->connector_id;
  scfg.mode = out->mode;
  auto scene_r = drm::scene::LayerScene::create(dev, scfg);
  if (!scene_r) {
    std::fprintf(stderr, "LayerScene::create: %s\n", scene_r.error().message().c_str());
    return 1;
  }
  auto scene = std::move(*scene_r);

  drm::scene::LayerDesc desc;
  desc.source = std::move(*src_r);
  desc.display.src_rect = {0, 0, coded_w, coded_h};
  desc.display.dst_rect = {0, 0, mode_w, mode_h};  // full-screen (HW plane scales)
  desc.content_type = drm::planes::ContentType::Video;
  auto layer = scene->add_layer(std::move(desc));
  if (!layer) {
    std::fprintf(stderr, "add_layer: %s\n", layer.error().message().c_str());
    return 1;
  }
  auto* vsrc = dynamic_cast<drm::scene::V4l2DecoderSource*>(&scene->get_layer(*layer)->source());
  const int dec_fd = vsrc->fd();
  const auto fmt = vsrc->format();
  std::fprintf(stderr, "decoder %s: %ux%u  drm_fourcc=0x%08x  modifier=0x%016llx\n", codec.c_str(),
               fmt.width, fmt.height, fmt.drm_fourcc,
               static_cast<unsigned long long>(fmt.modifier));

  // 4. Decode on a pump thread, present on the main thread. The pump drives the
  //    V4L2 decoder continuously (feed bitstream, dequeue frames) so it never
  //    starves while the commit loop waits on a flip -- the failure mode of a
  //    single-threaded loop. One mutex serializes every source access: the pump
  //    holds it for submit_bitstream/drive, the main thread holds it around
  //    scene->commit(), which calls the source's acquire/release internally.
  std::mutex src_mtx;
  std::atomic<std::uint64_t> flips{0};  // frames actually scanned out

  std::thread pump([&] {
    std::ifstream file(clip, std::ios::binary);
    if (!file) {
      std::fprintf(stderr, "cannot open %s\n", clip);
      g_quit = 1;
      return;
    }
    std::vector<std::uint8_t> chunk;
    bool have_chunk = false;
    unsigned loops = 0;
    while (g_quit == 0) {
      {
        const std::lock_guard<std::mutex> lock(src_mtx);
        // Feed one coded chunk; hold it across EAGAIN (OUTPUT queue full). At end
        // of file, rewind and keep feeding -- a stateful decoder restarts on the
        // stream's leading SPS/PPS/IDR, so the clip loops and the CAPTURE queue
        // never drains (a drained queue drops the layer and freezes the display).
        if (!have_chunk) {
          chunk.assign(std::size_t{64} * 1024U, 0);
          file.read(reinterpret_cast<char*>(chunk.data()),
                    static_cast<std::streamsize>(chunk.size()));
          const auto n = file.gcount();
          if (n <= 0) {
            file.clear();
            file.seekg(0);
            // Compatibility watchdog. A decoder that supports the stream produces
            // frames within the first pass; if several full passes go by with
            // nothing scanned out, it is consuming the bitstream but cannot decode
            // it -- typically a profile beyond the hardware's reach (the Pi tops
            // out at H.264 High and cannot do High 4:4:4). Report and stop instead
            // of leaving a silent black screen.
            if (++loops == 5 && flips.load() < 5) {
              std::fprintf(stderr,
                           "no frames decoded after feeding the clip 5 times -- the V4L2 decoder "
                           "does not support this stream (e.g. an H.264 profile beyond the "
                           "hardware's capability, such as High 4:4:4)\n");
              g_quit = 1;
              return;
            }
            continue;
          }
          chunk.resize(static_cast<std::size_t>(n));
          have_chunk = true;
        }
        if (have_chunk) {
          auto sr =
              vsrc->submit_bitstream(drm::span<const std::uint8_t>(chunk.data(), chunk.size()), 0);
          if (sr) {
            have_chunk = false;
          } else if (sr.error() !=
                     std::make_error_code(std::errc::resource_unavailable_try_again)) {
            std::fprintf(stderr, "submit_bitstream: %s\n", sr.error().message().c_str());
            have_chunk = false;
          }
        }
        if (auto dr = vsrc->drive(); !dr) {
          if (dr.error() == std::make_error_code(std::errc::operation_canceled)) {
            std::fprintf(stderr, "decoder gave up after a resolution change it could not apply\n");
          } else {
            std::fprintf(stderr, "drive: %s\n", dr.error().message().c_str());
          }
          g_quit = 1;
          return;
        }
      }
      // Block for more decoder work outside the lock so the commit thread runs.
      pollfd p{dec_fd, POLLIN, 0};
      ::poll(&p, 1, 5);
    }
  });

  drm::PageFlip page_flip(dev);
  bool flip_pending = false;
  bool first_done = false;  // the first commit modesets and must not be NONBLOCK
  page_flip.set_handler([&](std::uint32_t, std::uint64_t, std::uint64_t) {
    flip_pending = false;
    const std::uint64_t n = flips.fetch_add(1) + 1;
    if (n % 60 == 0) {
      std::fprintf(stderr, "displayed %llu frames\n", static_cast<unsigned long long>(n));
    }
  });

  std::fprintf(stderr, "playing %s — Ctrl-C to quit\n", clip);
  while (g_quit == 0) {
    pollfd p{dev.fd(), POLLIN, 0};
    const int pr = ::poll(&p, 1, flip_pending ? 100 : 4);
    if ((p.revents & POLLIN) != 0) {
      (void)page_flip.dispatch(0);
    } else if (flip_pending && pr == 0) {
      // No flip within the timeout: the commit re-presented an unchanged buffer,
      // which vc4 retires without an event. Clear the flag so the loop keeps
      // presenting fresh frames instead of wedging.
      flip_pending = false;
    }

    if (!flip_pending) {
      // The first commit carries the modeset (LayerScene injects ALLOW_MODESET),
      // which cannot be non-blocking; steady-state flips are NONBLOCK. Before the
      // first decoded frame the commit returns EAGAIN -- just retry.
      const std::uint32_t flags = first_done ? (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK)
                                             : DRM_MODE_PAGE_FLIP_EVENT;
      const std::lock_guard<std::mutex> lock(src_mtx);
      if (auto r = scene->commit(flags, &page_flip)) {
        flip_pending = true;
        first_done = true;
      } else if (r.error() != std::make_error_code(std::errc::resource_unavailable_try_again)) {
        std::fprintf(stderr, "commit: %s\n", r.error().message().c_str());
      }
    }
  }

  g_quit = 1;
  pump.join();
  std::fprintf(stderr, "stopping\n");
  return 0;
}
