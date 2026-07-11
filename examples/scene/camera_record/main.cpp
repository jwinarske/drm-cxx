// SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// camera_record — headless hardware H.264 recorder.
//
// Captures MJPEG from a V4L2 webcam, decodes it to NV12 on the GPU
// (VaapiJpegDecoder), encodes that surface to H.264 on the GPU
// (VaapiH264Encoder), and writes an Annex-B elementary stream to a file —
// with no display and no DRM master, so it runs over SSH / on a headless box.
// It's the "recording consumer" of the camera example distilled to the parts
// that don't need scanout, and the way that path is validated end to end.
//
// The camera example's on-screen `R` / `--snapshot` still exercise the JPEG
// path; this tool is the H.264 counterpart and the CI/headless-friendly way to
// prove the decode -> encode pipeline against a real sensor.
//
//   camera_record [--device /dev/videoN] [--out FILE] [--frames N]
//                 [--size WxH] [--fps N] [--qp N]
//
// Play the result with:  ffplay out.h264   (or mpv / gst-play-1.0)
//
// Build is gated on libva + libva-drm (CAMERA_HAS_VAAPI); without them this TU
// is an empty stub that prints a message and exits.

#include "../camera/vaapi_h264_encoder.hpp"
#include "../camera/vaapi_jpeg_decoder.hpp"

#include <drm-cxx/detail/format.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#if CAMERA_HAS_VAAPI

#include <cerrno>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

using drm::examples::camera::VaapiH264Encoder;
using drm::examples::camera::VaapiJpegDecoder;

constexpr int k_buffer_count = 4;

struct MappedBuf {
  void* start = nullptr;
  std::size_t length = 0;
};

int xioctl(int fd, unsigned long req, void* arg) {
  int r = 0;
  for (;;) {
    r = ::ioctl(fd, req, arg);
    if (r != -1 || errno != EINTR) {
      break;
    }
  }
  return r;
}

struct Args {
  const char* device = "/dev/video0";
  const char* out = "camera_record.h264";
  int frames = 150;
  std::uint32_t width = 1280;
  std::uint32_t height = 720;
  std::uint32_t fps = 30;
  std::uint32_t qp = 26;
};

bool parse_size(const char* s, std::uint32_t& w, std::uint32_t& h) {
  const char* x = std::strchr(s, 'x');
  if (x == nullptr) {
    return false;
  }
  w = static_cast<std::uint32_t>(std::atoi(s));
  h = static_cast<std::uint32_t>(std::atoi(x + 1));
  return w != 0 && h != 0;
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) — a throw here just aborts the tool
int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (arg == "--device") {
      a.device = next();
    } else if (arg == "--out") {
      a.out = next();
    } else if (arg == "--frames") {
      a.frames = std::atoi(next());
    } else if (arg == "--fps") {
      a.fps = static_cast<std::uint32_t>(std::atoi(next()));
    } else if (arg == "--qp") {
      a.qp = static_cast<std::uint32_t>(std::atoi(next()));
    } else if (arg == "--size") {
      if (const char* s = next(); s == nullptr || !parse_size(s, a.width, a.height)) {
        drm::println(stderr, "--size WxH expected (e.g. 1280x720)");
        return EXIT_FAILURE;
      }
    } else {
      drm::println(stderr,
                   "usage: camera_record [--device /dev/videoN] [--out FILE] "
                   "[--frames N] [--size WxH] [--fps N] [--qp N]");
      return EXIT_FAILURE;
    }
  }

  // ── V4L2 MJPEG capture setup ──────────────────────────────────────────
  const int fd = ::open(a.device, O_RDWR);
  if (fd < 0) {
    drm::println(stderr, "open {}: {}", a.device, std::strerror(errno));
    return EXIT_FAILURE;
  }

  v4l2_format fmt{};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = a.width;
  fmt.fmt.pix.height = a.height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
    drm::println(stderr, "VIDIOC_S_FMT(MJPEG): {}", std::strerror(errno));
    ::close(fd);
    return EXIT_FAILURE;
  }
  a.width = fmt.fmt.pix.width;
  a.height = fmt.fmt.pix.height;
  if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
    drm::println(stderr, "camera does not offer MJPEG at this size");
    ::close(fd);
    return EXIT_FAILURE;
  }

  v4l2_requestbuffers req{};
  req.count = k_buffer_count;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
    drm::println(stderr, "VIDIOC_REQBUFS: {}", std::strerror(errno));
    ::close(fd);
    return EXIT_FAILURE;
  }
  std::vector<MappedBuf> bufs(req.count);
  for (unsigned int i = 0; i < req.count; ++i) {
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = i;
    if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
      drm::println(stderr, "VIDIOC_QUERYBUF: {}", std::strerror(errno));
      ::close(fd);
      return EXIT_FAILURE;
    }
    bufs[i].length = b.length;
    bufs[i].start = ::mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
    if (bufs[i].start == MAP_FAILED) {
      drm::println(stderr, "mmap: {}", std::strerror(errno));
      ::close(fd);
      return EXIT_FAILURE;
    }
    if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
      drm::println(stderr, "VIDIOC_QBUF: {}", std::strerror(errno));
      ::close(fd);
      return EXIT_FAILURE;
    }
  }
  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
    drm::println(stderr, "VIDIOC_STREAMON: {}", std::strerror(errno));
    ::close(fd);
    return EXIT_FAILURE;
  }

  // ── VA-API decode + encode ────────────────────────────────────────────
  const int render_fd = ::open("/dev/dri/renderD128", O_RDWR);
  if (render_fd < 0) {
    drm::println(stderr, "open renderD128: {}", std::strerror(errno));
    return EXIT_FAILURE;
  }
  std::error_code ec;
  void* disp = VaapiJpegDecoder::open_display(render_fd, &ec);
  if (disp == nullptr) {
    drm::println(stderr, "open_display: {}", ec.message());
    return EXIT_FAILURE;
  }

  auto sampling = VaapiJpegDecoder::Sampling::Yuv420;
  auto decoder = VaapiJpegDecoder::create(disp, a.width, a.height, sampling, &ec);
  if (!decoder) {
    drm::println(stderr, "decoder create: {}", ec.message());
    return EXIT_FAILURE;
  }

  VaapiH264Encoder::Config cfg{};
  cfg.width = a.width;
  cfg.height = a.height;
  cfg.fps = a.fps;
  cfg.qp = a.qp;
  auto encoder = VaapiH264Encoder::create(disp, cfg, &ec);
  if (!encoder) {
    drm::println(stderr, "encoder create: {}", ec.message());
    return EXIT_FAILURE;
  }

  FILE* out = std::fopen(a.out, "wb");
  if (out == nullptr) {
    drm::println(stderr, "open {}: {}", a.out, std::strerror(errno));
    return EXIT_FAILURE;
  }

  drm::println("recording {}x{} MJPEG from {} @ {}fps -> {} (qp {})", a.width, a.height, a.device,
               a.fps, a.out, a.qp);

  std::size_t total = 0;
  int encoded = 0;
  std::vector<std::uint8_t> bits;
  for (int f = 0; f < a.frames; ++f) {
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
      drm::println(stderr, "VIDIOC_DQBUF: {}", std::strerror(errno));
      break;
    }
    const auto* jpeg = static_cast<const std::uint8_t*>(bufs[b.index].start);

    if (!decoder->decode_into_surface(jpeg, b.bytesused)) {
      // A chroma-sampling mismatch on the first frames: rebuild the decoder
      // with the sampling the JPEG actually uses (UVC MJPEG is often 4:2:2).
      const auto det = decoder->detected_sampling();
      if (det != VaapiJpegDecoder::Sampling::Unknown && det != sampling) {
        sampling = det;
        decoder = VaapiJpegDecoder::create(disp, a.width, a.height, sampling, &ec);
        drm::println(stderr, "rebuilt decoder as {}",
                     sampling == VaapiJpegDecoder::Sampling::Yuv422 ? "4:2:2" : "4:2:0");
      }
      xioctl(fd, VIDIOC_QBUF, &b);
      continue;
    }

    bits.clear();
    if (decoder && encoder->encode_surface(decoder->output_surface(), bits)) {
      std::fwrite(bits.data(), 1, bits.size(), out);
      total += bits.size();
      ++encoded;
    }
    xioctl(fd, VIDIOC_QBUF, &b);
  }

  std::fclose(out);
  xioctl(fd, VIDIOC_STREAMOFF, &type);
  for (auto& mb : bufs) {
    if (mb.start != nullptr && mb.start != MAP_FAILED) {
      ::munmap(mb.start, mb.length);
    }
  }
  ::close(fd);
  encoder.reset();
  decoder.reset();
  VaapiJpegDecoder::close_display(disp);
  ::close(render_fd);

  drm::println("wrote {}: {} frames, {} bytes", a.out, encoded, total);
  return EXIT_SUCCESS;
}

#else  // CAMERA_HAS_VAAPI

int main() {
  drm::println(stderr, "camera_record: built without libva — VA-API H.264 recording unavailable");
  return EXIT_FAILURE;
}

#endif  // CAMERA_HAS_VAAPI
