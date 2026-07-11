// SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// camera_record — headless hardware H.264 recorder.
//
// Captures from a V4L2 webcam and encodes to an Annex-B H.264 file with no
// display and no DRM master, so it runs over SSH / on a headless box. It's the
// "recording consumer" of the camera example distilled to the parts that don't
// need scanout, and the way that path is validated end to end.
//
// Two encode backends, picked by what the box has:
//
//   * V4L2 stateful M2M (--v4l2-encoder /dev/videoN, e.g. the Pi's
//     bcm2835-codec): capture YUYV -> V4l2H264Encoder -> H.264. No decode; the
//     encoder inserts its own SPS/PPS.
//   * VA-API (default where libva is present, e.g. AMD/Intel): capture MJPEG ->
//     VaapiJpegDecoder (-> NV12) -> VaapiH264Encoder -> H.264, staying on the
//     GPU.
//
// Play the result with:  ffplay out.h264   (or mpv / gst-play-1.0)
//
//   camera_record [--device /dev/videoN] [--out FILE] [--frames N] [--size WxH]
//                 [--fps N] [--qp N] [--v4l2-encoder /dev/videoN]

#include "../camera/v4l2_h264_encoder.hpp"
#if CAMERA_HAS_VAAPI
#include "../camera/vaapi_h264_encoder.hpp"
#include "../camera/vaapi_jpeg_decoder.hpp"
#endif

#include <drm-cxx/detail/format.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <memory>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

using drm::examples::camera::V4l2H264Encoder;
#if CAMERA_HAS_VAAPI
using drm::examples::camera::VaapiH264Encoder;
using drm::examples::camera::VaapiJpegDecoder;
#endif

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
  const char* v4l2_encoder = nullptr;  // set => V4L2 M2M backend at this device
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

// Open + configure V4L2 MMAP capture at `fourcc`, returning the mmap'd buffers
// (streaming on). Returns false on any failure (message already logged).
bool start_capture(int fd, std::uint32_t& width, std::uint32_t& height, std::uint32_t fourcc,
                   std::vector<MappedBuf>& bufs) {
  v4l2_format fmt{};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = width;
  fmt.fmt.pix.height = height;
  fmt.fmt.pix.pixelformat = fourcc;
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
    drm::println(stderr, "VIDIOC_S_FMT: {}", std::strerror(errno));
    return false;
  }
  width = fmt.fmt.pix.width;
  height = fmt.fmt.pix.height;
  if (fmt.fmt.pix.pixelformat != fourcc) {
    drm::println(stderr, "camera does not offer the requested format at this size");
    return false;
  }

  v4l2_requestbuffers req{};
  req.count = k_buffer_count;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
    drm::println(stderr, "VIDIOC_REQBUFS: {}", std::strerror(errno));
    return false;
  }
  bufs.resize(req.count);
  for (unsigned int i = 0; i < req.count; ++i) {
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = i;
    if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
      drm::println(stderr, "VIDIOC_QUERYBUF: {}", std::strerror(errno));
      return false;
    }
    bufs[i].length = b.length;
    bufs[i].start = ::mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
    if (bufs[i].start == MAP_FAILED) {
      drm::println(stderr, "mmap: {}", std::strerror(errno));
      return false;
    }
    if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
      drm::println(stderr, "VIDIOC_QBUF: {}", std::strerror(errno));
      return false;
    }
  }
  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
    drm::println(stderr, "VIDIOC_STREAMON: {}", std::strerror(errno));
    return false;
  }
  return true;
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-size) — a throw here just aborts
int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (arg == "--device") {
      a.device = next();
    } else if (arg == "--out") {
      a.out = next();
    } else if (arg == "--v4l2-encoder") {
      a.v4l2_encoder = next();
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
                   "usage: camera_record [--device /dev/videoN] [--out FILE] [--frames N] "
                   "[--size WxH] [--fps N] [--qp N] [--v4l2-encoder /dev/videoN]");
      return EXIT_FAILURE;
    }
  }

  const bool use_v4l2 = a.v4l2_encoder != nullptr;
#if !CAMERA_HAS_VAAPI
  if (!use_v4l2) {
    drm::println(stderr,
                 "camera_record: built without libva — pass --v4l2-encoder /dev/videoN to use a "
                 "V4L2 M2M encoder (e.g. the Pi's bcm2835-codec)");
    return EXIT_FAILURE;
  }
#endif

  const int fd = ::open(a.device, O_RDWR);
  if (fd < 0) {
    drm::println(stderr, "open {}: {}", a.device, std::strerror(errno));
    return EXIT_FAILURE;
  }

  // V4L2 encoder wants raw YUYV; the VA-API path decodes MJPEG.
  const std::uint32_t cap_fourcc = use_v4l2 ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_MJPEG;
  std::vector<MappedBuf> bufs;
  if (!start_capture(fd, a.width, a.height, cap_fourcc, bufs)) {
    ::close(fd);
    return EXIT_FAILURE;
  }

  const std::unique_ptr<FILE, decltype(&std::fclose)> out(std::fopen(a.out, "wb"), &std::fclose);
  if (!out) {
    drm::println(stderr, "open {}: {}", a.out, std::strerror(errno));
    ::close(fd);
    return EXIT_FAILURE;
  }

  auto dq = [&](v4l2_buffer& b) {
    b = {};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    return xioctl(fd, VIDIOC_DQBUF, &b) == 0;
  };

  std::size_t total = 0;
  int encoded = 0;
  std::vector<std::uint8_t> bits;
  std::error_code ec;

  if (use_v4l2) {
    V4l2H264Encoder::Config cfg{};
    cfg.device = a.v4l2_encoder;
    cfg.in_fourcc = V4L2_PIX_FMT_YUYV;
    cfg.width = a.width;
    cfg.height = a.height;
    cfg.fps = a.fps;
    auto enc = V4l2H264Encoder::create(cfg, &ec);
    if (!enc) {
      drm::println(stderr, "v4l2 encoder create ({}): {}", a.v4l2_encoder, ec.message());
      return EXIT_FAILURE;
    }
    drm::println("recording {}x{} YUYV from {} @ {}fps -> {} (V4L2 {})", a.width, a.height,
                 a.device, a.fps, a.out, a.v4l2_encoder);
    for (int f = 0; f < a.frames; ++f) {
      v4l2_buffer b{};
      if (!dq(b)) {
        break;
      }
      bits.clear();
      if (enc->encode(static_cast<const std::uint8_t*>(bufs[b.index].start), b.bytesused, bits)) {
        std::fwrite(bits.data(), 1, bits.size(), out.get());
        total += bits.size();
        ++encoded;
      }
      xioctl(fd, VIDIOC_QBUF, &b);
    }
  }
#if CAMERA_HAS_VAAPI
  else {
    const int render_fd = ::open("/dev/dri/renderD128", O_RDWR);
    if (render_fd < 0) {
      drm::println(stderr, "open renderD128: {}", std::strerror(errno));
      return EXIT_FAILURE;
    }
    void* disp = VaapiJpegDecoder::open_display(render_fd, &ec);
    if (disp == nullptr) {
      drm::println(stderr, "open_display: {}", ec.message());
      return EXIT_FAILURE;
    }
    auto sampling = VaapiJpegDecoder::Sampling::Yuv420;
    auto decoder = VaapiJpegDecoder::create(disp, a.width, a.height, sampling, &ec);
    VaapiH264Encoder::Config cfg{};
    cfg.width = a.width;
    cfg.height = a.height;
    cfg.fps = a.fps;
    cfg.qp = a.qp;
    auto encoder = VaapiH264Encoder::create(disp, cfg, &ec);
    if (!decoder || !encoder) {
      drm::println(stderr, "vaapi decode/encode create: {}", ec.message());
      return EXIT_FAILURE;
    }
    drm::println("recording {}x{} MJPEG from {} @ {}fps -> {} (VA-API, qp {})", a.width, a.height,
                 a.device, a.fps, a.out, a.qp);
    for (int f = 0; f < a.frames; ++f) {
      v4l2_buffer b{};
      if (!dq(b)) {
        break;
      }
      const auto* jpeg = static_cast<const std::uint8_t*>(bufs[b.index].start);
      if (!decoder->decode_into_surface(jpeg, b.bytesused)) {
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
        std::fwrite(bits.data(), 1, bits.size(), out.get());
        total += bits.size();
        ++encoded;
      }
      xioctl(fd, VIDIOC_QBUF, &b);
    }
    VaapiJpegDecoder::close_display(disp);
    ::close(render_fd);
  }
#endif

  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  xioctl(fd, VIDIOC_STREAMOFF, &type);
  for (auto& mb : bufs) {
    if (mb.start != nullptr && mb.start != MAP_FAILED) {
      ::munmap(mb.start, mb.length);
    }
  }
  ::close(fd);
  drm::println("wrote {}: {} frames, {} bytes", a.out, encoded, total);
  return EXIT_SUCCESS;
}
