// SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "v4l2_h264_encoder.hpp"

#include <drm-cxx/detail/format.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <memory>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace drm::examples::camera {

namespace {

constexpr unsigned int k_buffer_count = 4;

int xioctl(int fd, unsigned long req, void* arg) noexcept {
  int r = 0;
  for (;;) {
    r = ::ioctl(fd, req, arg);
    if (r != -1 || errno != EINTR) {
      break;
    }
  }
  return r;
}

// Best-effort integer control; a device that lacks one keeps its default.
void set_ctrl(int fd, std::uint32_t id, std::int32_t value) noexcept {
  v4l2_control c{};
  c.id = id;
  c.value = value;
  (void)xioctl(fd, VIDIOC_S_CTRL, &c);
}

}  // namespace

std::unique_ptr<V4l2H264Encoder> V4l2H264Encoder::create(const Config& cfg, std::error_code* ec) {
  auto set_ec = [&](std::errc e) {
    if (ec != nullptr) {
      *ec = std::make_error_code(e);
    }
  };
  if (cfg.width == 0 || cfg.height == 0) {
    set_ec(std::errc::invalid_argument);
    return nullptr;
  }

  auto enc = std::unique_ptr<V4l2H264Encoder>(new V4l2H264Encoder());
  enc->width_ = cfg.width;
  enc->height_ = cfg.height;

  enc->fd_ = ::open(cfg.device, O_RDWR);
  if (enc->fd_ < 0) {
    drm::println(stderr, "[v4l2_h264_encoder] open {}: {}", cfg.device, std::strerror(errno));
    set_ec(std::errc::no_such_device);
    return nullptr;
  }
  const int fd = enc->fd_;
  const std::uint32_t in_fourcc = cfg.in_fourcc != 0 ? cfg.in_fourcc : V4L2_PIX_FMT_YUYV;

  // OUTPUT (raw frames) format.
  v4l2_format ofmt{};
  ofmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  ofmt.fmt.pix_mp.width = cfg.width;
  ofmt.fmt.pix_mp.height = cfg.height;
  ofmt.fmt.pix_mp.pixelformat = in_fourcc;
  ofmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
  ofmt.fmt.pix_mp.num_planes = 1;
  if (xioctl(fd, VIDIOC_S_FMT, &ofmt) < 0) {
    drm::println(stderr, "[v4l2_h264_encoder] S_FMT(OUTPUT): {}", std::strerror(errno));
    set_ec(std::errc::not_supported);
    enc->destroy_state();
    return nullptr;
  }
  enc->out_sizeimage_ = ofmt.fmt.pix_mp.plane_fmt[0].sizeimage;

  // CAPTURE (coded H.264) format.
  v4l2_format cfmt{};
  cfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  cfmt.fmt.pix_mp.width = cfg.width;
  cfmt.fmt.pix_mp.height = cfg.height;
  cfmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
  cfmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
  cfmt.fmt.pix_mp.num_planes = 1;
  cfmt.fmt.pix_mp.plane_fmt[0].sizeimage = std::max(512U * 1024U, cfg.width * cfg.height);
  if (xioctl(fd, VIDIOC_S_FMT, &cfmt) < 0) {
    drm::println(stderr, "[v4l2_h264_encoder] S_FMT(CAPTURE): {}", std::strerror(errno));
    set_ec(std::errc::not_supported);
    enc->destroy_state();
    return nullptr;
  }

  // Encode controls. Bitrate defaults to ~0.1 bits/pixel/frame, clamped to the
  // bcm2835 range; SPS/PPS repeat on every keyframe so the stream is seekable.
  const std::uint32_t fps = cfg.fps != 0 ? cfg.fps : 30;
  std::uint32_t bitrate = cfg.bitrate_bps;
  if (bitrate == 0) {
    bitrate = std::max(2000000U, (cfg.width * cfg.height * fps) / 10U);
  }
  bitrate = std::clamp(bitrate, 25000U, 25000000U);
  const std::uint32_t gop = cfg.gop != 0 ? cfg.gop : fps * 2;
  set_ctrl(fd, V4L2_CID_MPEG_VIDEO_BITRATE, static_cast<std::int32_t>(bitrate));
  set_ctrl(fd, V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE);
  set_ctrl(fd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, static_cast<std::int32_t>(gop));
  set_ctrl(fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, static_cast<std::int32_t>(gop));
  set_ctrl(fd, V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, 1);

  // OUTPUT buffers — raw frames fed by encode()/encode_dmabuf(), never displayed.
  // A dma-buf-import queue owns no memory (the caller's fd backs each QBUF), so
  // it skips QUERYBUF/mmap; if the driver rejects the DMABUF request outright,
  // fall back to MMAP so the copy path still works.
  const bool want_dmabuf = cfg.dmabuf_output;
  v4l2_requestbuffers oreq{};
  oreq.count = k_buffer_count;
  oreq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  oreq.memory = want_dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
  bool req_ok = xioctl(fd, VIDIOC_REQBUFS, &oreq) >= 0 && oreq.count != 0;
  if (!req_ok && want_dmabuf) {
    drm::println(stderr, "[v4l2_h264_encoder] REQBUFS(OUTPUT,DMABUF): {} — using MMAP",
                 std::strerror(errno));
    oreq = {};
    oreq.count = k_buffer_count;
    oreq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    oreq.memory = V4L2_MEMORY_MMAP;
    req_ok = xioctl(fd, VIDIOC_REQBUFS, &oreq) >= 0 && oreq.count != 0;
  }
  if (!req_ok) {
    drm::println(stderr, "[v4l2_h264_encoder] REQBUFS(OUTPUT): {}", std::strerror(errno));
    set_ec(std::errc::not_supported);
    enc->destroy_state();
    return nullptr;
  }
  enc->output_dmabuf_ = oreq.memory == V4L2_MEMORY_DMABUF;
  enc->out_count_ = oreq.count;
  if (!enc->output_dmabuf_) {
    enc->out_bufs_.resize(oreq.count);
    for (unsigned int i = 0; i < oreq.count; ++i) {
      v4l2_plane plane{};
      v4l2_buffer buf{};
      buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = i;
      buf.length = 1;
      buf.m.planes = &plane;
      if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
        set_ec(std::errc::not_supported);
        enc->destroy_state();
        return nullptr;
      }
      enc->out_bufs_[i].length = plane.length;
      void* p =
          ::mmap(nullptr, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, plane.m.mem_offset);
      if (p == MAP_FAILED) {
        set_ec(std::errc::not_supported);
        enc->destroy_state();
        return nullptr;
      }
      enc->out_bufs_[i].start = static_cast<std::uint8_t*>(p);
    }
  }

  // CAPTURE buffers (MMAP) — receive coded chunks; queued up front.
  v4l2_requestbuffers creq{};
  creq.count = k_buffer_count;
  creq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  creq.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd, VIDIOC_REQBUFS, &creq) < 0 || creq.count == 0) {
    drm::println(stderr, "[v4l2_h264_encoder] REQBUFS(CAPTURE): {}", std::strerror(errno));
    set_ec(std::errc::not_supported);
    enc->destroy_state();
    return nullptr;
  }
  enc->cap_bufs_.resize(creq.count);
  for (unsigned int i = 0; i < creq.count; ++i) {
    v4l2_plane plane{};
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    buf.length = 1;
    buf.m.planes = &plane;
    if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
      set_ec(std::errc::not_supported);
      enc->destroy_state();
      return nullptr;
    }
    enc->cap_bufs_[i].length = plane.length;
    void* p =
        ::mmap(nullptr, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, plane.m.mem_offset);
    if (p == MAP_FAILED) {
      set_ec(std::errc::not_supported);
      enc->destroy_state();
      return nullptr;
    }
    enc->cap_bufs_[i].start = static_cast<std::uint8_t*>(p);
    if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
      set_ec(std::errc::not_supported);
      enc->destroy_state();
      return nullptr;
    }
  }

  v4l2_buf_type ot = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  v4l2_buf_type ct = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (xioctl(fd, VIDIOC_STREAMON, &ot) < 0 || xioctl(fd, VIDIOC_STREAMON, &ct) < 0) {
    drm::println(stderr, "[v4l2_h264_encoder] STREAMON: {}", std::strerror(errno));
    set_ec(std::errc::not_supported);
    enc->destroy_state();
    return nullptr;
  }
  enc->streaming_ = true;
  return enc;
}

// NOLINTNEXTLINE(bugprone-exception-escape) — only std::vector growth can throw (OOM = abort)
bool V4l2H264Encoder::encode(const std::uint8_t* frame, std::size_t size,
                             std::vector<std::uint8_t>& out) noexcept {
  if (frame == nullptr || output_dmabuf_) {
    return false;
  }

  // Acquire a free OUTPUT buffer: use each once up front, then recycle the
  // kernel's completed ones.
  unsigned int idx = 0;
  v4l2_plane oplane{};
  v4l2_buffer obuf{};
  obuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  obuf.memory = V4L2_MEMORY_MMAP;
  obuf.length = 1;
  obuf.m.planes = &oplane;
  if (out_queued_ < out_count_) {
    idx = out_queued_++;
  } else {
    if (xioctl(fd_, VIDIOC_DQBUF, &obuf) < 0) {
      drm::println(stderr, "[v4l2_h264_encoder] DQBUF(OUTPUT): {}", std::strerror(errno));
      return false;
    }
    idx = obuf.index;
  }

  const std::size_t n = std::min(size, out_bufs_[idx].length);
  std::memcpy(out_bufs_[idx].start, frame, n);

  v4l2_plane qplane{};
  qplane.bytesused = static_cast<std::uint32_t>(n);
  qplane.length = static_cast<std::uint32_t>(out_bufs_[idx].length);
  v4l2_buffer qbuf{};
  qbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  qbuf.memory = V4L2_MEMORY_MMAP;
  qbuf.index = idx;
  qbuf.length = 1;
  qbuf.m.planes = &qplane;
  if (xioctl(fd_, VIDIOC_QBUF, &qbuf) < 0) {
    drm::println(stderr, "[v4l2_h264_encoder] QBUF(OUTPUT): {}", std::strerror(errno));
    return false;
  }
  return drain_capture(out);
}

// NOLINTNEXTLINE(bugprone-exception-escape) — only std::vector growth can throw (OOM = abort)
bool V4l2H264Encoder::encode_dmabuf(int dmabuf_fd, std::size_t data_offset,
                                    std::vector<std::uint8_t>& out) noexcept {
  if (dmabuf_fd < 0 || !output_dmabuf_) {
    return false;
  }

  // Acquire a free OUTPUT slot: use each once up front, then recycle the
  // kernel's completed ones. The slot carries no memory — the caller's fd does.
  unsigned int idx = 0;
  v4l2_plane oplane{};
  v4l2_buffer obuf{};
  obuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  obuf.memory = V4L2_MEMORY_DMABUF;
  obuf.length = 1;
  obuf.m.planes = &oplane;
  if (out_queued_ < out_count_) {
    idx = out_queued_++;
  } else {
    if (xioctl(fd_, VIDIOC_DQBUF, &obuf) < 0) {
      drm::println(stderr, "[v4l2_h264_encoder] DQBUF(OUTPUT,DMABUF): {}", std::strerror(errno));
      return false;
    }
    idx = obuf.index;
  }

  // Import the caller's frame by fd — no copy. `data_offset` locates the first
  // (Y) plane; a single-fd NV12 buffer's UV follows contiguously, matching the
  // single-plane OUTPUT format the encoder negotiated.
  v4l2_plane qplane{};
  qplane.bytesused = out_sizeimage_;
  qplane.length = out_sizeimage_;
  qplane.data_offset = static_cast<std::uint32_t>(data_offset);
  qplane.m.fd = dmabuf_fd;
  v4l2_buffer qbuf{};
  qbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  qbuf.memory = V4L2_MEMORY_DMABUF;
  qbuf.index = idx;
  qbuf.length = 1;
  qbuf.m.planes = &qplane;
  if (xioctl(fd_, VIDIOC_QBUF, &qbuf) < 0) {
    drm::println(stderr, "[v4l2_h264_encoder] QBUF(OUTPUT,DMABUF): {}", std::strerror(errno));
    return false;
  }
  return drain_capture(out);
}

// NOLINTNEXTLINE(bugprone-exception-escape) — only std::vector growth can throw (OOM = abort)
bool V4l2H264Encoder::drain_capture(std::vector<std::uint8_t>& out) noexcept {
  // Dequeue the coded frame (baseline is 1-in-1-out) and requeue the buffer.
  v4l2_plane cplane{};
  v4l2_buffer cbuf{};
  cbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  cbuf.memory = V4L2_MEMORY_MMAP;
  cbuf.length = 1;
  cbuf.m.planes = &cplane;
  if (xioctl(fd_, VIDIOC_DQBUF, &cbuf) < 0) {
    drm::println(stderr, "[v4l2_h264_encoder] DQBUF(CAPTURE): {}", std::strerror(errno));
    return false;
  }
  const auto* p = cap_bufs_[cbuf.index].start;
  out.insert(out.end(), p, p + cplane.bytesused);

  v4l2_plane rplane{};
  v4l2_buffer rbuf{};
  rbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  rbuf.memory = V4L2_MEMORY_MMAP;
  rbuf.index = cbuf.index;
  rbuf.length = 1;
  rbuf.m.planes = &rplane;
  (void)xioctl(fd_, VIDIOC_QBUF, &rbuf);
  return true;
}

void V4l2H264Encoder::destroy_state() noexcept {
  if (fd_ >= 0) {
    if (streaming_) {
      v4l2_buf_type ot = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
      v4l2_buf_type ct = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      (void)xioctl(fd_, VIDIOC_STREAMOFF, &ot);
      (void)xioctl(fd_, VIDIOC_STREAMOFF, &ct);
      streaming_ = false;
    }
    for (auto& b : out_bufs_) {
      if (b.start != nullptr) {
        ::munmap(b.start, b.length);
      }
    }
    for (auto& b : cap_bufs_) {
      if (b.start != nullptr) {
        ::munmap(b.start, b.length);
      }
    }
    out_bufs_.clear();
    cap_bufs_.clear();
    ::close(fd_);
    fd_ = -1;
  }
}

V4l2H264Encoder::~V4l2H264Encoder() {
  destroy_state();
}

}  // namespace drm::examples::camera
